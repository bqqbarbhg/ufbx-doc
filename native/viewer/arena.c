#include "arena.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    ARENAIMP_MAGIC_FREE = 0x65657266,
    ARENAIMP_MAGIC_ARENA = 0x6e657261,
    ARENAIMP_MAGIC_DEFER = 0x66656461,
};

static const uint8_t arenaimp_size_classes[] = {
    2, 3, 4, 6, 10, 14, 18, 26, 34, 56,
};

static const uint8_t arenaimp_size_to_class[] = {
    0, 0, 0, 1, 2, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6,
    7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
};

enum {
    ARENAIMP_SIZECLASS_QUANTIZATION = 8,
    ARENAIMP_NUM_SIZECLASSES = sizeof(arenaimp_size_classes),
    ARENAIMP_LARGEST_SIZECLASS = 448,
    ARENAIMP_FIRST_PAGE_SIZE = 512,
    ARENAIMP_MAX_PAGE_SIZE = 4096,
};

typedef struct arenaimp_t arenaimp_t;
typedef union arenaimp_common_header arenaimp_common_header;
typedef union arenaimp_small_header arenaimp_small_header;
typedef struct arenaimp_big_header arenaimp_big_header;

typedef struct {
	arena_defer_fn *fn;
	void *user;
    size_t prev, next;
} arenaimp_defer_slot;

typedef struct {
    size_t magic;
    size_t slot;
} arenaimp_defer_header;

union arenaimp_common_header {
    uint64_t imp_align;
    size_t capacity;
};

union arenaimp_small_header {
	arenaimp_common_header active;
    struct {
		arenaimp_small_header *next_free;
    } freed;
};

struct arenaimp_big_header {
	arenaimp_big_header *prev;
	arenaimp_big_header *next;
	arenaimp_common_header common;
};

struct arenaimp_t {
    size_t magic;

    char *page;
    size_t pos, size;

    arenaimp_t *parent;
    size_t parent_slot;
    size_t next_size;

    arenaimp_defer_slot *defers;
    size_t num_defers;
    size_t active_defer_head;
    size_t free_defer_head;

    arenaimp_big_header big_head, big_tail;

    arenaimp_small_header *next_free[ARENAIMP_NUM_SIZECLASSES];

    bool allocated;
};

enum {
    ARENAIMP_INIT_SIZE = 512,
    ARENAIMP_EXTRA_SIZE = 512 - sizeof(arenaimp_t),
};

static void arenaimp_copy(void *dst, const void *src, size_t size)
{
    if (src) {
        memcpy(dst, src, size);
    } else {
        memset(dst, 0, size);
    }
}

static void arenaimp_defer_free_arena(void *user)
{
    arena_free((arena_t*)user);
}

static void arenaimp_init(arenaimp_t *a)
{
	a->magic = ARENAIMP_MAGIC_ARENA;
    a->active_defer_head = SIZE_MAX;
    a->free_defer_head = SIZE_MAX;
    a->big_head.next = &a->big_tail;
    a->big_tail.next = &a->big_head;
    a->next_size = ARENAIMP_FIRST_PAGE_SIZE / 2;

    a->page = (char*)a;
    a->pos = sizeof(arenaimp_t);
    a->size = ARENAIMP_INIT_SIZE;
}

arena_t *arena_create(arena_t *parent)
{
	arenaimp_t *a, *p = (arenaimp_t*)parent;
    if (p) {
        a = (arenaimp_t*)aalloc_size(parent, sizeof(arenaimp_t) + ARENAIMP_EXTRA_SIZE, 1);
        if (!a) return NULL;
        a->parent = p;
        a->parent_slot = arena_ext_defer(parent, &arenaimp_defer_free_arena, a);
        if (a->parent_slot == SIZE_MAX) {
            afree(parent, a);
            return NULL;
        }
    } else {
        a = (arenaimp_t*)malloc(sizeof(a) + ARENAIMP_EXTRA_SIZE);
        if (!a) return NULL;
        memset(a, 0, sizeof(arenaimp_t));
    }

    a->allocated = true;
    arenaimp_init(a);
    return (arena_t*)a;
}

