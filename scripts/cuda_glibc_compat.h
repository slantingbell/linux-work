/*
 * cuda_glibc_compat.h —— 让 CUDA 13.x 能在 glibc >= 2.41 上编译
 *
 * 问题：
 *   glibc 2.41 起按 C23 标准声明了 rsqrt()/rsqrtf()，在 C++ 里 __THROW 展开成
 *   `noexcept (true)`：
 *       extern float rsqrtf (float __x) noexcept (true);
 *   而 CUDA 的 crt/math_functions.h 里同名函数没有异常规格：
 *       extern __device_builtin__ float rsqrtf(float x);
 *   C++17 之后 noexcept 属于函数类型的一部分，两者不兼容，nvcc 的宿主编译
 *   阶段直接报错：
 *       error: exception specification is incompatible with that of
 *              previous function "rsqrtf"
 *
 * 做法：
 *   本头文件通过 `-Xcompiler=-include` 强制包含在所有 .cu 的宿主编译最前面。
 *   它在 <features.h>（进而 sys/cdefs.h）之后把 __THROW 清空，于是后面才被
 *   包含的 mathcalls.h 里的声明就不再带 noexcept，与 CUDA 的声明一致。
 *
 *   用 -Xcompiler 传参意味着这个头文件只作用于 nvcc 的宿主编译器那一趟，
 *   不进入设备端（device pass）编译，因此不会影响核函数。
 *
 * 为什么不用改 CUDA 自带头文件：
 *   /usr/local/cuda/... 是 root 所有，改它需要 sudo，而且升级 CUDA 就被覆盖。
 *   在项目里带一个 shim 更干净、可重复。
 *
 * 只在 glibc >= 2.41 且 C++ 模式下生效；其他 libc（musl/macOS/Windows）不受影响。
 */
#ifndef LLM_CHAT_CUDA_GLIBC_COMPAT_H
#define LLM_CHAT_CUDA_GLIBC_COMPAT_H

#include <features.h>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && defined(__cplusplus)
#  if __GLIBC_PREREQ(2, 41)
     /* 先让 sys/cdefs.h 把 __THROW 定义出来（features.h 的包含保护保证
        后面不会再定义一次），再清掉它的异常规格。 */
#    undef __THROW
#    define __THROW
#    undef __THROWNL
#    define __THROWNL
#  endif
#endif

#endif /* LLM_CHAT_CUDA_GLIBC_COMPAT_H */
