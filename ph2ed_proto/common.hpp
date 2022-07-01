// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define _NO_CRT_STDIO_INLINE

#include <string.h>

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
static inline int assert_(const char *s) {
    int x = MessageBoxA(0, s, "Assertion Failed", MB_ABORTRETRYIGNORE | MB_ICONERROR | MB_SYSTEMMODAL);
    if (x == 3) ExitProcess(1);
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
template <class T> struct Array {
    int64_t count = 0;
    int64_t capacity = 0;
    T *data = nullptr;

    Array(int64_t count = 0, int64_t capacity = 0, T *data = nullptr) : count { count }, capacity { capacity }, data { data } {}
    void invariants() const {
        assert(count >= 0);
        assert(count <= capacity);
        if (data) {
            assert(capacity);
        } else {
            assert(!capacity);
        }
    }
    T &operator[](int64_t i) {
        invariants();
        assert(i >= 0);
        assert(i < count);
        return data[i];
    }
    const T &operator[](int64_t i) const {
        invariants();
        assert(i >= 0);
        assert(i < count);
        return data[i];
    }
    void clear() {
        invariants();
        count = 0;
    }
    void release() {
        invariants();
        free(data);
        data = nullptr;
        capacity = 0;
        count = 0;
    }
    void amortize(int64_t new_count) {
        invariants();
        if (new_count > capacity) {
            if (capacity < 16) capacity = 16;
            while (new_count > capacity) capacity = capacity * 3 / 2;
            data = (T *)realloc(data, capacity * sizeof(T));
            assert(data); // Yucky!
            ++num_array_resizes;
        }
    }
    void reserve(int64_t new_capacity) {
        invariants();
        assert(new_capacity >= 0);
        amortize(new_capacity);
    }
    void resize(int64_t new_count, T value = {}) {
        invariants();
        amortize(new_count);
        for (int64_t i = count; i < new_count; i += 1) data[i] = value;
        count = new_count;
    }
    Array copy() {
        invariants();
        Array result = {};
        if (count) {
            result.amortize(count);
            memcpy(result.data, data, count * sizeof(T));
        }
        result.count = count;
        return result;
    }
    T *push(T value = {}) {
        invariants();
        amortize(count + 1);
        count += 1;
        data[count - 1] = value;
        return &data[count - 1];
    }
    void pop() {
        invariants();
        assert(count > 0);
        count -= 1;
    }
    T *insert(int64_t index, T value = {}) {
        invariants();
        assert(index < count);
        amortize(count + 1);
        memmove(&data[index + 1], &data[index], (count - index - 1) * sizeof(T));
        count += 1;
        data[index] = value;
        return &data[index];
    }
    void remove(int64_t index) {
        invariants();
        assert(index < count);
        data[index] = data[count - 1];
        count -= 1;
    }
    void remove_ordered(int64_t index) {
        invariants();
        assert(index < count);
        memmove(&data[index], &data[index + 1], (count - index - 1) * sizeof(T));
        count -= 1;
    }
    T *begin() {
        invariants();
        return data;
    }
    T *end() {
        invariants();
        return data + count;
    }
};