bool arena_init(arena_t *arena, arena_t *parent)
{
	arenaimp_t *a = (arenaimp_t*)arena, *p = (arenaimp_t*)parent;
	memset(a, 0, sizeof(arenaimp_t));
    if (p) {
        a->parent = p;
        a->parent_slot = arena_ext_defer(parent, &arenaimp_defer_free_arena, a);
        if (a->parent_slot == SIZE_MAX) {
            afree(parent, a);
            return false;
        }
    }

    a->allocated = false;
    arenaimp_init(a);
    return true;
}

void arena_free(arena_t *arena)
{
	arenaimp_t *a = (arenaimp_t*)arena;
    if (!a) return;
    assert(a->magic == ARENAIMP_MAGIC_ARENA);
    a->magic = ARENAIMP_MAGIC_FREE;

    size_t slot = a->active_defer_head;
    arenaimp_defer_slot *defers = a->defers;
    while (slot != SIZE_MAX) {
        defers[slot].fn(defers[slot].user);
        slot = defers[slot].next;
    }

    arenaimp_big_header *alloc = a->big_head.next;
    arenaimp_big_header *last = &a->big_tail;
    while (alloc != last) {
		arenaimp_big_header *next = alloc->next;
        free(alloc);
        alloc = next;
    }

    if (a->allocated) {
		if (a->parent) {
			arena_t *parent = (arena_t*)a->parent;
			arena_ext_cancel(parent, a->parent_slot, false);
			afree(parent, a);
		} else {
			free(a);
		}
    }
}

void *arena_defer_size(arena_t *arena, arena_defer_fn *fn, size_t size, const void *data)
{
	arenaimp_t *a = (arenaimp_t*)arena;
    assert(a);
    assert(a->magic == ARENAIMP_MAGIC_ARENA);

    size_t total = sizeof(arenaimp_defer_header) + size;
    arenaimp_defer_header *dh = (arenaimp_defer_header*)aalloc_uninit_size(arena, total, 1);
    assert(dh);
    if (!dh) return NULL;
    void *copy = dh + 1;

    arenaimp_copy(copy, data, size);
    size_t slot = arena_ext_defer((arena_t*)a, fn, copy);
    if (slot == SIZE_MAX) {
        afree(arena, dh);
        return NULL;
    }

    dh->magic = ARENAIMP_MAGIC_DEFER;
    dh->slot = slot;
    return copy;
}

void arena_cancel_retain(arena_t *arena, void *ptr, bool run_defer)
{
    if (!ptr) return;
    arenaimp_defer_header *dh = (arenaimp_defer_header*)ptr - 1;
    assert(dh->magic == ARENAIMP_MAGIC_DEFER);

    arena_ext_cancel(arena, dh->slot, run_defer);

	dh->slot = SIZE_MAX;
    dh->magic = ARENAIMP_MAGIC_FREE;
}

void arena_cancel(arena_t *arena, void *ptr, bool run_defer)
{
    if (!ptr) return;
    arena_cancel_retain(arena, ptr, run_defer);
    arenaimp_defer_header *dh = (arenaimp_defer_header*)ptr - 1;
    afree(arena, dh);
}

size_t arena_ext_defer(arena_t *arena, arena_defer_fn *fn, const void *data)
{
	arenaimp_t *a = (arenaimp_t*)arena;
    assert(a);
    assert(a->magic == ARENAIMP_MAGIC_ARENA);
    assert(fn);

    size_t slot = 0;
    if (a->free_defer_head != SIZE_MAX) {
        slot = a->free_defer_head;
        a->free_defer_head = a->defers[slot].next;
    } else {
        arenaimp_defer_slot *defers = arealloc((arena_t*)a, arenaimp_defer_slot, a->num_defers + 1, a->defers);
        if (!defers) return SIZE_MAX;
        a->defers = defers;
        slot = a->num_defers++;
    }

    size_t head = a->active_defer_head;
    arenaimp_defer_slot *ds = &a->defers[slot];
    ds->fn = fn;
    ds->user = (void*)data;
    ds->next = head;
    ds->prev = SIZE_MAX;
    if (head != SIZE_MAX) {
        a->defers[head].prev = slot;
    }
    a->active_defer_head = slot;
    return slot;
}

void arena_ext_redefer(arena_t *arena, size_t slot, arena_defer_fn *fn, const void *data)
{
	arenaimp_t *a = (arenaimp_t*)arena;
    assert(a);
    assert(a->magic == ARENAIMP_MAGIC_ARENA);

    arenaimp_defer_slot *ds = &a->defers[slot];
    ds->fn = fn;
    ds->user = (void*)data;
}

