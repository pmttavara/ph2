// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define _NO_CRT_STDIO_INLINE

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#include <Windows.h>

#ifndef defer
struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

template<typename T, size_t N> constexpr size_t countof(T (&)[N]) {
    return N;
}

// assert
#ifdef _WIN32
#ifdef __cplusplus
extern "C" {
#endif
static int assert_(const char *s) {
    int x = MessageBoxA(0, s, "Assertion Failed", MB_ABORTRETRYIGNORE | MB_ICONERROR | MB_SYSTEMMODAL);
    if (x == 3) ExitProcess(1);
    return x == 4;
}
#define assert_STR_(LINE) #LINE
#define assert_STR(LINE) assert_STR_(LINE)
#ifdef NDEBUG
#define assert(ignore) ((void)0)
#else
#define assert(e) ((e) || assert_("At " __FILE__ ":" assert_STR(__LINE__) ":\n\n" #e "\n\nPress Retry to debug.") && (__debugbreak(), 0))
#endif
#ifdef __cplusplus
}
#endif
#else
#include <assert.h>
#endif // _WIN32

#define IM_ASSERT assert
#define SOKOL_ASSERT assert
