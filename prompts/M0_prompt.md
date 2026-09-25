# M0 —— 地基

**状态：已完成**

---

## 本阶段目标

建立项目骨架、基础类型、内存管理（arena / pool / 动态数组 / 哈希表）、
数学库（vec2/3/4、mat3/4、quat、变换、投影）、日志、断言、单元测试框架。

## 验收标准

| 项 | 要求 | 实际 |
|---|---|---|
| 构建 | CMake + Ninja + clang 通过 | ✅ Debug / Release / `-Werror` 三档零告警 |
| 数学库测试 | 矩阵乘、逆、四元数旋转、投影全过 | ✅ 100 用例 / 16775 断言 / 0 失败 |
| arena / pool | 无泄漏 | ✅ 追踪分配器断言 `live_blocks == 0` |
| 日志 | 分级输出到控制台 + 文件 | ✅ 控制台带色、文件 64KB 缓冲、WARN 以上带 `file:line` |
| ASSERT | Debug 生效、Release 编译掉 | ✅ CMake 按配置注入 `ENGINE_DEBUG` |
| 语言 | 纯 C11 | ✅ `-std=c11`，GNU 扩展被拒绝 |

## 技术约束

* 纯 C11，不用 C++。
* 命名 `snake_case`，类型 `_t` 后缀。
* `vec3` 16 字节对齐（SIMD 友好）。
* 数学库用 `float`，不用 `double`。
* 不引入任何外部依赖（含 glm）。

## 交付物

```
include/engine/core/{types,assert,console,log,memory,array,hash}.h
include/engine/core/{math,vec,mat,quat,transform}.h
include/engine/core/core.h
include/engine/engine.h
src/core/{assert,console,log,math,memory,array,hash}.c
tests/{test_framework.h,test_framework.c,test_main.c,test_suites.h}
tests/test_{types,math,vec,mat,quat,transform,memory,array,hash}.c
examples/00_smoke/main.c
CMakeLists.txt / CMakePresets.json / tools/env.{sh,cmd}
docs/M0-设计决策.md
```

## 本阶段的坑（实测踩到的，详见 docs/M0-设计决策.md）

1. **`_Alignas` 作用在整条声明上**
   `typedef struct { _Alignas(16) float x, y, z, w; } vec3_t;` 会给 x/y/z/w
   **各自**加 16 字节对齐，`sizeof` 变成 64 而不是 16。
   正确写法是只让第一个成员带 `_Alignas`，其余成员另起一条声明。
   所有布局都用 `_Static_assert` 钉死，改错立刻编译失败。

2. **矩阵转置写成空操作**
   列主序下 `transpose` 的实现如果误用 `from_rows` 再传一遍同样的元素，
   等于原地转置两次，函数退化为"返回原矩阵"。而**旋转矩阵、缩放矩阵、
   对角矩阵的转置都等于自身**，所以在这些矩阵上测试全都通过，完全看不出来。
   必须用非对称矩阵做测试。

3. **视图矩阵少转置一次**
   `look_at` 的旋转部分是 `R^T`（行是 s、u、-f），列主序下
   `col0 = (s.x, u.x, -f.x)`。如果按"`col0 = (s.x, s.y, s.z)`"写，
   存进去的其实是 R。相机沿坐标轴看时 R 恰好对称，测试照样全过；
   只有斜视角才会暴露，表现为"整个世界被斜着拧了一下"。

4. **arena 的对齐必须基于绝对地址**
   块头是 16 字节对齐的，所以块内数据区起点只保证 16 字节对齐。
   当请求 32/64/256 字节对齐时，用"块内偏移向上取整"算出来的指针
   是相对起点对齐、而非绝对地址对齐。只用 16 字节对齐时完全看不出来，
   等 SIMD 一上就崩。

5. **pool 的 `POOL_NIL` 哨兵有歧义**
   用一个值同时表示"槽位被占用"和"空闲链表结束"，会导致链表尾部的那个
   空闲槽被误判成"正在使用"，迭代器多遍历一个元素。
   加一个显式的 `in_use` 字段解决，且不增加结构体大小（对齐填充正好用上）。

6. **哈希表后移删除的游标必须独立前进**
   `j` 是扫描游标，每轮无条件前进；`i` 是空洞位置，只在真的搬移了元素时
   才跟着 `j` 走。如果图省事写成 `j = (i + 1) & mask`，那么当某个元素不该
   被搬移时 `j` 就不前进，循环卡死 —— 表现是**删除操作直接挂住**。

7. **`ASSERT_MSG` 的变参不能直接当格式串**
   宏里没法把 `__VA_ARGS__` 拆成"格式串 + 参数"两段。需要单独提供一个
   变参函数 `engine_assert_fail_fmt(expr, file, line, func, fmt, ...)`。

8. **`math_wrap_angle` 的边界不能用精确比较**
   `-3π` 在 float 下无法精确表示，`fmodf` 会返回与 `-π` 相差 1e-7 的值，
   `if (r == -F32_PI)` 这种精确比较会随机失效。
   改成"先折到 `[0, TAU)` 再折到 `(-PI, PI]`"，全程只有一次比较。

9. **`quat_rotate_vec3` 的展开式只对单位四元数成立**
   传入非单位四元数不会报错，只会让结果既被旋转又被缩放，缩放系数取决于
   模长 —— 极难排查。Debug 下用断言拦住。

10. **MSYS/Git Bash 会把 `D:/foo` 从冒号处截断**
    往 `PATH` 里塞 Windows 风格路径会被路径列表转换逻辑拆成 `D` 和 `/foo`，
    结果是"目录明明存在、工具却找不到"。必须先转成 `/d/foo`。

## 下一阶段

M1 —— Win32 窗口 + OpenGL 4.5 Core Context。