void arena_ext_cancel(arena_t *arena, size_t slot, bool run_defer)
{
	arenaimp_t *a = (arenaimp_t*)arena;
    assert(a);
    assert(a->magic == ARENAIMP_MAGIC_ARENA);

    arenaimp_defer_slot *ds = &a->defers[slot];
    if (run_defer) {
        ds->fn(ds->user);
    }

    if (ds->prev != SIZE_MAX) {
        a->defers[ds->prev].next = ds->next;
    } else {
		a->active_defer_head = ds->next;
    }
    if (ds->next != SIZE_MAX) {
        a->defers[ds->next].prev = ds->prev;
    }

    ds->fn = NULL;
    ds->user = NULL;
    ds->next = a->free_defer_head;
    ds->prev = SIZE_MAX;
    a->free_defer_head = slot;
}

void *aalloc_uninit_size(arena_t *arena, size_t size, size_t count)
{
    size_t total = size * count;
    if (!arena) {
		size_t total_alloc = sizeof(arenaimp_common_header) + total;
        arenaimp_common_header *alloc = (arenaimp_common_header*)malloc(total_alloc);
        if (!alloc) return NULL;
        alloc->capacity = total;
        return alloc + 1;
    }

	arenaimp_t *a = (arenaimp_t*)arena;
    assert(a);
    assert(a->magic == ARENAIMP_MAGIC_ARENA);

    size_t total_small = sizeof(arenaimp_small_header) + total;

    if (total_small <= ARENAIMP_LARGEST_SIZECLASS) {
        size_t quantized = (total_small + ARENAIMP_SIZECLASS_QUANTIZATION - 1) / ARENAIMP_SIZECLASS_QUANTIZATION;
        size_t sizeclass = arenaimp_size_to_class[quantized];
        arenaimp_small_header *next = a->next_free[sizeclass];
        if (next) {
            a->next_free[sizeclass] = next->freed.next_free;
            next->active.capacity = total;
            return next + 1;
        } else {
            size_t chunk = (size_t)arenaimp_size_classes[sizeclass] * ARENAIMP_SIZECLASS_QUANTIZATION;
            size_t pos = a->pos;
            if (a->size - pos >= chunk) {
				arenaimp_small_header *alloc = (arenaimp_small_header*)(a->page + pos);
                a->pos = pos + chunk;
                alloc->active.capacity = total;
                return alloc + 1;
            } else {
                size_t next_size = a->next_size * 2;
                if (next_size > ARENAIMP_MAX_PAGE_SIZE) next_size = ARENAIMP_MAX_PAGE_SIZE;
                a->next_size = next_size;

                size_t page_size = next_size;
                if (page_size < total_small) page_size = total_small;
                assert(page_size > ARENAIMP_LARGEST_SIZECLASS);
                void *new_page = aalloc_uninit_size((arena_t*)a, 1, page_size);
                if (!new_page) return NULL;

				arenaimp_small_header *alloc = (arenaimp_small_header*)new_page;
                alloc->active.capacity = total;

                if (page_size - total_small > a->size - a->pos) {
					a->page = (char*)new_page;
					a->pos = total_small;
					a->size = page_size;
                }
                return alloc + 1;
            }
        }
    } else {
        arenaimp_big_header *alloc = (arenaimp_big_header*)malloc(sizeof(arenaimp_big_header) + total);
        assert(alloc);
        if (!alloc) return NULL;

        arenaimp_big_header *head = &a->big_head;
        arenaimp_big_header *next = head->next;
        alloc->prev = head;
        alloc->next = next;
        alloc->common.capacity = total;
        next->prev = alloc;
        head->next = alloc;
        return alloc + 1;
    }
}

void *aalloc_size(arena_t *a, size_t size, size_t count)
{
    void *ptr = aalloc_uninit_size(a, size, count);
    memset(ptr, 0, size * count);
    return ptr;
}

void *aalloc_copy_size(arena_t *a, size_t size, size_t count, const void *data)
{
    void *ptr = aalloc_uninit_size(a, size, count);
    arenaimp_copy(ptr, data, size * count);
    return ptr;
}

