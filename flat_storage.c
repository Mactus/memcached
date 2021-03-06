/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/**
 * The flat allocator mmaps the entire region as limited by settings.maxbytes.
 * When first started, we do not initialize the entire region to prevent
 * unnecessary page allocation.  As we need additional memory, we will
 * initialize (and page in) additional memory.
 */

#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "generic.h"

#if defined(USE_FLAT_ALLOCATOR)

#define FLAT_STORAGE_MODULE

#include "assoc.h"
#include "flat_storage.h"
#include "memcached.h"
#include "stats.h"

typedef enum {
    COALESCE_NO_PROGRESS,               /* no progress was made in coalescing a block */
    COALESCE_LARGE_CHUNK_FORMED,        /* a large chunk was formed */
    COALESCE_FORWARD_PROGRESS,          /* a large chunk was not formed, but
                                         * forward progress was made. */
} coalesce_progress_t;


flat_storage_info_t fsi;


/** forward declarations */
static void free_list_push(chunk_t* chunk, chunk_type_t chunk_type, bool try_merge);
static chunk_t* free_list_pop(chunk_type_t chunk_type);
static void break_large_chunk(chunk_t* chunk);
static void unbreak_large_chunk(large_chunk_t* lc, bool mandatory);
static void item_free(item *it);


/**
 * flat storage code
 */
void flat_storage_init(size_t maxbytes) {
    intptr_t addr;

    always_assert(fsi.initialized == false);
    always_assert(maxbytes % LARGE_CHUNK_SZ == 0);
    always_assert(maxbytes % FLAT_STORAGE_INCREMENT_DELTA == 0);
    fsi.mmap_start = mmap(NULL,
                          maxbytes + LARGE_CHUNK_SZ - 1, /* alloc extra to
                                                          * ensure we can align
                                                          * our buffers. */
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON,
                          -1,
                          0);
    if (fsi.mmap_start == MAP_FAILED) {
        fprintf(stderr, "failed to mmap memory\n");
        exit(EXIT_FAILURE);
    }

    /* ensure the alignment of mmap'ed region. */
    addr = (intptr_t) fsi.mmap_start;
    addr = ((addr + LARGE_CHUNK_SZ - 1) / LARGE_CHUNK_SZ) * LARGE_CHUNK_SZ;
    fsi.flat_storage_start = (void*) addr;
    fsi.uninitialized_start = fsi.flat_storage_start;
    fsi.unused_memory = maxbytes;

    fsi.large_free_list = NULL_CHUNKPTR;
    fsi.large_free_list_sz = 0;
    fsi.small_free_list = NULL_CHUNKPTR;
    fsi.small_free_list_sz = 0;
    fsi.lru_head = NULL_CHUNKPTR;
    fsi.lru_tail = NULL_CHUNKPTR;

    /* shouldn't fail here.... right? */
    flat_storage_alloc();
    always_assert(fsi.large_free_list_sz != 0);

    fsi.initialized = 1;
}

void item_init(void) {
    /* run a bunch of always_asserts to make sure that there are no inconsistent or
     * inherently wrong implementation details. */
    always_assert(LARGE_CHUNK_SZ >= SMALL_CHUNK_SZ);
    always_assert(LARGE_CHUNK_SZ >= CHUNK_ADDRESSING_SZ);
    always_assert( (LARGE_CHUNK_SZ % CHUNK_ADDRESSING_SZ) == 0 );
    always_assert( (LARGE_CHUNK_SZ / SMALL_CHUNK_SZ) <=
                   (LARGE_CHUNK_SZ / CHUNK_ADDRESSING_SZ) );
    always_assert(LARGE_TITLE_CHUNK_DATA_SZ >= KEY_MAX_LENGTH);
    always_assert(SMALL_CHUNKS_PER_LARGE_CHUNK >= 2);

    /* make sure that the size of the structure is what they're supposed to be. */
    always_assert(sizeof(large_chunk_t) == LARGE_CHUNK_SZ);
    always_assert(sizeof(large_title_chunk_t) + LARGE_CHUNK_TAIL_SZ == LARGE_CHUNK_SZ);
    always_assert(sizeof(large_body_chunk_t) + LARGE_CHUNK_TAIL_SZ == LARGE_CHUNK_SZ);
    always_assert(sizeof(small_chunk_t) == SMALL_CHUNK_SZ);
    always_assert(sizeof(small_title_chunk_t) + SMALL_CHUNK_TAIL_SZ == SMALL_CHUNK_SZ);
    always_assert(sizeof(small_body_chunk_t) + SMALL_CHUNK_TAIL_SZ == SMALL_CHUNK_SZ);

    /* make sure that the fields line up in item */
    always_assert( &(((item*) 0)->empty_header.h_next) == &(((item*) 0)->large_title.h_next) );
    always_assert( &(((item*) 0)->empty_header.h_next) == &(((item*) 0)->small_title.h_next) );
    always_assert( &(((item*) 0)->empty_header.next) == &(((item*) 0)->large_title.next) );
    always_assert( &(((item*) 0)->empty_header.next) == &(((item*) 0)->small_title.next) );
    always_assert( &(((item*) 0)->empty_header.prev) == &(((item*) 0)->large_title.prev) );
    always_assert( &(((item*) 0)->empty_header.prev) == &(((item*) 0)->small_title.prev) );
    always_assert( &(((item*) 0)->empty_header.next_chunk) == &(((item*) 0)->large_title.next_chunk) );
    always_assert( &(((item*) 0)->empty_header.next_chunk) == &(((item*) 0)->small_title.next_chunk) );
    always_assert( &(((item*) 0)->empty_header.time) == &(((item*) 0)->large_title.time) );
    always_assert( &(((item*) 0)->empty_header.time) == &(((item*) 0)->small_title.time) );
    always_assert( &(((item*) 0)->empty_header.exptime) == &(((item*) 0)->large_title.exptime) );
    always_assert( &(((item*) 0)->empty_header.exptime) == &(((item*) 0)->small_title.exptime) );
    always_assert( &(((item*) 0)->empty_header.nbytes) == &(((item*) 0)->large_title.nbytes) );
    always_assert( &(((item*) 0)->empty_header.nbytes) == &(((item*) 0)->small_title.nbytes) );
    always_assert( &(((item*) 0)->empty_header.refcount) == &(((item*) 0)->large_title.refcount) );
    always_assert( &(((item*) 0)->empty_header.refcount) == &(((item*) 0)->small_title.refcount) );
    always_assert( &(((item*) 0)->empty_header.nkey) == &(((item*) 0)->large_title.nkey) );
    always_assert( &(((item*) 0)->empty_header.nkey) == &(((item*) 0)->small_title.nkey) );

    /* make sure that the casting functions in flat_storage.h are sane. */
    always_assert( (void*) &(((item*) 0)->small_title) == ((void*) 0));
    always_assert( (void*) &(((item*) 0)->large_title) == ((void*) 0));

    /* ensure that the first piece of a broken large chunk is the same address
     * as the large_title_chunk / large_body_chunk.  this is so that the
     * chunkptrs map correctly. */
    always_assert( (intptr_t) &(((large_chunk_t*) 0)->lc_broken.lbc[0]) ==
            (intptr_t) &(((large_chunk_t*) 0)->lc_title) );

    always_assert(FLAT_STORAGE_INCREMENT_DELTA % LARGE_CHUNK_SZ == 0);
}


/* initialize at least nbytes more memory and add them as large chunks to the
 * free list. */
