// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define _NO_CRT_STDIO_INLINE

#include <string.h>
#include <stdio.h>

#ifndef NDEBUG
#include "stb_leakcheck.h"
static inline void *stb_leakcheck_calloc(size_t n, size_t s, const char *file, int line) {
    size_t bytes = n * s;
    void *result = stb_leakcheck_malloc(bytes, file, line);
    if (result) {
        memset(result, 0, bytes);
    }
    return result;
}
#define calloc(n, s) stb_leakcheck_calloc(n, s, __FILE__, __LINE__)
static inline char *stb_leakcheck_strdup(const char *s) {
    if (!s) {
        return nullptr;
    }
    size_t n = strlen(s) + 1;
    char *result = (char *)malloc(n);
    if (!result) {
        return nullptr;
    }
    memcpy(result, s, n);
    return result;
}
#define strdup stb_leakcheck_strdup
#else
#include <stdlib.h>
#endif


#ifndef defer
struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

template <typename T, typename U, typename V> auto clamp(T &&t, U &&u, V &&v) {
    // assert(u <= v);
    return t < u ? u : t > v ? v : t;
}
template <typename T, typename U> auto max(T &&t, U &&u) {
    return t > u ? t : u;
}
template <typename T, typename U> auto min(T &&t, U &&u) {
    return t < u ? t : u;
}

template<typename T, size_t N> constexpr size_t countof(T (&)[N]) {
    return N;
}

// #define offsetof(T, m) ((size_t)&(((T *)0)->m))

