// =============================================================================
//  src/core/array.c —— 动态数组实现
// =============================================================================
#include "engine/core/array.h"

#include <string.h>

#include "engine/core/assert.h"

// 起始容量。选 8 而不是 1：绝大多数数组最终都会超过 1 个元素，
// 从 1 开始会经历 1→2→4→8 四次分配，纯粹是浪费。
#define ARRAY_MIN_CAPACITY 8u

bool array_init(array_t *arr, allocator_t *alloc, u32 item_size, u32 initial_capacity)
{
    ASSERT(arr != NULL);
    ASSERT(alloc != NULL);
    ASSERT(item_size > 0);

    memset(arr, 0, sizeof(*arr));
    arr->item_size = item_size;
    arr->alloc     = alloc;

    if (initial_capacity > 0) {
        return array_reserve(arr, initial_capacity);
    }
    return true;
}

void array_destroy(array_t *arr)
{
    if (arr == NULL) {
        return;
    }

    if (arr->data != NULL && arr->alloc != NULL) {
        arr->alloc->free(arr->alloc, arr->data, (usize)arr->capacity * arr->item_size);
    }
    memset(arr, 0, sizeof(*arr));
}

bool array_reserve(array_t *arr, u32 capacity)
{
    ASSERT(arr != NULL);

    if (capacity <= arr->capacity) {
        return true;
    }

    u32 new_capacity = (arr->capacity > 0) ? arr->capacity : ARRAY_MIN_CAPACITY;
    while (new_capacity < capacity) {
        if (new_capacity > (UINT32_MAX / 2u)) {
            // 再翻倍会溢出。直接取请求值，让上层的一次大分配去暴露问题，
            // 而不是在这里静默地算出一个小得多的容量。
            new_capacity = capacity;
            break;
        }
        new_capacity *= 2u;
    }

    usize bytes    = (usize)new_capacity * arr->item_size;
    void *new_data = arr->alloc->alloc(arr->alloc, bytes, ARRAY_ITEM_ALIGNMENT);
    if (new_data == NULL) {
        return false;
    }

    if (arr->data != NULL) {
        memcpy(new_data, arr->data, (usize)arr->size * arr->item_size);
        arr->alloc->free(arr->alloc, arr->data, (usize)arr->capacity * arr->item_size);
    }

    arr->data     = new_data;
    arr->capacity = new_capacity;
    return true;
}

bool array_resize(array_t *arr, u32 size)
{
    ASSERT(arr != NULL);

    if (size > arr->capacity) {
        if (!array_reserve(arr, size)) {
            return false;
        }
    }

    if (size > arr->size) {
        // 新元素清零：与 pool_alloc 同样的理由，确定性优于省这点 memset。
        memset((u8 *)arr->data + (usize)arr->size * arr->item_size, 0,
               (usize)(size - arr->size) * arr->item_size);
    }

    arr->size = size;
    return true;
}

void array_clear(array_t *arr)
{
    ASSERT(arr != NULL);
    arr->size = 0;
}

void *array_push_uninit(array_t *arr)
{
    ASSERT(arr != NULL);

    if (arr->size == arr->capacity) {
        u32 new_capacity;
        if (arr->capacity == 0) {
            new_capacity = ARRAY_MIN_CAPACITY;
        } else if (arr->capacity > (UINT32_MAX / 2u)) {
            return NULL; // 无法再扩容
        } else {
            new_capacity = arr->capacity * 2u;
        }

        if (!array_reserve(arr, new_capacity)) {
            return NULL;
        }
    }

    void *slot = (u8 *)arr->data + (usize)arr->size * arr->item_size;
    arr->size++;
    return slot;
}

void array_pop_uninit(array_t *arr)
{
    ASSERT(arr != NULL);
    if (arr->size > 0) {
        arr->size--;
    }
}

void *array_at(const array_t *arr, u32 index)
{
    ASSERT(arr != NULL);
    ASSERT(index < arr->size);

    // Release 下没有断言，这里补一次真实检查。
    // 返回 NULL 而不是野指针，让越界访问变成一个"能立刻看到"的空指针解引用。
    if (arr == NULL || index >= arr->size) {
        return NULL;
    }
    return (u8 *)arr->data + (usize)index * arr->item_size;
}

u32 array_size(const array_t *arr)
{
    ASSERT(arr != NULL);
    return arr->size;
}

u32 array_capacity(const array_t *arr)
{
    ASSERT(arr != NULL);
    return arr->capacity;
}

u32 array_item_size(const array_t *arr)
{
    ASSERT(arr != NULL);
    return arr->item_size;
}

bool array_is_empty(const array_t *arr)
{
    ASSERT(arr != NULL);
    return arr->size == 0;
}

void *array_data(const array_t *arr)
{
    ASSERT(arr != NULL);
    return arr->data;
}