FA_STATIC bool flat_storage_alloc(void) {
    stats_t *stats = STATS_GET_TLS();
    large_chunk_t* initialize_end;

    if (FLAT_STORAGE_INCREMENT_DELTA > fsi.unused_memory) {
        return false;
    }

    initialize_end = fsi.uninitialized_start + (FLAT_STORAGE_INCREMENT_DELTA / LARGE_CHUNK_SZ);
    STATS_LOCK(stats);
    stats->item_storage_allocated += FLAT_STORAGE_INCREMENT_DELTA;
    STATS_UNLOCK(stats);
    /* initialize the large chunks. */
    for (;
         fsi.uninitialized_start < initialize_end;
         fsi.unused_memory -= LARGE_CHUNK_SZ) {
        large_chunk_t* chunk = fsi.uninitialized_start;
        fsi.uninitialized_start ++;
        chunk->flags = LARGE_CHUNK_INITIALIZED;
        free_list_push( (chunk_t*) chunk, LARGE_CHUNK, false);
    }

    return true;
}


/* pushes the chunk onto the free list.  the chunk must be initialized but not
 * used.  afterwards, the flags will be set to INITIALIZED | FREE.  if
 * try_merge is true and chunk_type is SMALL_CHUNK, it will try to coalesce the
 * parent block. */
static void free_list_push(chunk_t* chunk, chunk_type_t chunk_type,
                           bool try_merge) {
    /* can't merge large chunks */
    assert(! (try_merge == true && chunk_type == LARGE_CHUNK) );

    switch (chunk_type) {
        case SMALL_CHUNK:
        {
            large_chunk_t* pc;

            assert( SMALL_CHUNK_INITIALIZED ==
                    chunk->sc.flags );

            /* adjust the allocated count for the parent chunk */
            pc = get_parent_chunk(&(chunk->sc));
            assert(pc->lc_broken.small_chunks_allocated > 0);
            fsi.stats.broken_chunk_histogram[pc->lc_broken.small_chunks_allocated] --; /* STATS: update */
            pc->lc_broken.small_chunks_allocated --;
            fsi.stats.broken_chunk_histogram[pc->lc_broken.small_chunks_allocated] ++; /* STATS: update */

            /* add ourselves to the free list.
             *
             * ttung NOTE: if small chunk fragmentation is severe (i.e., we
             * spend too much time migrating data in
             * coalesce_free_small_chunks(..)), then we can employ strategies
             * to keep small chunks with its siblings on the free list. */
            if (fsi.small_free_list != NULL_CHUNKPTR) {
                chunk_t* old_head;
                old_head = (chunk_t*) fsi.small_free_list;
                old_head->sc.sc_free.prev_next = &chunk->sc.sc_free.next;
                chunk->sc.sc_free.next = fsi.small_free_list;
            } else {
                chunk->sc.sc_free.next = NULL_CHUNKPTR;
            }
            chunk->sc.sc_free.prev_next = &fsi.small_free_list;
            fsi.small_free_list = &(chunk->sc);
            fsi.small_free_list_sz ++;

            chunk->sc.flags = (SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_FREE);

            if (try_merge) {
                unbreak_large_chunk(pc, false);
            }
        }

            break;

        case LARGE_CHUNK:
            assert( LARGE_CHUNK_INITIALIZED ==
                    chunk->lc.flags );
            if (fsi.large_free_list != NULL_CHUNKPTR) {
                chunk_t* old_head;
                old_head = (chunk_t*) fsi.large_free_list;
                chunk->lc.lc_free.next = fsi.large_free_list;
            } else {
                chunk->lc.lc_free.next = NULL_CHUNKPTR;
            }
            fsi.large_free_list = &(chunk->lc);
            fsi.large_free_list_sz ++;

            chunk->lc.flags = (LARGE_CHUNK_INITIALIZED | LARGE_CHUNK_FREE);
            break;
    }

}


/* gets the first chunk from the free list.  if no chunk is available, then
 * NULL is returned. */
static chunk_t* free_list_pop(chunk_type_t chunk_type) {
    chunk_t* retval;

    switch (chunk_type) {
        case SMALL_CHUNK:
        {
            large_chunk_t* parent_chunk;

            if (fsi.small_free_list_sz == 0) {
                return NULL;
            }
            retval = (chunk_t*) fsi.small_free_list;
            parent_chunk = get_parent_chunk(&(retval->sc));
            fsi.stats.broken_chunk_histogram[parent_chunk->lc_broken.small_chunks_allocated] --; /* STATS: update */
            parent_chunk->lc_broken.small_chunks_allocated ++;
            fsi.stats.broken_chunk_histogram[parent_chunk->lc_broken.small_chunks_allocated] ++; /* STATS: update */
            assert(parent_chunk->lc_broken.small_chunks_allocated <= SMALL_CHUNKS_PER_LARGE_CHUNK);

            /* remove ourselves from the free list. */
            fsi.small_free_list = retval->sc.sc_free.next;
            if (fsi.small_free_list != NULL_CHUNKPTR) {
                chunk_t* new_head;

                new_head = (chunk_t*) fsi.small_free_list;
                new_head->sc.sc_free.prev_next = &fsi.small_free_list;
            }
            fsi.small_free_list_sz --;

            /* do some sanity checks on the chunk */
            assert( (SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_FREE) ==
                    retval->sc.flags );

            /* unmark free flag */
            retval->sc.flags &= (~SMALL_CHUNK_FREE);
            return retval;
        }

        case LARGE_CHUNK:
            if (fsi.large_free_list_sz == 0) {
                return NULL;
            }
            retval = (chunk_t*) fsi.large_free_list;
            fsi.large_free_list = retval->lc.lc_free.next;
            if (fsi.large_free_list != NULL_CHUNKPTR) {
                chunk_t* new_head;

                new_head = (chunk_t*) fsi.large_free_list;
            }
            fsi.large_free_list_sz --;

            /* do some sanity checks on the chunk */
            assert( (LARGE_CHUNK_INITIALIZED | LARGE_CHUNK_FREE) ==
                    retval->lc.flags );
            /* unmark free flag */
            retval->lc.flags &= (~LARGE_CHUNK_FREE);
            return retval;

    }

    return NULL;
}


/* this takes an unused large chunk and breaks it into small chunks.  it adds
 * the small chunks to the small chunk free list.
 */
static void break_large_chunk(chunk_t* chunk) {
    int i;

    assert( LARGE_CHUNK_INITIALIZED == chunk->lc.flags );
    chunk->lc.flags |= LARGE_CHUNK_USED | LARGE_CHUNK_BROKEN;

    /* this is a dummy kludge to ensure that free_list_push can go ahead and
     * decrement the allocated count safely. */
    chunk->lc.lc_broken.small_chunks_allocated = SMALL_CHUNKS_PER_LARGE_CHUNK;
    fsi.stats.broken_chunk_histogram[SMALL_CHUNKS_PER_LARGE_CHUNK] ++; /* STATS: update */

    for (i = SMALL_CHUNKS_PER_LARGE_CHUNK - 1; i >= 0; i --) {
        small_chunk_t* small_chunk = &(chunk->lc.lc_broken.lbc[i]);
        small_chunk->flags = SMALL_CHUNK_INITIALIZED;
        free_list_push( (chunk_t*) small_chunk, SMALL_CHUNK, false);
    }

    chunk->lc.lc_broken.small_chunks_allocated = 0;

    /* STATS: update */
    fsi.stats.large_broken_chunks ++;
    fsi.stats.break_events ++;
}