// assert
#ifdef _WIN32
#ifdef __cplusplus
extern "C" {
#endif
#pragma comment(linker, "/alternatename:__imp_my_MessageBoxA=__imp_MessageBoxA")
#pragma comment(linker, "/alternatename:__imp_my_ExitProcess=__imp_ExitProcess")
extern __declspec(dllimport) int __stdcall my_MessageBoxA(void *hWnd, const char *lpText, const char *lpCaption, unsigned int uType);
extern __declspec(dllimport) void __stdcall my_ExitProcess(unsigned int uExitCode);
static inline int assert_(const char *s) {
    int x = my_MessageBoxA(0, s, "Assertion Failed", 0x1012);
    if (x == 3) my_ExitProcess(1);
    return x == 4;
}
#define assert_STR_(LINE) #LINE
#define assert_STR(LINE) assert_STR_(LINE)
#undef assert
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

#include <stdint.h>

extern int num_array_resizes;

struct Mallocator {
    static void *reallocate(void *p, size_t size, size_t old_size) {
        (void)old_size;
        return ::realloc(p, size);
    }
    static void (free)(void *p, size_t size) {
        (void)size;
        return ::free(p);
    }
};

template <class T, class Allocator = Mallocator> struct Array {
    int64_t count = 0;
    int64_t capacity = 0;
    T *data = nullptr;

    Array(int64_t count = 0, int64_t capacity = 0, T *data = nullptr) : count { count }, capacity { capacity }, data { data } {}
#define array_invariants() (count >= 0 && count <= capacity && !data == !capacity)
    T &operator[](int64_t i) {
        assert(i >= 0 && i < count && count <= capacity && data);
        return data[i];
    }
    const T &operator[](int64_t i) const {
        assert(i >= 0 && i < count && count <= capacity && data);
        return data[i];
    }
    void clear() {
        assert(array_invariants());
        count = 0;
    }
    void release() {
        assert(array_invariants());
        if (data) {
            (Allocator::free)(data, capacity * sizeof(T));
        }
        data = nullptr;
        capacity = 0;
        count = 0;
    }
    void amortize(int64_t new_count) {
        assert(array_invariants());
        if (new_count > capacity) {
            auto old_capacity = capacity;
            if (capacity < 16) capacity = 16;
            while (new_count > capacity) capacity = capacity * 3 / 2;
            data = (T *)Allocator::reallocate(data, capacity * sizeof(T), old_capacity * sizeof(T));
            assert(data); // Yucky!
            ++num_array_resizes;
        }
    }
    void reserve(int64_t new_capacity) {
        assert(array_invariants() && new_capacity >= 0);
        amortize(new_capacity);
    }
    void resize(int64_t new_count, T value = {}) {
        assert(array_invariants());
        amortize(new_count);
        for (int64_t i = count; i < new_count; i += 1) data[i] = value;
        count = new_count;
    }
    Array copy() {
        assert(array_invariants());
        Array result = {};
        if (count) {
            result.amortize(count);
            memcpy(result.data, data, count * sizeof(T));
        }
        result.count = count;
        return result;
    }
    T *push(T value = {}) {
        assert(array_invariants());
        amortize(count + 1);
        count += 1;
        data[count - 1] = value;
        return &data[count - 1];
    }
    T pop() {
        assert(array_invariants() && count > 0);
        count -= 1;
        return data[count];
    }
    T *insert(int64_t index, T value = {}) {
        assert(array_invariants() && index < count);
        amortize(count + 1);
        memmove(&data[index + 1], &data[index], (count - index - 1) * sizeof(T));
        count += 1;
        data[index] = value;
        return &data[index];
    }
    void remove(int64_t index) {
        assert(array_invariants() && index < count);
        data[index] = data[count - 1];
        count -= 1;
    }
    void remove_ordered(int64_t index, int64_t how_many = 1) {
        assert(array_invariants() && index < count && how_many >= 0 && index + how_many <= count);
        memmove(&data[index], &data[index + how_many], (count - index - how_many) * sizeof(T));
        count -= how_many;
    }
    T *begin() {
        assert(array_invariants());
        return data;
    }
    T *end() {
        assert(array_invariants());
        return data + count;
    }
};

#include <wchar.h>
static inline uint16_t *utf8_to_utf16(const char *utf8) {
    uint16_t *utf16 = nullptr;
    mbstate_t state = {0};
    size_t utf16len = mbsrtowcs(nullptr, &utf8, 0, &state) + 1;
    if (utf16len) { //may fail if the input string is invalid, so this is a handle-able error
        utf16 = (uint16_t *)malloc(utf16len * sizeof(uint16_t));
        if (utf16) {
            size_t charsWritten = mbsrtowcs((wchar_t *)utf16, &utf8, utf16len, &state);
            if (charsWritten != utf16len || //should always be true
                utf16[utf16len - 1] != 0) { //should be null terminated
                free(utf16);
                utf16 = nullptr;
            }
        }
    }
    return utf16;
}

static inline char *utf16_to_utf8(const uint16_t *utf16) {
    char *utf8 = nullptr;
    mbstate_t state = {0};
    size_t utf8len = wcsrtombs(nullptr, (const wchar_t **)utf16, 0, &state) + 1;
    if (utf8len) { //may fail if the input string is invalid, so this is a handle-able error
        utf8 = (char *)malloc(utf8len * sizeof(char));
        if (utf8) {
            size_t charsWritten = wcsrtombs(utf8, (const wchar_t **)utf16, utf8len, &state);
            if (charsWritten != utf8len || //should always be true
                utf8[utf8len - 1] != 0) { //should be null terminated
                free(utf8);
                utf8 = nullptr;
            }
        }
    }
    return utf8;
}

#include <stdarg.h>
static inline char *mprintf(const char *fmt, ...) {
    va_list arg1;
    va_list arg2;
    va_start(arg1, fmt);
    va_copy(arg2, arg1);
    defer {
        va_end(arg1);
        va_end(arg2);
    };
    int n = vsnprintf(nullptr, 0, fmt, arg1);
    if (n < 0) {
        return nullptr;
    }
    char *result = (char *)malloc(n + 1);
    if (!result) {
        return nullptr;
    }
    vsnprintf(result, n + 1, fmt, arg2);
    return result;
}
