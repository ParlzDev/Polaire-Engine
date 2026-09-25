// =============================================================================
//  engine/core/array.h —— 动态数组
//
//  命名说明：原始需求里这个模块叫 vec，但它与数学库的 vec2/vec3/vec4 撞名。
//  一个项目里出现 `vec` 和 `vec3` 两种完全不同的东西，读代码的人迟早会看错。
//  这里改名为 array，数学库保留 vec* 前缀。详见 docs/M0-设计决策.md。
//
//  类型安全策略：
//  容器本体是"字节数组 + 元素大小"，运行时泛型（一份实现，代码量小、好测）；
//  元素访问通过 ARRAY_PUSH / ARRAY_AT 等宏在调用点做类型转换。
//  这是 C 里在"零重复代码"和"类型安全"之间的常规折中，代价是类型不匹配时
//  报错位置在调用点而非容器实现里。
// =============================================================================
#ifndef ENGINE_CORE_ARRAY_H
#define ENGINE_CORE_ARRAY_H

#include "engine/core/memory.h"
#include "engine/core/types.h"

typedef struct array_t {
    void        *data;
    u32          size;      // 当前元素个数
    u32          capacity;  // 已分配的元素容量
    u32          item_size;
    allocator_t *alloc;
} array_t;

// 所有元素按 16 字节对齐分配，因为最大对齐需求来自 vec4/mat4。
// 元素本身是否真的需要 16 字节对齐由调用方决定，多出来的填充可以忽略。
#define ARRAY_ITEM_ALIGNMENT 16u

bool array_init(array_t *arr, allocator_t *alloc, u32 item_size, u32 initial_capacity);
void array_destroy(array_t *arr);

// 确保容量 >= capacity。容量只增不减。
bool array_reserve(array_t *arr, u32 capacity);

// 调整元素个数。新增元素被零初始化。
bool array_resize(array_t *arr, u32 size);

// 清空（size = 0），保留已分配内存。
void array_clear(array_t *arr);

// 追加一个未初始化的元素，返回其地址；失败返回 NULL。
void *array_push_uninit(array_t *arr);

// 移除最后一个元素。空数组时无操作。
void array_pop_uninit(array_t *arr);

// 越界访问在 Debug 下断言，Release 下不检查（性能敏感路径靠调用方自己保证）。
void *array_at(const array_t *arr, u32 index);

u32   array_size(const array_t *arr);
u32   array_capacity(const array_t *arr);
u32   array_item_size(const array_t *arr);
bool  array_is_empty(const array_t *arr);
void *array_data(const array_t *arr);

// ---------------------------------------------------------------- 类型化语法糖
#define ARRAY_AT(arr, T, index) (*(T *)array_at((arr), (u32)(index)))

#define ARRAY_PUSH(arr, T, value)                                                                  \
    do {                                                                                           \
        T *_engine_push_slot = (T *)array_push_uninit(arr);                                        \
        if (_engine_push_slot != NULL) {                                                           \
            *_engine_push_slot = (value);                                                          \
        }                                                                                          \
    } while (0)

#define ARRAY_POP(arr, T) (*(T *)array_at((arr), array_size(arr) - 1u))

// 正向遍历：
//     ARRAY_FOREACH(&list, int, it) { printf("%d\n", *it); }
#define ARRAY_FOREACH(arr, T, it)                                                                  \
    for (T *it = (T *)array_data(arr); it != (T *)array_data(arr) + array_size(arr); ++it)

// 需要删除元素时用索引遍历（倒序保证删除后下标仍有效）：
//     for (i32 i = (i32)array_size(a) - 1; i >= 0; --i) { ... }
#define ARRAY_FOREACH_INDEX(arr, index_var)                                                        \
    for (u32 index_var = 0; index_var < array_size(arr); ++index_var)

#endif // ENGINE_CORE_ARRAY_H