char *aalloc_copy_str(arena_t *a, const char *str)
{
    size_t count = strlen(str) + 1;
    return aalloc_copy(a, char, count, str);
}

void afree(arena_t *arena, void *ptr)
{
    if (!ptr) return;
	arenaimp_common_header *common = (arenaimp_common_header*)ptr - 1;

    if (!arena) {
        free(common);
        return;
    }

    size_t capacity = common->capacity;
    if (capacity <= ARENAIMP_LARGEST_SIZECLASS - sizeof(arenaimp_small_header)) {
        arenaimp_small_header *alloc = (arenaimp_small_header*)ptr - 1;
        arenaimp_t *a = (arenaimp_t*)arena;
        size_t total_small = capacity + sizeof(arenaimp_small_header);
        size_t quantized = (total_small + ARENAIMP_SIZECLASS_QUANTIZATION - 1) / ARENAIMP_SIZECLASS_QUANTIZATION;
        size_t sizeclass = arenaimp_size_to_class[quantized];
        alloc->freed.next_free = a->next_free[sizeclass];
        a->next_free[sizeclass] = alloc;
    } else {
        arenaimp_big_header *alloc = (arenaimp_big_header*)ptr - 1;
        arenaimp_big_header *prev = alloc->prev, *next = alloc->next;
        assert(prev->next == alloc && next->prev == alloc);
        prev->next = next;
        next->prev = prev;
    }
}

size_t aalloc_capacity_bytes(void *ptr)
{
    if (!ptr) return 0;
	arenaimp_common_header *common = (arenaimp_common_header*)ptr - 1;
    return common->capacity;
}

void *arealloc_size(arena_t *arena, size_t size, size_t count, void *ptr)
{
    if (!ptr) {
		if (count == 0) return NULL;
        return aalloc_uninit_size(arena, size, count);
    }
	arenaimp_common_header *common = (arenaimp_common_header*)ptr - 1;
    size_t capacity = common->capacity;

    size_t total = size * count;
    if (total <= capacity) return ptr;

    size_t new_cap = capacity * 2;
    if (total >= new_cap) new_cap = total;

    void *new_ptr = aalloc_uninit_size(arena, 1, new_cap);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, capacity);

    afree(arena, ptr);

    return new_ptr;
}

typedef struct {
    void *data;
    size_t count;
} arenaimp_alist;

void *alist_push_size(arena_t *arena, size_t size, void *p_list, void *item)
{
    arenaimp_alist *list = (arenaimp_alist*)p_list;
    size_t index = list->count;
    void *ptr = arealloc_size(arena, size, index + 1, list->data);
    if (!ptr) return NULL;
    if (ptr != list->data) list->data = ptr;
    void *dst = (char*)ptr + index * size;
    arenaimp_copy(dst, item, size);
    list->count = index + 1;
    return dst;
}

void *alist_pop_size(size_t size, void *p_list)
{
    arenaimp_alist *list = (arenaimp_alist*)p_list;
    assert(list->count > 0);
    if (list->count == 0) return NULL;
    size_t index = list->count - 1;
    list->count = index;
    return (char*)list->data + index * size;
}

void *alist_push_n_size(arena_t *arena, size_t size, void *p_list, size_t n, void *p_item)
{
    arenaimp_alist *list = (arenaimp_alist*)p_list;
    size_t index = list->count;
    void *ptr = arealloc_size(arena, size, index + n, list->data);
    if (!ptr) return NULL;
    if (ptr != list->data) list->data = ptr;
    void *dst = (char*)ptr + index * size;
    arenaimp_copy(dst, p_item, size);
    list->count = index + n;
    return dst;
}

void *alist_pop_n_size(size_t size, void *p_list, size_t n)
{
    arenaimp_alist *list = (arenaimp_alist*)p_list;
    assert(list->count >= n);
    if (list->count < n) return NULL;
    size_t index = list->count - n;
    list->count = index;
    return (char*)list->data + index * size;
}

bool alist_remove_size(size_t size, void *p_list, size_t index)
{
    arenaimp_alist *list = (arenaimp_alist*)p_list;
    assert(index < list->count);
    if (index >= list->count) return false;
	size_t last = --list->count;
    if (index != last) {
		void *src = (char*)list->data + index * size;
		void *dst = (char*)list->data + last * size;
        memcpy(dst, src, size);
    }
    return true;
}