/* take a large broken chunk and unbreak it.
 */
static void unbreak_large_chunk(large_chunk_t* lc, bool mandatory) {
    int i;

    assert( (LARGE_CHUNK_INITIALIZED | LARGE_CHUNK_USED | LARGE_CHUNK_BROKEN) ==
            lc->flags );

    /* if the coalesce is not mandatory, then make sure there are no used
     * chunks. */
    if (! mandatory) {
        if (lc->lc_broken.small_chunks_allocated != 0) {
            return;
        }
#if !defined(NDEBUG)
        for (i = 0; i < SMALL_CHUNKS_PER_LARGE_CHUNK; i ++) {
            small_chunk_t* small_chunk = &(lc->lc_broken.lbc[i]);

            assert(small_chunk->flags & SMALL_CHUNK_INITIALIZED);
            assert((small_chunk->flags & SMALL_CHUNK_USED) == 0);
        }
#endif /* #if !defined(NDEBUG) */
    } else {
        assert(lc->lc_broken.small_chunks_allocated == 0);
    }

    for (i = 0; i < SMALL_CHUNKS_PER_LARGE_CHUNK; i ++) {
        small_chunk_t* small_chunk = &(lc->lc_broken.lbc[i]);
        small_chunk_t** prev_next;

        /* some sanity checks */
        assert(small_chunk->flags == (SMALL_CHUNK_INITIALIZED |
                                      SMALL_CHUNK_FREE) ||
               small_chunk->flags == (SMALL_CHUNK_INITIALIZED |
                                      SMALL_CHUNK_COALESCE_PENDING));

        /* remove this chunk from the free list */
        if (small_chunk->flags & SMALL_CHUNK_FREE) {
            prev_next = small_chunk->sc_free.prev_next;
            assert(*prev_next == small_chunk);
            *(prev_next) = small_chunk->sc_free.next;

            if (small_chunk->sc_free.next != NULL_CHUNKPTR) {
                small_chunk_t* next = small_chunk->sc_free.next;
                next->sc_free.prev_next = prev_next;
            }

            small_chunk->flags = 0;

            fsi.small_free_list_sz --;
        }
    }

    lc->flags = LARGE_CHUNK_INITIALIZED;
    free_list_push( (chunk_t*) lc, LARGE_CHUNK, false);

    /* STATS: update */
    fsi.stats.large_broken_chunks --;
    fsi.stats.broken_chunk_histogram[0] --;
    fsi.stats.unbreak_events ++;
}


/*
 * gets the oldest item on the LRU with refcount == 0.
 */
FA_STATIC item* get_lru_item(void) {
    int i;
    item* iter, * prev;

    for (i = 0,
             iter = fsi.lru_tail;
         i < LRU_SEARCH_DEPTH && iter != NULL_CHUNKPTR;
         i ++, iter = prev) {
        /* large chunk */
        if (iter->empty_header.refcount == 0) {
            return iter;
        }

        prev = get_item_from_chunk(get_chunk_address(iter->empty_header.prev));
    }

    return NULL;
}


static bool small_chunk_referenced(const small_chunk_t* sc) {
    assert((sc->flags & SMALL_CHUNK_INITIALIZED) != 0);
    if (sc->flags & SMALL_CHUNK_FREE) {
        return false;                   /* free nodes count as refcount = 0. */
    } else {
        for (;
            (sc->flags & SMALL_CHUNK_TITLE) == 0;
             sc = &get_chunk_address(sc->sc_body.prev_chunk)->sc) {
            assert((sc->flags & (SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_USED)) ==
                   (SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_USED));
        }

        assert((sc->flags & (SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_USED | SMALL_CHUNK_TITLE)) ==
               (SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_USED | SMALL_CHUNK_TITLE));
        return (sc->sc_title.refcount == 0) ? false : true;
    }
}


static bool large_broken_chunk_referenced(const large_broken_chunk_t* lc) {
    unsigned counter;

    for (counter = 0;
         counter < SMALL_CHUNKS_PER_LARGE_CHUNK;
         counter ++) {
        const small_chunk_t* iter = &(lc->lbc[counter]);

        if (small_chunk_referenced(iter)) {
            return true;
        }
    }

    return false;
}


/*
 * if search_depth is zero, then the search depth is not limited.  if the search
 * depth is non-zero, constrain search to the first search_depth items on the
 * small free list
 */
static large_chunk_t* find_unreferenced_broken_chunk(size_t search_depth) {
    small_chunk_t* small_chunk_iter;
    unsigned counter;

    for (counter = 0,
             small_chunk_iter = fsi.small_free_list;
         small_chunk_iter != NULL && (search_depth == 0 || counter < search_depth);
         counter ++,
             small_chunk_iter = small_chunk_iter->sc_free.next) {
        large_chunk_t* lc = get_parent_chunk(small_chunk_iter);
        large_broken_chunk_t* pc = &(lc->lc_broken);

        if (large_broken_chunk_referenced(pc) == false) {
            return lc;
        }
    }

    return NULL;
}


/*
 * coalesce as many small free chunks as we can find to form large free chunks.
 * two things are needed to perform this operation:
 * 1) at least SMALL_CHUNKS_PER_LARGE_CHUNK free small chunks.
 * 2) a large broken chunk that has refcount == 0 so we can move items off of it.
 *
 * returns COALESCE_NO_PROGRESS if no forward progress was made.
 *         COALESCE_LARGE_CHUNK_FORMED if large chunks was formed.
 */
