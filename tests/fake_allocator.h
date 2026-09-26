// =============================================================================
//  tests/fake_allocator.h —— 会在指定次数上失败的分配器
//
//  为什么要它：M0 的规划要求负面用例覆盖"分配失败"。而仓库里唯一的包装器
//  `allocator_tracking_t` 只**计数**、不会失败，于是所有"分配失败怎么办"的分支
//  一次都没被执行过 —— 那些分支的契约通常是"返回 false 并且不留半份状态",
//  而这件事只有真的失败一次才看得见。编译器不管，review 更容易放过去。
//
//  用法：`fake_allocator_init(&f, 3)` 后把 `&f.base` 交给被测对象，
//  于是第 3 次 alloc 起返回 NULL。想全部成功就传 FAKE_ALLOC_NEVER。
//
//  为什么是"第 N 次之后失败"而不是"一律失败"：真实的失败点几乎都在中间 ——
//  init 已经拿到一块元数据、第二块失败，那才是"会不会回滚第一块"的场景。
//  一律失败只能验到最外面那一层，而那层几乎不会写错。
//
//  实现放在头文件里（static inline）：它只有十几行，单独一个 .c 还要进
//  tests/CMakeLists.txt 的源文件清单，而那个清单是按"一个测试一套"排的。
// =============================================================================
#ifndef MYENGINE_TESTS_FAKE_ALLOCATOR_H
#define MYENGINE_TESTS_FAKE_ALLOCATOR_H

#include <stdlib.h>
#include <string.h>

#include "engine/core/memory.h"

#define FAKE_ALLOC_NEVER ((usize)0xFFFFFFFFFFFFFFFFull)
#define FAKE_ALLOC_MAX_LIVE 48u

typedef struct fake_allocator_t {
    allocator_t base;
    usize       calls;    // alloc 被调了几次（含被拒的）
    usize       failures; // 其中被我们拒掉了几次
    usize       live;     // 还挂着的块数
    usize       fail_from_call;
    void       *blocks[FAKE_ALLOC_MAX_LIVE]; // 收尾时全部归还
} fake_allocator_t;

static void *fake_alloc(allocator_t *self, usize size, usize alignment)
{
    fake_allocator_t *f = (fake_allocator_t *)self->ctx;
    f->calls++;
    ENGINE_UNUSED(alignment);
    if (f->calls >= f->fail_from_call || f->live + 1u > FAKE_ALLOC_MAX_LIVE) {
        f->failures++;
        return NULL;
    }
    void *p = malloc(size == 0u ? 1u : size);
    if (p == NULL) {
        f->failures++;
        return NULL;
    }
    for (usize i = 0u; i < FAKE_ALLOC_MAX_LIVE; ++i) {
        if (f->blocks[i] == NULL) {
            f->blocks[i] = p;
            break;
        }
    }
    f->live++;
    return p;
}

static void fake_free(allocator_t *self, void *ptr, usize size)
{
    fake_allocator_t *f = (fake_allocator_t *)self->ctx;
    if (ptr == NULL) {
        return;
    }
    ENGINE_UNUSED(size);
    for (usize i = 0u; i < FAKE_ALLOC_MAX_LIVE; ++i) {
        if (f->blocks[i] == ptr) {
            f->blocks[i] = NULL;
            break;
        }
    }
    f->live--;
    free(ptr);
}

static void fake_allocator_init(fake_allocator_t *f, usize fail_from_call)
{
    memset(f, 0, sizeof(*f));
    f->base.alloc = fake_alloc;
    f->base.free = fake_free;
    f->base.ctx = f;
    f->fail_from_call = fail_from_call;
}

// 归还所有还挂着的块。收尾时必须调，否则这个 fake 自己就成了测试里的泄漏。
static void fake_allocator_drain(fake_allocator_t *f)
{
    for (usize i = 0u; i < FAKE_ALLOC_MAX_LIVE; ++i) {
        if (f->blocks[i] != NULL) {
            free(f->blocks[i]);
            f->blocks[i] = NULL;
            f->live--;
        }
    }
}

#endif // MYENGINE_TESTS_FAKE_ALLOCATOR_H