static coalesce_progress_t coalesce_free_small_chunks(void) {
    coalesce_progress_t retval = COALESCE_NO_PROGRESS;

    while (fsi.small_free_list_sz >= SMALL_CHUNKS_PER_LARGE_CHUNK) {
        large_chunk_t* lc;
        unsigned i;

        lc = find_unreferenced_broken_chunk(0);
        if (lc == NULL) {
            /* we don't want to be stuck in an infinite loop if we can't find a
             * large unreferenced chunk, so just report no progress. */
            return retval;
        }

        /* STATS: update */
        fsi.stats.broken_chunk_histogram[lc->lc_broken.small_chunks_allocated] --;
        fsi.stats.migrates += lc->lc_broken.small_chunks_allocated;

        if (lc->lc_broken.small_chunks_allocated != 0) {
            /* any free small chunks that belong to the same parent chunk should be
             * removed from the free list.  this is to ensure that we don't pick them
             * up as replacement blocks. */
            for (i = 0; i < SMALL_CHUNKS_PER_LARGE_CHUNK; i ++) {
                small_chunk_t* iter = &(lc->lc_broken.lbc[i]);

                if (iter->flags & SMALL_CHUNK_FREE) {
                    /* need to remove this from the free list */
                    small_chunk_t** prev_next;

                    prev_next = iter->sc_free.prev_next;
                    assert(*prev_next == iter);
                    *(prev_next) = iter->sc_free.next;

                    if (iter->sc_free.next != NULL_CHUNKPTR) {
                        small_chunk_t* next = iter->sc_free.next;
                        next->sc_free.prev_next = prev_next;
                    }

                    iter->flags &= ~(SMALL_CHUNK_FREE);
                    iter->flags |= SMALL_CHUNK_COALESCE_PENDING;

                    fsi.small_free_list_sz --;
                }
            }

            for (i = 0; i < SMALL_CHUNKS_PER_LARGE_CHUNK; i ++) {
                small_chunk_t* iter = &(lc->lc_broken.lbc[i]);
                chunk_t* old_chunk = (chunk_t*) iter;
                (void) old_chunk;           /* when optimizing, old_chunk is not
                                             * used.  this is to quiesce the
                                             * compiler warning. */

                assert( (iter->flags & SMALL_CHUNK_INITIALIZED) ==
                        SMALL_CHUNK_INITIALIZED);

                if (iter->flags & SMALL_CHUNK_USED) {
                    /* title block */
                    chunk_t* _replacement = free_list_pop(SMALL_CHUNK);
                    small_chunk_t* replacement;
                    chunkptr_t replacement_chunkptr;
                    assert(_replacement != NULL);

                    replacement = &(_replacement->sc);
                    assert(replacement->flags == (SMALL_CHUNK_INITIALIZED));
                    memcpy(replacement, iter, sizeof(small_chunk_t));
                    replacement_chunkptr = get_chunkptr(_replacement);

                    if (iter->flags & SMALL_CHUNK_TITLE) {
                        item* new_it, * old_it;
                        chunk_t* next, * prev;
                        small_chunk_t* next_chunk;

                        new_it = get_item_from_small_title(&(replacement->sc_title));
                        old_it = get_item_from_small_title(&(iter->sc_title));

                        /* edit the forward and backward links. */
                        if (replacement->sc_title.next != NULL_CHUNKPTR) {
                            next = get_chunk_address(replacement->sc_title.next);
                            assert(next->sc.sc_title.prev == get_chunkptr(old_chunk));
                            next->sc.sc_title.prev = replacement_chunkptr;
                        } else {
                            assert(fsi.lru_tail == get_item_from_small_title(&old_chunk->sc.sc_title));
                            fsi.lru_tail = get_item_from_small_title(&replacement->sc_title);
                        }

                        if (replacement->sc_title.prev != NULL_CHUNKPTR) {
                            prev = get_chunk_address(replacement->sc_title.prev);
                            assert(prev->sc.sc_title.next == get_chunkptr(old_chunk));
                            prev->sc.sc_title.next = replacement_chunkptr;
                        } else {
                            assert(fsi.lru_head == get_item_from_small_title(&old_chunk->sc.sc_title));
                            fsi.lru_head = get_item_from_small_title(&replacement->sc_title);
                        }

                        /* edit the next_chunk's prev_chunk link */
                        next_chunk = &(get_chunk_address(replacement->sc_title.next_chunk))->sc;
                        if (next_chunk != NULL) {
                            assert(next_chunk->sc_body.prev_chunk == get_chunkptr(old_chunk));
                            next_chunk->sc_body.prev_chunk = replacement_chunkptr;
                        }

                        /* update flags */
                        replacement->flags |= (SMALL_CHUNK_USED | SMALL_CHUNK_TITLE);

                        /* do the replacement in the mapping. */
                        assoc_update(old_it, new_it);
                    } else {
                        /* body block.  this is more straightforward */
                        small_chunk_t* prev_chunk = &(get_chunk_address(replacement->sc_body.prev_chunk))->sc;
                        small_chunk_t* next_chunk = &(get_chunk_address(replacement->sc_body.next_chunk))->sc;

                        /* update the previous block's next pointer */
                        if (prev_chunk->flags & SMALL_CHUNK_TITLE) {
                            prev_chunk->sc_title.next_chunk = replacement_chunkptr;
                        } else {
                            prev_chunk->sc_body.next_chunk = replacement_chunkptr;
                        }

                        /* edit the next_chunk's prev_chunk link */
                        if (next_chunk != NULL) {
                            assert(next_chunk->sc_body.prev_chunk == get_chunkptr(old_chunk));
                            next_chunk->sc_body.prev_chunk = replacement_chunkptr;
                        }

                        /* update flags */
                        replacement->flags |= (SMALL_CHUNK_USED);
                    }

                    /* don't push this onto the free list.  if we do, we'll immediately
                     * pick it up when finding a replacement block.  instead, just mark
                     * it coalesce-pending. */
                    iter->flags = SMALL_CHUNK_INITIALIZED | SMALL_CHUNK_COALESCE_PENDING;

                    /* decrement the number of blocks allocated */
                    lc->lc_broken.small_chunks_allocated --;
                }
            }
        }

        /* STATS: update */
        fsi.stats.broken_chunk_histogram[0] ++;

        unbreak_large_chunk(lc, true);

        retval = COALESCE_LARGE_CHUNK_FORMED;
    }

    return retval;
}


static bool flat_storage_lru_evict(chunk_type_t chunk_type, size_t nchunks) {
    while (1) {
        /* release one item from the LRU... */
        item* lru_item;

        lru_item = get_lru_item();
        if (lru_item == NULL) {
            /* nothing to release, so we just fail. */
            return false;
        }
        do_item_unlink(lru_item, UNLINK_MAYBE_EVICT, NULL);

        /* do we have enough free chunks to leave this loop? */
        switch (chunk_type) {
            case SMALL_CHUNK:
                /* this is easier.  if we numerically have enough chunks, then
                 * we pass.  the caller will break the large chunks as
                 * necessary. */
                if (((fsi.large_free_list_sz * SMALL_CHUNKS_PER_LARGE_CHUNK) +
                     fsi.small_free_list_sz) >= nchunks) {
                    return true;
                }
                break;

            case LARGE_CHUNK:
                /* this is, not surprisingly, more complicated.  if we have
                 * sufficient large chunks, pass immediately.  if we have
                 * sufficient space, we can try a coalesce.  if that succeeds,
                 * we can check again to see if we have enough. */
                if (fsi.large_free_list_sz >= nchunks) {
                    return true;
                }

                if (((fsi.large_free_list_sz * SMALL_CHUNKS_PER_LARGE_CHUNK) +
                     fsi.small_free_list_sz) >= (nchunks * SMALL_CHUNKS_PER_LARGE_CHUNK)) {
                    /* try a coalesce */
                    if (coalesce_free_small_chunks() == COALESCE_NO_PROGRESS) {
                        continue;
                    }

                    /* we made progress, do we have what we need? */
                    if (fsi.large_free_list_sz >= nchunks) {
                        return true;
                    }
                }
                break;
        }
    }
}


static int do_stamp_on_block(char* block_start, size_t block_offset, size_t block_sz,
                             const rel_time_t now, const struct in_addr addr) {
    int retflags = 0;

    assert(block_offset <= block_sz);

    if (block_sz - block_offset >= sizeof(now)) {
        memcpy(&block_start[block_offset], &now, sizeof(now));
        retflags |= ITEM_HAS_TIMESTAMP;
        block_offset += sizeof(now);
    }

    if (block_sz - block_offset >= sizeof(addr)) {
        memcpy(&block_start[block_offset], &addr, sizeof(addr));
        retflags |= ITEM_HAS_IP_ADDRESS;
        block_offset += sizeof(addr);
    }

    assert(block_offset <= block_sz);

    return retflags;
}


void item_memcpy_to(item* it, size_t offset, const void* src, size_t nbytes,
                    bool beyond_item_boundary) {
#define MEMCPY_TO_APPLIER(it, ptr, bytes)       \
    memcpy((ptr), src, bytes);                  \
    src += bytes;

    ITEM_WALK(it, it->empty_header.nkey + offset, nbytes, beyond_item_boundary, MEMCPY_TO_APPLIER, );
#undef MEMCPY_TO_APPLIER
}


void item_memcpy_from(void* dst, const item* it, size_t offset, size_t nbytes,
                      bool beyond_item_boundary) {
#define MEMCPY_FROM_APPLIER(it, ptr, bytes)     \
    memcpy(dst, (ptr), bytes);                  \
    dst += bytes;

    ITEM_WALK(it, it->empty_header.nkey + offset, nbytes, beyond_item_boundary, MEMCPY_FROM_APPLIER, const);

#undef MEMCPY_FROM_APPLIER
}


int item_key_compare(const item* it, const char* key, const size_t nkey) {
    if (nkey != it->empty_header.nkey) {
        return it->empty_header.nkey - nkey;
    }

#define ITEM_KEY_COMPARE_APPLIER(it, ptr, bytes)        \
    do {                                                \
        int retval;                                     \
                                                        \
        if ((retval = memcmp(ptr, key, bytes)) != 0) {  \
            return retval;                              \
        }                                               \
                                                        \
        key += bytes;                                   \
    } while (0);

    ITEM_WALK(it, 0, nkey, 0, ITEM_KEY_COMPARE_APPLIER, const);
#undef ITEM_KEY_COMPARE_APPLIER

    return 0;
}


void do_try_item_stamp(item* it, rel_time_t now, const struct in_addr addr) {
    int slack;
    size_t offset = 0;

    it->empty_header.it_flags &= ~(ITEM_HAS_TIMESTAMP | ITEM_HAS_IP_ADDRESS);

    slack = item_slackspace(it);

    /* timestamp gets priority */
    if (slack >= sizeof(now)) {
        rel_time_t now = current_time;

        item_memcpy_to(it, it->empty_header.nbytes + offset, &now, sizeof(now), true);
        it->empty_header.it_flags |= ITEM_HAS_TIMESTAMP;
        slack -= sizeof(now);
        offset += sizeof(now);
    }

    /* still enough space for the ip address? */
    if (slack >= sizeof(addr)) {
        /* enough space for both the timestamp and the ip address */

        /* save the address */
        item_memcpy_to(it, it->empty_header.nbytes + offset, &addr, sizeof(addr), true);
        it->empty_header.it_flags |= ITEM_HAS_IP_ADDRESS;
        slack -= sizeof(addr);
        offset += sizeof(addr);
    }
}


/* allocates one item capable of storing a key of size nkey and a value field of
 * size nbytes.  stores the key, flags, and exptime.  the value field is not
 * initialized.  if there is insufficient memory, NULL is returned. */
item* do_item_alloc(const char *key, const size_t nkey, const int flags, const rel_time_t exptime,
                    const size_t nbytes, const struct in_addr addr) {
    if (item_size_ok(nkey, flags, nbytes) == false) {
        return NULL;
    }

    if (is_large_chunk(nkey, nbytes)) {
        /* allocate a large chunk */

        /* try various strategies to get a free item:
         * 1) free_list
         * 2) flat_storage_alloc
         * 3) if we have sufficient small free chunks + large free chunks to
         *    store the item, try a coalesce.
         * 4) flat_storage_lru_evict
         */
        size_t needed = chunks_needed(nkey, nbytes);
        size_t prev_free = fsi.large_free_list_sz - 1;
        chunk_t* temp;
        large_title_chunk_t* title;
        large_body_chunk_t* body;
        chunkptr_t* prev_next;
        size_t write_offset = nkey + nbytes;
        size_t key_left = nkey, key_write;

        while (fsi.large_free_list_sz < needed) {
            assert(prev_free != fsi.large_free_list_sz);
            prev_free = fsi.large_free_list_sz;
            /* try flat_storage_alloc first */
            if (flat_storage_alloc()) {
                continue;
            }

            if (((fsi.large_free_list_sz * SMALL_CHUNKS_PER_LARGE_CHUNK) +
                 fsi.small_free_list_sz) >= (needed * SMALL_CHUNKS_PER_LARGE_CHUNK)) {
                /* try a coalesce */
                coalesce_free_small_chunks();
            }
            if (prev_free != fsi.large_free_list_sz) {
                continue;
            }

            if (flat_storage_lru_evict(LARGE_CHUNK, needed)) {
                continue;
            }

            /* all avenues have been exhausted, and we still have
             * insufficient memory. */
            return NULL;
        }

        /* now chain up the chunks. */
        temp = free_list_pop(LARGE_CHUNK);
        temp->lc.flags |= (LARGE_CHUNK_USED | LARGE_CHUNK_TITLE);
        assert(temp != NULL);
        title = &(temp->lc.lc_title);
        title->h_next = NULL_ITEM_PTR;
        title->next = title->prev = title->next_chunk = NULL_CHUNKPTR;
        title->refcount = 1;            /* the caller will have a reference */
        title->it_flags = ITEM_VALID;
        title->nkey = nkey;
        title->nbytes = nbytes;
        title->exptime = exptime;
        title->flags = flags;
        prev_next = &title->next_chunk;

        key_write = __fs_MIN(LARGE_TITLE_CHUNK_DATA_SZ, key_left);
        memcpy(title->data, key, key_write);
        key_left -= key_write;
        key += key_write;

        if (needed == 1) {
            title->it_flags |= do_stamp_on_block(title->data, write_offset, LARGE_TITLE_CHUNK_DATA_SZ,
                                                 current_time, addr);
        }

        needed --;
        write_offset -= LARGE_TITLE_CHUNK_DATA_SZ;

        /* STATS: update */
        fsi.stats.large_title_chunks ++;
        fsi.stats.large_body_chunks += needed;

        while (needed > 0) {
            temp = free_list_pop(LARGE_CHUNK);
            temp->lc.flags |= LARGE_CHUNK_USED;
            assert(temp != NULL);
            body = &(temp->lc.lc_body);
            *(prev_next) = get_chunkptr(temp);
            prev_next = &body->next_chunk;

            key_write = __fs_MIN(LARGE_BODY_CHUNK_DATA_SZ, key_left);
            memcpy(body->data, key, key_write);
            key_left -= key_write;
            key += key_write;

            if (needed == 1) {
                title->it_flags |= do_stamp_on_block(body->data, write_offset, LARGE_BODY_CHUNK_DATA_SZ,
                                                 current_time, addr);
            }

            needed --;
            write_offset -= LARGE_BODY_CHUNK_DATA_SZ;
        }
        *(prev_next) = NULL_CHUNKPTR;

        return get_item_from_large_title(title);
    } else {
        /* allocate a small chunk */

        /* try various strategies to get a free item:
         * 1) small free_list
         * 2) large free_list
         * 3) flat_storage_alloc
         * 4) flat_storage_lru_evict
         */
        size_t needed = chunks_needed(nkey, nbytes);
        size_t small_prev_free = fsi.small_free_list_sz - 1,
            large_prev_free = fsi.large_free_list_sz;
        chunk_t* temp;
        small_title_chunk_t* title;
        small_body_chunk_t* body;
        chunkptr_t prev;
        chunkptr_t* prev_next;
        size_t write_offset = nkey + nbytes;
        size_t key_left = nkey, key_write;

        while (fsi.small_free_list_sz < needed) {
            assert(small_prev_free != fsi.small_free_list_sz ||
                   large_prev_free != fsi.large_free_list_sz);
            small_prev_free = fsi.small_free_list_sz;
            large_prev_free = fsi.large_free_list_sz;

            if (fsi.large_free_list_sz > 0) {
                temp = free_list_pop(LARGE_CHUNK);
                assert(temp != NULL);
                break_large_chunk(temp);
                continue;
            }

            /* try flat_storage_alloc first */
            if (flat_storage_alloc()) {
                continue;
            }

            if (flat_storage_lru_evict(SMALL_CHUNK, needed)) {
                continue;
            }

            /* all avenues have been exhausted, and we still have
             * insufficient memory. */
            return NULL;
        }

        /* now chain up the chunks. */
        temp = free_list_pop(SMALL_CHUNK);
        temp->sc.flags |= (SMALL_CHUNK_USED | SMALL_CHUNK_TITLE);
        assert(temp != NULL);
        title = &(temp->sc.sc_title);
        title->h_next = NULL_ITEM_PTR;
        title->next = title->prev = title->next_chunk = NULL_CHUNKPTR;
        title->refcount = 1;            /* the caller will have a reference */
        title->it_flags = ITEM_VALID;
        title->nkey = nkey;
        title->nbytes = nbytes;
        title->exptime = exptime;
        title->flags = flags;
        prev = get_chunkptr(temp);
        prev_next = &title->next_chunk;

        key_write = __fs_MIN(SMALL_TITLE_CHUNK_DATA_SZ, key_left);
        memcpy(title->data, key, key_write);
        key_left -= key_write;
        key += key_write;

        if (needed == 1) {
            title->it_flags |= do_stamp_on_block(title->data, write_offset, SMALL_TITLE_CHUNK_DATA_SZ,
                                                 current_time, addr);
        }

        needed --;
        write_offset -= SMALL_TITLE_CHUNK_DATA_SZ;

        /* STATS: update */
        fsi.stats.small_title_chunks ++;
        fsi.stats.small_body_chunks += needed;

        while (needed > 0) {
            chunkptr_t current_chunkptr;
            temp = free_list_pop(SMALL_CHUNK);
            temp->sc.flags |= SMALL_CHUNK_USED;
            assert(temp != NULL);

            current_chunkptr = get_chunkptr(temp);
            body = &(temp->sc.sc_body);
            *(prev_next) = current_chunkptr;
            body->prev_chunk = prev;
            prev_next = &body->next_chunk;
            prev = current_chunkptr;

            key_write = __fs_MIN(SMALL_BODY_CHUNK_DATA_SZ, key_left);
            memcpy(body->data, key, key_write);
            key_left -= key_write;
            key += key_write;

            if (needed == 1) {
                title->it_flags |= do_stamp_on_block(body->data, write_offset, SMALL_BODY_CHUNK_DATA_SZ,
                                                     current_time, addr);
            }

            needed --;
            write_offset -= SMALL_BODY_CHUNK_DATA_SZ;
        }
        *(prev_next) = NULL_CHUNKPTR;

        return get_item_from_small_title(title);
    }
}


/* marks the item as free.  if to_freelist is true, it can be sent to the
 * freelist.  as it is now, to_freelist is *always* true. */
static void item_free(item *it) {
    chunkptr_t next_chunk;
    size_t chunks_freed = 0;
#if !defined(NDEBUG)
    size_t expected_chunks_freed = chunks_in_item(it);
#endif /* #if !defined(NDEBUG) */
    bool is_large_chunks = is_item_large_chunk(it);

    assert((it->empty_header.it_flags & ~(ITEM_HAS_TIMESTAMP | ITEM_HAS_IP_ADDRESS))== ITEM_VALID);
    assert(it->empty_header.refcount == 0);
    assert(it->empty_header.next == NULL_CHUNKPTR);
    assert(it->empty_header.prev == NULL_CHUNKPTR);
    assert(it->empty_header.h_next == NULL_ITEM_PTR);

    /* find all the chunks and liberate them. */
    next_chunk = it->empty_header.next_chunk;
    if (is_large_chunks) {
        chunk_t* chunk;

        /* free body chunks */
        while (next_chunk != NULL_CHUNKPTR) {
            chunk = get_chunk_address(next_chunk);

            /* save the next chunk before we obliterate things. */
            next_chunk = chunk->lc.lc_body.next_chunk;

            assert(chunk->lc.flags == (LARGE_CHUNK_INITIALIZED |
                                       LARGE_CHUNK_USED));
            chunk->lc.flags &= ~(LARGE_CHUNK_USED);
            DEBUG_CLEAR(&chunk->lc.lc_body, sizeof(large_body_chunk_t));
            free_list_push(chunk, LARGE_CHUNK, false);
            chunks_freed ++;
        }

        /* STATS: update */
        fsi.stats.large_body_chunks -= chunks_freed;

        /* free the title */
        chunk = (chunk_t*) &(it->large_title);
        assert(chunk->lc.flags == (LARGE_CHUNK_INITIALIZED |
                                   LARGE_CHUNK_USED |
                                   LARGE_CHUNK_TITLE));
        chunk->lc.flags &= ~(LARGE_CHUNK_USED | LARGE_CHUNK_TITLE);
        DEBUG_CLEAR(&chunk->lc.lc_title, sizeof(large_title_chunk_t));
        it->large_title.it_flags = 0;   /* but the flags should be 0. */
        free_list_push(chunk, LARGE_CHUNK, false);
        chunks_freed ++;

        /* STATS: update */
        fsi.stats.large_title_chunks --;
    } else {
        chunk_t* chunk;

        /* free body chunks */
        while (next_chunk != NULL_CHUNKPTR) {
            chunk = get_chunk_address(next_chunk);

            /* save the next chunk before we obliterate things. */
            next_chunk = chunk->sc.sc_body.next_chunk;

            assert(chunk->sc.flags == (SMALL_CHUNK_INITIALIZED |
                                       SMALL_CHUNK_USED));
            chunk->sc.flags &= ~(SMALL_CHUNK_USED);
            DEBUG_CLEAR(&chunk->sc.sc_body, sizeof(small_body_chunk_t));
            free_list_push(chunk, SMALL_CHUNK, true);

            chunks_freed ++;
        }

        /* STATS: update */
        fsi.stats.small_body_chunks -= chunks_freed;

        /* free the title */
        chunk = (chunk_t*) &(it->small_title);
        assert(chunk->sc.flags == (SMALL_CHUNK_INITIALIZED |
                                   SMALL_CHUNK_USED |
                                   SMALL_CHUNK_TITLE));
        chunk->sc.flags &= ~(SMALL_CHUNK_USED | SMALL_CHUNK_TITLE);
        DEBUG_CLEAR(&chunk->sc.sc_title, sizeof(small_title_chunk_t));
        it->small_title.it_flags = 0;   /* but the flags should be 0. */
        free_list_push(chunk, SMALL_CHUNK, true);
        chunks_freed ++;

        /* STATS: update */
        fsi.stats.small_title_chunks --;
    }

    assert(chunks_freed == expected_chunks_freed);
}


/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    return (nkey <= KEY_MAX_LENGTH) && (nbytes <= MAX_ITEM_SIZE);
}


bool item_need_realloc(const item* it,
                       const size_t new_nkey, const int new_flags, const size_t new_nbytes) {
    return (is_item_large_chunk(it) != is_large_chunk(new_nkey, new_nbytes) ||
            chunks_in_item(it) != chunks_needed(new_nkey, new_nbytes));
}


static void item_link_q(item *it) {
    assert(it->empty_header.next == NULL_CHUNKPTR);
    assert(it->empty_header.prev == NULL_CHUNKPTR);

    assert( ((fsi.lru_head == NULL) ^ (fsi.lru_head == NULL)) == 0 );
    if (fsi.lru_head != NULL) {
        it->empty_header.next = get_chunkptr((chunk_t*) fsi.lru_head);
        fsi.lru_head->empty_header.prev = get_chunkptr((chunk_t*) it);
    }
    fsi.lru_head = it;

    if (fsi.lru_tail == NULL) {
        fsi.lru_tail = it;
    }
}


static void item_unlink_q(item* it) {
    item* next, * prev;

    next = get_item_from_chunk(get_chunk_address(it->empty_header.next));
    prev = get_item_from_chunk(get_chunk_address(it->empty_header.prev));

    if (it == fsi.lru_head) {
        assert(prev == NULL);
        fsi.lru_head = next;
    }
    if (it == fsi.lru_tail) {
        assert(next == NULL);
        fsi.lru_tail = prev;
    }

    if (next) {
        next->empty_header.prev = it->empty_header.prev;
    }
    if (prev) {
        prev->empty_header.next = it->empty_header.next;
    }

    it->empty_header.prev = NULL_CHUNKPTR;
    it->empty_header.next = NULL_CHUNKPTR;
}


/**
 * adds the item to the LRU.
 */
int do_item_link(item* it, const char* key) {
    stats_t *stats = STATS_GET_TLS();
    assert(it->empty_header.it_flags & ITEM_VALID);
    assert((it->empty_header.it_flags & ITEM_LINKED) == 0);

    it->empty_header.it_flags |= ITEM_LINKED;
    it->empty_header.time = current_time;
    assoc_insert(it, key);

    STATS_LOCK(stats);
    stats->item_total_size += ITEM_nkey(it) + ITEM_nbytes(it);
    stats->curr_items += 1;
    stats->total_items += 1;
    STATS_UNLOCK(stats);

    item_link_q(it);

    return 1;
}


/*
 * unlink an item from the LRU and the assoc table. because there is a race
 * condition between item_get(..) and item_unlink(..) in
 * process_delete_command(..), we must use the key to look up in the assoc table
 * to ensure that we are deleting the correct item.
 */
void do_item_unlink(item* it, long flags, const char* key) {
    stats_t *stats = STATS_GET_TLS();
    char key_temp[KEY_MAX_LENGTH];
    if (key == NULL) {
        key = item_key_copy(it, key_temp);
    }

    assert(it->empty_header.it_flags & ITEM_VALID);
    /*
     * this test (& ITEM_LINKED) must be here because the cache lock is not held
     * between item_get and item_unlink in process_delete_command.  therefore,
     * another delete could sneak in between and remove the item from the LRU.
     */
    if (it->empty_header.it_flags & ITEM_LINKED) {
        it->empty_header.it_flags &= ~(ITEM_LINKED);
        if (flags & UNLINK_MAYBE_EVICT) {
            /* if the item is expired, then it is an expire.  otherwise it is an
             * evict. */
            if (it->empty_header.exptime == 0 ||
                it->empty_header.exptime > current_time) {
                /* it's an evict. */
                flags = UNLINK_IS_EVICT;
            } else {
                flags = UNLINK_IS_EXPIRED;
            }
        }

        STATS_LOCK(stats);
        stats->item_total_size -= ITEM_nkey(it) + ITEM_nbytes(it);
        stats->curr_items -= 1;
        STATS_UNLOCK(stats);

        if (flags & UNLINK_IS_EVICT) {
            stats_evict(ITEM_nkey(it) + ITEM_nbytes(it));
            STATS_LOCK(stats);
            stats->evictions ++;
            STATS_UNLOCK(stats);
        } else if (flags & UNLINK_IS_EXPIRED) {
            stats_expire(ITEM_nkey(it) + ITEM_nbytes(it));
        }
        if (settings.detail_enabled) {
            stats_prefix_record_removal(key, ITEM_nkey(it), ITEM_nkey(it) + ITEM_nbytes(it), it->empty_header.time, flags);
        }
        assoc_delete(key, ITEM_nkey(it));
        it->empty_header.h_next = NULL_ITEM_PTR;
        item_unlink_q(it);
        if (it->empty_header.refcount == 0) {
            item_free(it);
        }
    }
}


/* decrease the refcount of item it */
void do_item_deref(item* it) {
    assert(it->empty_header.it_flags & ITEM_VALID);

    /* may not be ITEM_LINKED because the unlink may have preceeded the remove. */
    if (it->empty_header.refcount != 0) {
        it->empty_header.refcount --;
    }
    assert((it->empty_header.it_flags & ITEM_DELETED) == 0 ||
           it->empty_header.refcount != 0);
    if (it->empty_header.refcount == 0 &&
        (it->empty_header.it_flags & ITEM_LINKED) == 0) {
        item_free(it);
    }
}


/** update LRU time to current and reposition */
void do_item_update(item* it) {
    if (it->empty_header.time < current_time - ITEM_UPDATE_INTERVAL) {
        assert(it->empty_header.it_flags & ITEM_VALID);

        if (it->empty_header.it_flags & ITEM_LINKED) {
            item_unlink_q(it);
            it->empty_header.time = current_time;
            item_link_q(it);
        }
    }
}

int do_item_replace(item* it, item* new_it, const char* key) {
    int retval;

    assert((it->empty_header.it_flags & (ITEM_VALID | ITEM_LINKED)) ==
           (ITEM_VALID | ITEM_LINKED));
    do_item_unlink(it, UNLINK_NORMAL, key);

    assert(new_it->empty_header.it_flags & ITEM_VALID);
    retval = do_item_link(new_it, key);
    return retval;
}


char* do_item_cachedump(const chunk_type_t type, const unsigned int limit, unsigned int* bytes) {
    unsigned int memlimit = ITEM_CACHEDUMP_LIMIT;   /* 2MB max response size */
    char *buffer;
    unsigned int bufcurr;
    item *it;
    unsigned int len;
    unsigned int shown = 0;
    char temp[512];
    char key_temp[KEY_MAX_LENGTH];
    const char* key;

    buffer = malloc((size_t)memlimit);
    if (buffer == 0) return NULL;
    bufcurr = 0;

    it = fsi.lru_head;

    while (it != NULL && (limit == 0 || shown < limit)) {
        key = item_key_copy(it, key_temp);
        len = snprintf(temp, sizeof(temp), "ITEM %*s [%d b; %lu s]\r\n",
                       ITEM_nkey(it), key,
                       ITEM_nbytes(it), it->empty_header.time + started);
        if (bufcurr + len + 6 > memlimit)  /* 6 is END\r\n\0 */
            break;
        strcpy(buffer + bufcurr, temp);
        bufcurr += len;
        shown++;
        it = get_item_from_chunk(get_chunk_address(it->empty_header.next));
    }

    memcpy(buffer + bufcurr, "END\r\n", 6);
    bufcurr += 5;

    *bytes = bufcurr;
    return buffer;
}


char* do_item_stats_sizes(int* bytes) {
    const size_t max_item_size = sizeof(large_chunk_t) + KEY_MAX_LENGTH + MAX_ITEM_SIZE;
    const int num_buckets = (max_item_size + 32 - 1) / 32;   /* max object, divided into 32 bytes size buckets */
    unsigned int *histogram = (unsigned int *)malloc((size_t)num_buckets * sizeof(int));
    char *buf = (char *)malloc(ITEM_STATS_SIZES); /* 2MB max response size */
    int i;

    if (histogram == 0 || buf == 0) {
        if (histogram) free(histogram);
        if (buf) free(buf);
        return NULL;
    }

    /* build the histogram */
    memset(histogram, 0, (size_t)num_buckets * sizeof(int));

    item* iter = fsi.lru_head;
    while (iter) {
        int ntotal = ITEM_ntotal(iter);
        int bucket = ntotal / 32;
        if ((ntotal % 32) != 0) bucket++;
        if (bucket < num_buckets) histogram[bucket]++;
        iter = get_item_from_chunk(get_chunk_address(iter->small_title.next));
    }

    iter = fsi.lru_head;
    while (iter) {
        int ntotal = ITEM_ntotal(iter);
        int bucket = ntotal / 32;
        if ((ntotal % 32) != 0) bucket++;
        if (bucket < num_buckets) histogram[bucket]++;
        iter = get_item_from_chunk(get_chunk_address(iter->large_title.next));
    }

    /* write the buffer */
    *bytes = 0;
    for (i = 0; i < num_buckets; i++) {
        if (histogram[i] != 0) {
            *bytes += sprintf(&buf[*bytes], "%d %u\r\n", i * 32, histogram[i]);
        }
    }
    *bytes += sprintf(&buf[*bytes], "END\r\n");
    free(histogram);
    return buf;
}


void do_item_flush_expired(void) {
    item *iter, *next;
    if (settings.oldest_live == 0)
        return;

    for (iter = fsi.lru_head;
         iter != NULL;
         iter = next) {
        if (iter->small_title.time >= settings.oldest_live) {
            next = get_item_from_chunk(get_chunk_address(iter->small_title.next));
            assert( (iter->empty_header.it_flags & (ITEM_VALID | ITEM_LINKED)) ==
                    (ITEM_VALID | ITEM_LINKED) );
            do_item_unlink(iter, UNLINK_IS_EXPIRED, NULL);
        } else {
            /* We've hit the first old item. Continue to the next queue. */
            break;
        }
    }

    for (iter = fsi.lru_head;
         iter != NULL;
         iter = next) {
        if (iter->large_title.time >= settings.oldest_live) {
            next = get_item_from_chunk(get_chunk_address(iter->large_title.next));
            assert( (iter->empty_header.it_flags & (ITEM_VALID | ITEM_LINKED)) ==
                    (ITEM_VALID | ITEM_LINKED) );
            do_item_unlink(iter, UNLINK_IS_EXPIRED, NULL);
        } else {
            /* We've hit the first old item. Continue to the next queue. */
            break;
        }
    }
}


item* item_get(const char* key, const size_t nkey) {
    return item_get_notedeleted(key, nkey, NULL);
}


item* do_item_get_notedeleted(const char* key, const size_t nkey, bool* delete_locked) {
    item *it = assoc_find(key, nkey);
    if (delete_locked) *delete_locked = false;
    if (it != NULL && (it->empty_header.it_flags & ITEM_DELETED)) {
        /* it's flagged as delete-locked.  let's see if that condition
           is past due, and the 5-second delete_timer just hasn't
           gotten to it yet... */
        if (!item_delete_lock_over(it)) {
            if (delete_locked) *delete_locked = true;
            it = NULL;
        }
    }
    if (it != NULL && settings.oldest_live != 0 && settings.oldest_live <= current_time &&
        it->empty_header.time <= settings.oldest_live) {
        do_item_unlink(it, UNLINK_IS_EXPIRED, key); /* MTSAFE - cache_lock held */
        it = NULL;
    }
    if (it != NULL && it->empty_header.exptime != 0 && it->empty_header.exptime <= current_time) {
        do_item_unlink(it, UNLINK_IS_EXPIRED, key); /* MTSAFE - cache_lock held */
        it = NULL;
    }

    if (it != NULL) {
        it->empty_header.refcount ++;
    }
    return it;
}


item* do_item_get_nocheck(const char* key, const size_t nkey) {
    item *it = assoc_find(key, nkey);
    if (it) {
        it->empty_header.refcount ++;
    }
    return it;
}

/* returns true if a deleted item's delete-locked-time is over, and it
   should be removed from the namespace */
bool item_delete_lock_over(item* it) {
    assert(it->empty_header.it_flags & ITEM_DELETED);
    return (current_time >= it->empty_header.exptime);
}


/**
 * returns a pointer to the key, flattened into a single array.  if the key
 * spans multiple chunks, it is copied into space pointed to by keyptr.
 * otherwise, the key is returned directly.
 */
const char* item_key_copy(const item* it, char* keyptr) {
    const char* retval = keyptr;
    size_t title_data_size;

    if (is_item_large_chunk(it)) {
        title_data_size = LARGE_TITLE_CHUNK_DATA_SZ;
        if (it->large_title.nkey <= title_data_size) {
            return &it->large_title.data[0];
        }
    } else {
        title_data_size = SMALL_TITLE_CHUNK_DATA_SZ;
        if (it->small_title.nkey <= title_data_size) {
            return &it->small_title.data[0];
        }
    }
#define ITEM_key_copy_applier(it, ptr, bytes)   \
    memcpy(keyptr, ptr, bytes);                 \
    keyptr += bytes;

    ITEM_WALK(it, 0, it->empty_header.nkey, false, ITEM_key_copy_applier, const);

    return retval;
}


char* do_flat_allocator_stats(size_t* result_size) {
    size_t bufsize = 2048, offset = 0, i;
    char* buffer = malloc(bufsize);
    char terminator[] = "END\r\n";
    item* lru_item = NULL;
    rel_time_t oldest_item_lifetime;

    if (buffer == NULL) {
        *result_size = 0;
        return NULL;
    }

    /* get the LRU items */
    lru_item = get_lru_item();
    if (lru_item == NULL) {
        oldest_item_lifetime = 0;
    } else {
        oldest_item_lifetime = current_time - lru_item->empty_header.time;
    }

    offset = append_to_buffer(buffer, bufsize, offset, sizeof(terminator),
                              "STAT large_chunk_sz %d\n"
                              "STAT small_chunk_sz %d\n"
                              "STAT large_title_chunks %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT large_body_chunks %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT large_broken_chunks %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT small_title_chunks %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT small_body_chunks %" PRINTF_INT64_MODIFIER "u\n",
                              LARGE_CHUNK_SZ,
                              SMALL_CHUNK_SZ,
                              fsi.stats.large_title_chunks,
                              fsi.stats.large_body_chunks,
                              fsi.stats.large_broken_chunks,
                              fsi.stats.small_title_chunks,
                              fsi.stats.small_body_chunks);

    for (i = 0; i < SMALL_CHUNKS_PER_LARGE_CHUNK + 1; i ++) {
        offset = append_to_buffer(buffer, bufsize, offset, sizeof(terminator),
                                  "STAT broken_chunk_histogram %lu %" PRINTF_INT64_MODIFIER "u\n", i, fsi.stats.broken_chunk_histogram[i]);
   }

    offset = append_to_buffer(buffer, bufsize, offset, sizeof(terminator),
                              "STAT break_events %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT unbreak_events %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT migrates %" PRINTF_INT64_MODIFIER "u\n"
                              "STAT unused_memory %lu\n"
                              "STAT large_free_list_sz %lu\n"
                              "STAT small_free_list_sz %lu\n"
                              "STAT oldest_item_lifetime %us\n",
                              fsi.stats.break_events,
                              fsi.stats.unbreak_events,
                              fsi.stats.migrates,
                              fsi.unused_memory,
                              fsi.large_free_list_sz,
                              fsi.small_free_list_sz,
                              oldest_item_lifetime);

    offset = append_to_buffer(buffer, bufsize, offset, 0, terminator);

    *result_size = offset;

    return buffer;
}

#endif /* #if defined(USE_FLAT_ALLOCATOR) */
