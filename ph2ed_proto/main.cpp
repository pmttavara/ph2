// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

#ifdef NDEBUG
#include "libs.cpp"
#endif

#include "common.hpp"

int num_array_resizes = 0;

// Path to MVP:
// - MAP mesh vertex dragging
// - MAP mesh vertex snapping
// - Better move UX

// FINISHED on Path to MVP:
// - MAP mesh vertex snapping
//      - cast triangles
//      - optimize
//      - BVH
// - Multimesh movement/deleting/editing
// - OBJ export
// - Undo/redo
// - If I save and reload a map with a custom texture, it's moshed - OMG!!!
//    -> Specific to when I edited and re-saved the DDS from Paint.NET. Punt!
//    -> Fixed!
//  - View any surface as a solid/shaded set of triangles with a wireframe overlaid
//  - View any surface as only solid, only shaded, only wireframe, only vertex colours etc.

// With editor widgets, you should probably be able to:
//  - Box-select a group of vertices or edges
//      - Drag that selection as a group in any direction (with or without axis- and plane-alignment)
//      - Rotate that selection around its center of geometry (with or without plane-alignment)
//      - Scale that selection (with or without axis- and plane-alignment)
//      - Delete that selection and gracefully handle the results of that deletion (removing degenerate faces etc.)
//      - (Remember: these all need to include cylinders somehow!)
//      - Render the AABB of all selected things


//  - Lesson learned from Happenlance editor: Definitely need a base "Transform" struct so that you aren't
//    reimplementing the same logic for N different "widget kinds" (in happenlance editor this was
//    a huge pain because objects, sprites, particle emitters etc. all had scattered transform data.
//    Unity Engine and friends absolutely make the right decision to have Transforms be fundamental bases to all
//    editable things!)

#define TAU 6.283185307179586476925
#define TAU32 6.283185307179586476925f

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#include <Windows.h>

#define SPALL_BUFFER_PROFILING
#define SPALL_BUFFER_PROFILING_GET_TIME() ((double)__rdtsc())
#include "spall.h"

#pragma comment(lib, "legacy_stdio_definitions")
#pragma comment(lib, "comdlg32")

SpallProfile spall_ctx = {};
char spall_buffer_data[1 << 16];
SpallBuffer spall_buffer = {spall_buffer_data, sizeof(spall_buffer_data)};

extern "C" __declspec(dllimport) unsigned long GetCurrentThreadId(void);

struct ScopedTraceTimer {
    SPALL_FORCEINLINE constexpr explicit operator bool() { return false; }
    template <size_t N> 
    SPALL_FORCEINLINE ScopedTraceTimer(const char (&name)[N]) {
        spall_buffer_begin_ex(&spall_ctx, &spall_buffer, name, N - 1, (double)__rdtsc(), GetCurrentThreadId(), 0);
    }
    SPALL_FORCEINLINE ~ScopedTraceTimer() {
        spall_buffer_end_ex(&spall_ctx, &spall_buffer, (double)__rdtsc(), GetCurrentThreadId(), 0);
    }
};

#define CAT__(a, b) a ## b
#define CAT_(a, b) CAT__(a, b)
#define CAT(a, b) CAT_(a, b)

#define ProfileScope(name) ScopedTraceTimer CAT(zz_profile, __COUNTER__) (name)
#define ProfileFunction() ScopedTraceTimer CAT(zz_profile, __COUNTER__) (__FUNCSIG__)

// Dear Imgui
#ifndef IMGUI_VERSION
#include "imgui.h"
#include "imgui_internal.h"
#endif

// Sokol libraries
#ifndef SOKOL_APP_IMPL_INCLUDED
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"
#include "sokol_log.h"
#endif

#include "zip.h"

#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

struct LogMsg {
    unsigned int colour;// = IM_COL32(127, 127, 127, 255);
    char buf[252];// = {};
};
enum { LOG_MAX = 16384 };
LogMsg log_buf[LOG_MAX];
int log_buf_index = 0;
void LogC_(uint32_t c, const char *fmt, ...) {
    ProfileFunction();

    log_buf[log_buf_index % LOG_MAX].colour = c;
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_buf[log_buf_index % LOG_MAX].buf, sizeof(log_buf[0].buf), fmt, args);
    va_end(args);
    printf("%.*s\n", (int)sizeof(log_buf[0].buf), log_buf[log_buf_index % LOG_MAX].buf);
    fflush(stdout);
    log_buf_index++;
}
#define LogC(c, ...) LogC_(c, "" __VA_ARGS__)
#define Log(...) LogC(IM_COL32(127,127,127,255), "" __VA_ARGS__)
#define LogErr(...) LogC(IM_COL32(255, 127, 127, 255), "" __VA_ARGS__)

void MsgBox_(const char *title, int flag, const char *msg, ...) {
    ProfileFunction();

    va_list arg;
    va_start(arg, msg);
    defer {
        va_end(arg);
    };
    char buf[1024];
    if (vsnprintf(buf, sizeof buf, msg, arg) > 0) {
        MessageBoxA(0, buf, title, MB_OK | flag | MB_TASKMODAL);
    }
}
#define MsgErr(title, ...) MsgBox_(title, MB_ICONERROR, "" __VA_ARGS__);
#define MsgWarn(title, ...) MsgBox_(title, MB_ICONWARNING, "" __VA_ARGS__);
#define MsgInfo(title, ...) MsgBox_(title, MB_ICONINFORMATION, "" __VA_ARGS__);

#include "HandmadeMath.h"

#include "meshoptimizer.h"
#undef assert
#define assert PH2_assert

#define PH2CLD_IMPLEMENTATION
#include "../cld/ph2_cld.h"
#include <io.h> // @Debug @Temporary @Remove

#include "shaders.glsl.h"
#include <ddraw.h>
#include <commdlg.h>
#include <shellapi.h>

sg_context_desc sapp_sgcontext(void);

#ifdef _WIN32
#pragma warning(disable : 4255)
#pragma warning(disable : 4668)
static inline double get_time(void) {
    LARGE_INTEGER counter;
    static double invfreq;
    if (invfreq == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        invfreq = 1.0 / (double)freq.QuadPart;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * invfreq;
}

// Use the dedicated graphics card
extern "C" {
    __declspec(dllexport) /* DWORD */ uint32_t NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#else
#include <time.h>
static inline double get_time(void) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}
#endif

// How's this done for Linux/etc?
#define KEY(vk) ((unsigned short)GetAsyncKeyState(vk) >= 0x8000)

#include "meow_hash_x64_aesni.h"

uint64_t meow_hash(void *key, int len) {
    ProfileFunction();

    meow_u128 hash = MeowHash(MeowDefaultSeed, len, key);
    auto hash64 = MeowU64From(hash, 0);
    return hash64;
}

extern "C" {
    void __asan_poison_memory_region(void const volatile *addr, size_t size);
    void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
}


struct The_Arena_Allocator {
    static char *arena_data;
    static uint64_t arena_head;
    static uint64_t bytes_used;
    static int allocations_this_frame;
    enum { ARENA_SIZE = 512 * 1024 * 1024 };
    static bool contains(void *p, size_t n) {
        ProfileFunction();

        return (char *)p >= arena_data && (char *)p + n <= arena_data + arena_head;
    }
    static void init() {
        ProfileFunction();

        arena_data = (char *)malloc(ARENA_SIZE); // arena_data = (char *)VirtualAlloc(nullptr, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        arena_head = 0;
        bytes_used = 0;
    }
    static void quit() {
        ProfileFunction();

        free_all();
        ::free(arena_data); // VirtualFree(arena_data, 0, MEM_RELEASE);
        arena_data = nullptr;
    }
    static void free_all() {
        ProfileFunction();

        (free)(arena_data, arena_head);
        arena_head = 0;
        bytes_used = 0;
    }
    static void (free)(void *p, size_t size) {
        ProfileFunction();

        assert((uint64_t)p % 8 == 0);

        size = (size + 7ull) & ~7ull;

        assert(size % 8 == 0);

        assert(contains(p, size));

        // __asan_poison_memory_region(p, size);
#ifndef NDEBUG
        // I'd like to be able to memset invalid regions,
        // but I don't want to slow down release mode,
        // and I don't want to introduce a discrepancy
        // between debug and release undo/redo behaviour,
        // so I'm just not gonna memset right now.
        // memset(p, 0xcc, size);
#endif
        bytes_used -= size;
    }
    static void *allocate(size_t size) {
        ProfileFunction();

        assert(arena_head % 8 == 0);

        size = (size + 7ull) & ~7ull;

        assert(size % 8 == 0);

        if (arena_head + size > ARENA_SIZE) return nullptr;

        void *result = arena_data + arena_head;
        arena_head += size;
#ifndef NDEBUG
        // I'd like to be able to memset invalid regions,
        // but I don't want to slow down release mode,
        // and I don't want to introduce a discrepancy
        // between debug and release undo/redo behaviour,
        // so I'm just not gonna memset right now.
        // memset(result, 0xcc, size);
#endif
        bytes_used += size;
        ++allocations_this_frame;
        // __asan_unpoison_memory_region(result, size);
        return result;
    }
    static void *reallocate(void *p, size_t size, size_t old_size) {
        ProfileFunction();

        assert((uint64_t)p % 8 == 0);

        old_size = (old_size + 7ull) & ~7ull;

        assert(old_size % 8 == 0);

        size = (size + 7ull) & ~7ull;

        assert(size % 8 == 0);

        // No old data - allocate new block
        if (p == NULL || old_size == 0) {
            return allocate(size);
        }

        // New allocation is smaller (or 0) - free the extra space
        if (size <= old_size) {
            (free)((char *)p + size, old_size - size);
            return p;
        }

        // If this allocation is at the end of the arena, then
        // grow forward.
        if ((char *)p + old_size == arena_data + arena_head) {
            auto more = size - old_size;
            assert(more % 8 == 0);
            auto extra = allocate(more);
            assert(arena_head % 8 == 0);
            if (extra) {
                assert((uint64_t)extra % 8 == 0);
                assert(extra == (char *)p + old_size);
                return p;
            }
            return nullptr;
        }

        // @Todo: If there is extra space to grow into, then
        // grow into that instead of allocating a new block.

        // Allocate new block and copy data
        void *q = allocate(size);
        if (q) {
            memcpy(q, p, old_size);
            // Free old allocation
            (free)(p, old_size);
        }
        return q;
    }
};
char *The_Arena_Allocator::arena_data;
uint64_t The_Arena_Allocator::arena_head;
uint64_t The_Arena_Allocator::bytes_used;
int The_Arena_Allocator::allocations_this_frame;

struct Ray {
    Ray() = default;
    Ray(HMM_Vec3 pos, HMM_Vec3 dir) : pos{pos}, dir{dir} {}
    HMM_Vec3 pos = {};
    HMM_Vec3 dir = {};
};

const float FOV_MIN = 10 * (TAU32 / 360);
const float FOV_MAX = 179 * (TAU32 / 360);
const float FOV_DEFAULT = 90 * (TAU32 / 360);
const HMM_Vec3 BG_COL_DEFAULT = { 0.125f, 0.125f, 0.125f };
const float MOVE_SPEED_DEFAULT = -2;
const float MOVE_SPEED_MAX = 6;
struct CLD_Face_Buffer {
    sg_buffer buf = {};
    int num_vertices = 0;
};

struct Node {
    Node *next = nullptr;
    Node *prev = nullptr;
};
static void node_insert_after(Node *dest, Node *node) {
    ProfileFunction();

    node->next = dest->next;
    node->prev = dest;
    dest->next->prev = node;
    dest->next = node;
}
static void node_insert_before(Node *dest, Node *node) {
    ProfileFunction();

    node->next = dest;
    node->prev = dest->prev;
    dest->prev->next = node;
    dest->prev = node;
}

struct MAP_Geometry_Vertex {
    float position[3] = {};
    float normal[3] = {};
    uint32_t color = 0xffffffff;
    float uv[2] = {};
};
namespace MAP_Geometry_Buffer_Source_ {
    enum MAP_Geometry_Buffer_Source {
        Opaque,
        Transparent,
        Decal,
    };
}
typedef MAP_Geometry_Buffer_Source_::MAP_Geometry_Buffer_Source MAP_Geometry_Buffer_Source;
struct MAP_Geometry_Buffer {
    sg_buffer vertex_buffer = {};
    sg_buffer index_buffer = {};
    MAP_Geometry_Buffer_Source source = MAP_Geometry_Buffer_Source::Opaque;
    Array<MAP_Geometry_Vertex> vertices = {}; // @Todo: convert to local variable that gets released once uploaded
    Array<uint32_t> indices = {};
    Array<int> vertices_per_mesh_part_group = {}; // parallel with mesh part groups; indicates how many triangles in each group.
    uint32_t id = 0;
    uint32_t subfile_index = 0;
    struct MAP_Geometry *geometry_ptr = nullptr;
    struct MAP_Mesh *mesh_ptr = nullptr;
    int mesh_index = 0;
    bool shown = true;
    bool selected = false; // Used by Imgui
    void release() {
        ProfileFunction();

        sg_destroy_buffer(index_buffer);
        sg_destroy_buffer(vertex_buffer);
        vertices.release();
        indices.release();
        vertices_per_mesh_part_group.release();
        *this = {};
    }
};
struct MAP_Texture_Buffer {
    sg_image tex = {};
    int width = 0;
    int height = 0;
    struct MAP_Texture_Subfile *subfile_ptr = nullptr;
    struct MAP_Texture *texture_ptr = nullptr;

    void release() {
        ProfileFunction();

        if (tex.id) {
            sg_destroy_image(tex);
        }
        *this = {};
    }
};
struct MAP_Mesh_Vertex_Buffer {
    int bytes_per_vertex;
    Array<char, The_Arena_Allocator> data;
    int num_vertices;

    void release() {
        ProfileFunction();

        data.release();
    }
};
struct MAP_Mesh_Part {
    int strip_length;
    int strip_count;

    bool was_inverted; // Only for roundtrippability.
};

template <class T, class Allocator = Mallocator>
struct LinkedList {
    Node *sentinel = nullptr; // @Todo @Temporary: by value node, no pointer

    bool empty() {
        return !sentinel || sentinel->next == sentinel;
    }

    int64_t count() {
        if (!sentinel) {
            return 0;
        }
        int64_t result = 0;
        for (Node *node = sentinel->next; node != sentinel; node = node->next) {
            ++result;
        }
        return result;
    }
    T *at_index(int64_t index) {
        assert(sentinel);

        int64_t i = 0;
        for (Node *node = sentinel->next; node != sentinel; node = node->next) {
            if (i == index) {
                return (T *)node;
            }
            ++i;
        }

        return nullptr;
    }

    int64_t get_index(T *ptr) {
        assert(sentinel);

        int64_t i = 0;
        for (Node *node = sentinel->next; node != sentinel; node = node->next) {
            if (node == ptr) {
                return i;
            }
            ++i;
        }

        return -1;
    }

    void init() {
        sentinel = (T *)Allocator::allocate(sizeof(T));
        assert(sentinel);
        sentinel->next = sentinel;
        sentinel->prev = sentinel;
    }
    void release() {
        ProfileFunction();

        if (sentinel) {
            for (Node *node = sentinel->prev; node != sentinel;) {
                Node *prev = node->prev;
                (Allocator::free)(static_cast<T *>(node), sizeof(T));
                node = prev;
            }
            (Allocator::free)(sentinel, sizeof(Node));
        }
        *this = {};
    }
    T *push(T value = {}) {
        if (!sentinel) {
            init();
        }
        T *node = (T *)Allocator::allocate(sizeof(T));
        assert(node);
        *node = value;
        node_insert_before(sentinel, node);
        return (T *)node;
    }

    struct Iterator {
        Node *sentinel;
        T *node;
        T &operator*() {
            assert(node);
            assert(node != sentinel);
            return *node;
        }
        T *operator->() {
            assert(node);
            assert(node != sentinel);
            return node;
        }
        Iterator &operator++() {
            assert(node);
            node = (T *)node->next;
            return *this;
        }
        bool operator!=(const Iterator &rhs) {
            return node != rhs.node;
        }
    };

    Iterator begin() {
        if (sentinel) {
            return {sentinel, (T *)sentinel->next};
        }
        return {};
    }
    Iterator end() {
        if (sentinel) {
            return {sentinel, (T *)sentinel};
        }
        return {};
    }


    bool has_node(Node *node) {
        if (sentinel) {
            for (auto &&it : *this) {
                if (&it == node) {
                    return true;
                }
            }
        }
        return false;
    }

    void remove_ordered(Node *node) {
        assert(sentinel && has_node(node));
        node->prev->next = node->next;
        node->next->prev = node->prev;
    }

};

struct MAP_Mesh_Part_Group : Node {
    uint32_t material_index;
    uint32_t section_index;
    Array<MAP_Mesh_Part, The_Arena_Allocator> mesh_parts = {};

    void release() {
        ProfileFunction();

        mesh_parts.release();
    }
};
// @Note: Geometries can be empty -- contain 0 mesh groups (no opaque, no transparent, no decal).
//        This means you can't just store tree nesting structure implicitly on the map meshes, you need
//        explicit metadata if you want to preserve bit-for-bit roundtrippability.
//        Geometry subfiles CANNOT be empty (I'm asserting geometry_count >= 1 as of writing). -p 2022-06-25
struct MAP_Mesh : Node {
    bool bbox_override = false;
    float bounding_box_a[3] = {};
    float bounding_box_b[3] = {};
    LinkedList<MAP_Mesh_Part_Group, The_Arena_Allocator> mesh_part_groups = {};
    Array<MAP_Mesh_Vertex_Buffer, The_Arena_Allocator> vertex_buffers = {};
    Array<uint16_t, The_Arena_Allocator> indices = {};
    void release() {
        ProfileFunction();

        for (auto &mpg : mesh_part_groups) {
            mpg.release();
        }
        mesh_part_groups.release();
        for (auto &buf : vertex_buffers) {
            buf.release();
        }
        vertex_buffers.release();
        indices.release();
    }

    uint8_t diff_between_unknown_value_and_index_buffer_end = 0; // @Temporary? for roundtrippability.

    // @Note: Some "manually-edited" (python scripts) maps have a different vertices_length than you would otherwise compute,
    //        so we store them as an override in that case. It's overridden if nonzero.
    //        It's possible in the future we'll be able to edit the manually-edited maps so that they have the same
    //        vertices_length field as the retail maps, and then we can make stronger claims about the field and
    //        obviate the need for this override.
    uint32_t vertices_length_override = 0;
};

// Geometries can be empty, so they can't be implicitly encoded by indices in MAP_Mesh.
// (MeshGroups can't be empty, so they can. Same with GeometryGroup subfiles, which we store here.)
// This encoding is preserved to achieve bit-for-bit roundtrippability.
struct MAP_Geometry : Node {
    uint32_t id = 0;
    uint32_t subfile_index = 0;
    LinkedList<MAP_Mesh, The_Arena_Allocator> opaque_meshes = {};
    LinkedList<MAP_Mesh, The_Arena_Allocator> transparent_meshes = {};
    LinkedList<MAP_Mesh, The_Arena_Allocator> decal_meshes = {};

    // Only here to preserve bit-for-bit roundtrippability.
    bool has_weird_2_byte_misalignment_before_transparents = false;
    bool has_weird_2_byte_misalignment_before_decals = false;

    void release() {
        ProfileFunction();

        for (auto &mesh : decal_meshes) {
            mesh.release();
        }
        decal_meshes.release();
        for (auto &mesh : transparent_meshes) {
            mesh.release();
        }
        transparent_meshes.release();
        for (auto &mesh : opaque_meshes) {
            mesh.release();
        }
        opaque_meshes.release();
    }
};



struct MAP_Material : Node {
    uint32_t subfile_index = 0;
    uint16_t mode;
    uint16_t texture_id;
    uint32_t diffuse_color;
    uint32_t specular_color;
    float specularity;
};

static bool PH2MAP_material_mode_is_valid(int mode) {
    return mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 6;
}

struct MAP_Sprite_Metadata {
    uint16_t id;
    uint16_t format;
};
enum MAP_Texture_Format {
    MAP_Texture_Format_BC1       = 0x100,
    MAP_Texture_Format_BC2       = 0x102,
    MAP_Texture_Format_BC3       = 0x103,
    MAP_Texture_Format_BC3_Maybe = 0x104,
};
// @Note: Texture subfiles can be empty -- contain 0 textures.
//        This means you can't just store tree nesting structure implicitly on the textures, you need
//        explicit metadata if you want to preserve bit-for-bit roundtrippability. -p 2022-06-25
struct MAP_Texture : Node {
    uint16_t id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t material = 0;
    // Sprite metadata literally only exists to facilitate bit-for-bit roundtrippability.
    // The data isn't used otherwise, so there's no point in adding more than SH2 ever had.
    // The max SH2 has is 41, so round that up to 64 in a fixed-size array to avoid dynamic allocations.
    uint8_t sprite_count = 0;
    MAP_Sprite_Metadata sprite_metadata[64] = {};
    uint16_t format = MAP_Texture_Format_BC1;
    Array<uint8_t, The_Arena_Allocator> blob = {};
    void release() {
        ProfileFunction();

        blob.release();
    }
};

// Texture subfiles can be empty, so they can't be implicitly encoded by indices in MAP_Texture.
struct MAP_Texture_Subfile : Node {
    bool came_from_non_numbered_dependency = false; // @Temporary! (maybe? Yuck!)

    LinkedList<MAP_Texture, The_Arena_Allocator> textures = {};

    void release() {
        ProfileFunction();

        for (auto &tex : textures) {
            tex.release();
        }
        textures.release();
    }
};


struct Map {
    LinkedList<MAP_Geometry, The_Arena_Allocator> geometries = {};

    LinkedList<MAP_Texture_Subfile, The_Arena_Allocator> texture_subfiles = {};

    LinkedList<MAP_Material, The_Arena_Allocator> materials = {};

    void release_geometry() {
        ProfileFunction();

        for (auto &geo : geometries) {
            geo.release();
        }
        geometries.release();
    }
    void release_textures() {
        ProfileFunction();

        materials.release();

        for (auto &sub : texture_subfiles) {
            sub.release();
        }

        texture_subfiles.release();
    }
};

enum struct ControlState {
    Normal,
    Orbiting,
    Dragging,
};

struct Map_History_Entry {
    uint64_t hash = 0;
    char *data = nullptr;
    size_t count = 0;
    size_t bytes_used = 0;
    void release() {
        ProfileFunction();

        free(data);
        *this = {};
    }
};

SPALL_FN double get_rdtsc_multiplier(void) {

    // Cache the answer so that multiple calls never take the slow path more than once
    static double multiplier = 0;
    if (multiplier) {
        return multiplier;
    }

    uint64_t tsc_freq = 0;

    // Fast path: Load kernel-mapped memory page
    HMODULE ntdll = LoadLibraryA("ntdll.dll");
    if (ntdll) {

        int (*NtQuerySystemInformation)(int, void *, unsigned int, unsigned int *) =
        (int (*)(int, void *, unsigned int, unsigned int *))GetProcAddress(ntdll, "NtQuerySystemInformation");
        if (NtQuerySystemInformation) {

            volatile uint64_t *hypervisor_shared_page = NULL;
            unsigned int size = 0;

            // SystemHypervisorSharedPageInformation == 0xc5
            int result = (NtQuerySystemInformation)(0xc5, (void *)&hypervisor_shared_page, sizeof(hypervisor_shared_page), &size);

            // success
            if (size == sizeof(hypervisor_shared_page) && result >= 0) {
                // docs say ReferenceTime = ((VirtualTsc * TscScale) >> 64)
                //      set ReferenceTime = 10000000 = 1 second @ 10MHz, solve for VirtualTsc
                //       =>    VirtualTsc = 10000000 / (TscScale >> 64)
                tsc_freq = (10000000ull << 32) / (hypervisor_shared_page[1] >> 32);
                // If your build configuration supports 128 bit arithmetic, do this:
                // tsc_freq = ((unsigned __int128)10000000ull << (unsigned __int128)64ull) / hypervisor_shared_page[1];
            }
        }
        FreeLibrary(ntdll);
    }

    // Slow path
    if (!tsc_freq) {

        // Get time before sleep
        uint64_t qpc_begin = 0; QueryPerformanceCounter((LARGE_INTEGER *)&qpc_begin);
        uint64_t tsc_begin = __rdtsc();

        Sleep(2);

        // Get time after sleep
        uint64_t qpc_end = qpc_begin + 1; QueryPerformanceCounter((LARGE_INTEGER *)&qpc_end);
        uint64_t tsc_end = __rdtsc();

        // Do the math to extrapolate the RDTSC ticks elapsed in 1 second
        uint64_t qpc_freq = 0; QueryPerformanceFrequency((LARGE_INTEGER *)&qpc_freq);
        tsc_freq = (tsc_end - tsc_begin) * qpc_freq / (qpc_end - qpc_begin);
    }

    // Failure case
    if (!tsc_freq) {
        tsc_freq = 1000000000;
    }

    multiplier = 1000000.0 / (double)tsc_freq;

    return multiplier;
}

struct Ray_Vs_World_Result {
    bool hit = false;

    float closest_t = INFINITY;
    HMM_Vec3 hit_widget_pos = {};

    struct {
        MAP_Mesh *hit_mesh = nullptr;
        int hit_vertex_buffer = -1;
        int hit_face = -1;
        int hit_vertex = -1;
    } map;

    struct {
        int hit_group = -1;
        int hit_face_index = -1;
        int hit_vertex = -1;
    } cld;
};

struct G : Map {
    Array<Map_History_Entry> undo_stack = {};
    Array<Map_History_Entry> redo_stack = {};

    double last_time = 0;
    double t = 0;
    float dt_history[1024] = {};

    float mouse_x = 0;
    float mouse_y = 0;

    ControlState control_state = {};
    float fov = FOV_DEFAULT;
    HMM_Vec3 bg_col = BG_COL_DEFAULT;

    HMM_Vec3 displacement = {};
    HMM_Vec3 scaling_factor = {1, 1, 1};
    HMM_Vec3 overall_center = {0, 0, 0};
    // @Note: This is really kinda meh in how it works. But it should be ok for now.
    bool overall_center_needs_recalc = true;

    bool show_editor = true;
    bool show_viewport = true;
    bool show_textures = true;
    bool show_materials = true;
    bool show_console = true;
    bool show_edit_widget = true;

    float view_x = 0; // in PIXELS!
    float view_y = 0;
    float view_w = 1;
    float view_h = 1;

    // These are REALLY dumb. But they work.
    bool control_s = false;
    bool control_shift_s = false;

    bool control_o = false;

    bool control_z = false;
    bool control_y = false;

    HMM_Vec3 cam_pos = {0, 0, 0};
    float yaw = 0;
    float pitch = 0;
    float move_speed = MOVE_SPEED_DEFAULT;
    float scroll_speed = MOVE_SPEED_DEFAULT;
    float scroll_speed_timer = 0;

    Ray click_ray = {};
    Ray mouse_ray = {};
    bool clicked = false;
    Ray_Vs_World_Result click_result = {};

    HMM_Vec3 widget_original_pos = {};
    int select_cld_group = -1;
    int select_cld_face = -1;
    int drag_cld_group = -1;
    int drag_cld_face = -1;
    int drag_cld_vertex = -1;

    MAP_Mesh *drag_map_mesh = nullptr;
    int drag_map_buffer = -1;
    int drag_map_vertex = -1;

    PH2CLD_Collision_Data cld = {};
    HMM_Vec3 cld_origin() {
        return {}; // HMM_Vec3 { cld.origin[0], 0, cld.origin[1] };
    }
    sg_pipeline cld_pipeline = {};
    enum { cld_buffers_count = 4 };
    CLD_Face_Buffer cld_face_buffers[cld_buffers_count] = {};

    sg_pipeline map_pipeline = {};
    sg_pipeline map_pipeline_no_cull = {};
    sg_pipeline map_pipeline_wireframe = {};
    sg_pipeline map_pipeline_no_cull_wireframe = {};

    sg_pipeline decal_pipeline = {};
    sg_pipeline decal_pipeline_no_cull = {};
    sg_pipeline decal_pipeline_wireframe = {};
    sg_pipeline decal_pipeline_no_cull_wireframe = {};

    enum { map_buffers_max = 64 };
    MAP_Geometry_Buffer map_buffers[map_buffers_max];// = {};
    int map_buffers_count = 0;

    Array<MAP_Texture_Buffer> map_textures = {};

    sg_buffer highlight_vertex_circle_buffer = {};
    sg_pipeline highlight_vertex_circle_pipeline = {};

    MAP_Texture_Buffer missing_texture = {};

    MAP_Texture_Subfile *ui_selected_texture_subfile = nullptr;
    MAP_Texture *ui_selected_texture = nullptr;

    bool texture_actual_size = false;

    int solo_material = -1; // @Hack, but whatever.

    bool cld_must_update = false;
    bool map_must_update = false;

    void staleify_cld() {
        cld_must_update = true;
    }
    void staleify_map() {
        map_must_update = true;
        // map_buffers_count = 0;
    }
    bool stale() {
        return cld_must_update || map_must_update;
    }

    bool subgroup_visible[16] = {true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};

    bool textured = true;
    bool use_lighting_colours = true;
    bool use_material_colours = true;
    bool cull_backfaces = true;
    bool wireframe = false;

    char *opened_map_filename = nullptr;

    void clear_undo_redo_stacks() {
        ProfileFunction();

        for (auto &entry : redo_stack) {
            entry.release();
        }
        redo_stack.clear();
        for (auto &entry : undo_stack) {
            entry.release();
        }
        undo_stack.clear();
    }

    void release() {
        ProfileFunction();

        clear_undo_redo_stacks();
        undo_stack.release();
        redo_stack.release();
        PH2CLD_free_collision_data(cld);
        sg_destroy_pipeline(cld_pipeline);
        for (auto &buf : cld_face_buffers) {
            sg_destroy_buffer(buf.buf);
        }
        sg_destroy_pipeline(map_pipeline);
        sg_destroy_pipeline(map_pipeline_no_cull);
        sg_destroy_pipeline(decal_pipeline);
        sg_destroy_pipeline(decal_pipeline_no_cull);
        for (auto &buf : map_buffers) {
            buf.release();
        }
        sg_destroy_buffer(highlight_vertex_circle_buffer);
        sg_destroy_pipeline(highlight_vertex_circle_pipeline);
        missing_texture.release();
        for (auto &tex : map_textures) {
            tex.release();
        }
        map_textures.release();
        free(opened_map_filename); opened_map_filename = nullptr;
        Map::release_geometry();
        Map::release_textures();
        *this = {};
    }
};
static void cld_upload(G &g) {
    ProfileFunction();

    auto &cld = g.cld;
    if (!cld.valid) {
        for (auto &buf : g.cld_face_buffers) {
            buf.num_vertices = 0;
        }
        return;
    }
    //Log("CLD origin is (%f, 0, %f)", cld.origin[0], cld.origin[1]);
    for (int group = 0; group < 4; group++) {
        PH2CLD_Face *faces = cld.group_0_faces;
        size_t num_faces = cld.group_0_faces_count;
        if (group == 1) { faces = cld.group_1_faces; num_faces = cld.group_1_faces_count; }
        if (group == 2) { faces = cld.group_2_faces; num_faces = cld.group_2_faces_count; }
        if (group == 3) { faces = cld.group_3_faces; num_faces = cld.group_3_faces_count; }
        //Log("group %d has %zu faces", group, num_faces);
        auto max_triangles = num_faces * 2;
        auto max_vertices = max_triangles * 3;
        auto max_floats = max_vertices * 3;
        auto floats = (float *)malloc(max_floats * sizeof(float));
        assert(floats);
        defer {
            free(floats);
        };
        auto write_pointer = floats;
        auto end = floats + max_floats;
        for (size_t i = 0; i < num_faces; i++) {
            PH2CLD_Face face = faces[i];
            bool skip_face = false;
            for (int subgroup = 0; subgroup < 16; subgroup++) {
                if ((face.subgroups & (1 << subgroup)) && !g.subgroup_visible[subgroup]) {
                    skip_face = true;
                }
            }
            if (skip_face) {
                continue;
            }
            auto push_vertex = [&] (float (&vertex)[3]) {
                *write_pointer++ = vertex[0];
                *write_pointer++ = vertex[1];
                *write_pointer++ = vertex[2];
            };
            push_vertex(face.vertices[0]);
            push_vertex(face.vertices[1]);
            push_vertex(face.vertices[2]);
            if (face.quad) {
                push_vertex(face.vertices[0]);
                push_vertex(face.vertices[2]);
                push_vertex(face.vertices[3]);
            }
        }
        auto num_floats = (size_t)(write_pointer - floats);
        
        if (num_floats) {
            sg_update_buffer(g.cld_face_buffers[group].buf, sg_range { floats, num_floats * sizeof(float) });
        }
        assert(num_floats % 3 == 0);
        g.cld_face_buffers[group].num_vertices = (int)num_floats / 3;
        assert(g.cld_face_buffers[group].num_vertices % 3 == 0);
    }
}
static void cld_load(G &g, const char *filename) {
    ProfileFunction();

    Log("CLD filename is \"%s\"", filename);
    PH2CLD_free_collision_data(g.cld);
    g.cld = PH2CLD_get_collision_data_from_file(filename);
    if (!g.cld.valid) {
        LogErr("Failed loading CLD file \"%s\"!", filename);
    }
    cld_upload(g);
    g.staleify_cld();
}
#define Read(ptr, x) (assert(ptr + sizeof(x) <= end), memcpy(&(x), ptr, sizeof(x)) && (ptr += sizeof(x)))
struct PH2MAP__Header {
    uint32_t magic; // should be 0x20010510
    uint32_t file_length;
    uint32_t subfile_count;
    uint32_t padding0; // always 0
};
struct PH2MAP__Subfile_Header {
    uint32_t type; // 1 == Geometry; 2 == Texture
    uint32_t length;
    uint32_t padding0;
    uint32_t padding1;
};
struct PH2MAP__Texture_Subfile_Header {
    uint32_t magic; // should be 0x19990901
    uint32_t pad[2];
    uint32_t always1;
};
struct PH2MAP__BC_Texture_Header {
    uint32_t id;
    uint16_t width;
    uint16_t height;
    uint16_t width2;
    uint16_t height2;
    uint32_t sprite_count;
    uint16_t material;
    uint16_t material2; // @Note: the docs say this is always `id`, but I observe it's always `material`
    uint32_t pad[3];
};
struct PH2MAP__BC_End_Sentinel {
    uint32_t line_check;
    uint32_t zero[3];
};
struct PH2MAP__Sprite_Header {
    uint32_t id;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t format;
    
    // Sprite Pixel Header
    uint32_t data_length;
    uint32_t data_length_plus_header; // plus_header meaning +16
    uint32_t pad;
    uint32_t always0x99000000;
};

struct PH2MAP__Geometry_Subfile_Header {
    uint32_t magic; // should be 0x20010730
    uint32_t geometry_count;
    uint32_t geometry_size;
    uint32_t material_count;
};

struct PH2MAP__Geometry_Header {
    uint32_t id;
    int32_t group_size;
    int32_t opaque_group_offset;
    int32_t transparent_group_offset;
    int32_t decal_group_offset;
};
struct PH2MAP__Mapmesh_Header {
    float bounding_box_a[4];
    float bounding_box_b[4];
    int32_t vertex_sections_header_offset;
    int32_t indices_offset;
    int32_t indices_length;
    int32_t unknown; // @Todo: some kinda sanity check on this value
    int32_t mesh_part_group_count;
};
struct PH2MAP__Mesh_Part_Group_Header {
    uint32_t material_index;
    uint32_t section_index;
    uint32_t mesh_part_count;
};
struct PH2MAP__Mesh_Part {
    uint16_t strip_length;
    uint8_t invert_reading;
    uint8_t strip_count;
    uint16_t first_vertex;
    uint16_t last_vertex;
};
struct PH2MAP__Decal_Header {
    float bounding_box_a[4];
    float bounding_box_b[4];
    int32_t vertex_sections_header_offset;
    int32_t indices_offset;
    int32_t indices_length;
    uint32_t sub_decal_count;
};
struct PH2MAP__Sub_Decal {
    uint32_t material_index;
    uint32_t section_index;
    uint32_t strip_length;
    uint32_t strip_count;
};
struct PH2MAP__Vertex_Sections_Header {
    int32_t vertices_length;
    int32_t vertex_section_count;
};
struct PH2MAP__Vertex14 {
    float position[3];
    float uv[2];
};
struct PH2MAP__Vertex18 {
    float position[3];
    uint32_t color; // rgba8888
    float uv[2];
};
struct PH2MAP__Vertex20 {
    float position[3];
    float normal[3];
    float uv[2];
};
struct PH2MAP__Vertex24 {
    float position[3];
    float normal[3];
    uint32_t color; // rgba8888
    float uv[2];
};
struct PH2MAP__Vertex_Section_Header {
    int32_t section_starts;
    int32_t bytes_per_vertex;
    int32_t section_length;
};
struct PH2MAP__Material {
    uint16_t mode;
    uint16_t texture_id;
    uint32_t diffuse_color;
    uint32_t specular_color;
    float specularity;
};

void map_unpack_mesh_vertex_buffer(MAP_Geometry_Buffer &buffer, MAP_Mesh &mesh) {
    ProfileFunction();

    for (MAP_Mesh_Vertex_Buffer &vertex_buffer : mesh.vertex_buffers) {
        for (int i = 0; i < vertex_buffer.num_vertices; i++) {
            MAP_Geometry_Vertex result = {};
            const char *const vertex_ptr = vertex_buffer.data.data + i * vertex_buffer.bytes_per_vertex;
            assert(i < vertex_buffer.num_vertices);
            switch (vertex_buffer.bytes_per_vertex) {
                case 0x14: {
                    const auto vert = *(const PH2MAP__Vertex14 *)vertex_ptr;
                    result.position[0] = vert.position[0];
                    result.position[1] = vert.position[1];
                    result.position[2] = vert.position[2];
                    result.uv[0] = vert.uv[0];
                    result.uv[1] = vert.uv[1];
                } break;
                case 0x18: {
                    const auto vert = *(const PH2MAP__Vertex18 *)vertex_ptr;
                    result.position[0] = vert.position[0];
                    result.position[1] = vert.position[1];
                    result.position[2] = vert.position[2];
                    result.color = vert.color;
                    result.uv[0] = vert.uv[0];
                    result.uv[1] = vert.uv[1];
                } break;
                case 0x20: {
                    const auto vert = *(const PH2MAP__Vertex20 *)vertex_ptr;
                    result.position[0] = vert.position[0];
                    result.position[1] = vert.position[1];
                    result.position[2] = vert.position[2];
                    result.normal[0] = vert.normal[0];
                    result.normal[1] = vert.normal[1];
                    result.normal[2] = vert.normal[2];
                    result.uv[0] = vert.uv[0];
                    result.uv[1] = vert.uv[1];
                } break;
                case 0x24: {
                    const auto vert = *(const PH2MAP__Vertex24 *)vertex_ptr;
                    result.position[0] = vert.position[0];
                    result.position[1] = vert.position[1];
                    result.position[2] = vert.position[2];
                    result.normal[0] = vert.normal[0];
                    result.normal[1] = vert.normal[1];
                    result.normal[2] = vert.normal[2];
                    result.color = vert.color;
                    result.uv[0] = vert.uv[0];
                    result.uv[1] = vert.uv[1];
                } break;
                default: {
                    assert(false);
                } break;
            }
            auto unpacked_vertex = result;
            buffer.vertices.push(unpacked_vertex);
        }
    }

}

void map_destrip_mesh_part_group(MAP_Geometry_Buffer &buffer, int &indices_index, const MAP_Mesh &mesh, MAP_Mesh_Part_Group mesh_part_group) {
    ProfileFunction();

    int num_vertices_written = 0;
    assert(mesh_part_group.mesh_parts.count > 0);
    for (MAP_Mesh_Part &mesh_part : mesh_part_group.mesh_parts) {
        uint32_t base_of_this_section = 0;
        for (uint32_t i = 0; i < mesh_part_group.section_index; i++) {
            base_of_this_section += mesh.vertex_buffers[i].num_vertices;
        }

        int outer_max = mesh_part.strip_count;
        int inner_max = mesh_part.strip_length;
        assert(inner_max >= 3);

        int num_vertices_written_in_this_strip = 0;
        for (int strip_index = 0; strip_index < outer_max; strip_index++) {
            unsigned int memory = (mesh.indices.data[indices_index++]) << 0x10;
            unsigned int mask = 0xFFFF0000;
            uint16_t currentIndex = (mesh.indices.data[indices_index++]);
            for (int i = 2; i < inner_max; i++) {
                memory = (memory & mask) + (currentIndex << (0x10 & mask));
                mask ^= 0xFFFFFFFF;

                currentIndex = (mesh.indices.data[indices_index++]);

                uint32_t a = memory >> 0x10;
                uint32_t b = memory & 0xffff;
                uint32_t c = currentIndex;

                bool degenerate = (a == b) || (b == c) || (a == c);

                if (!degenerate)
                {
                    buffer.indices.push(base_of_this_section + a);
                    buffer.indices.push(base_of_this_section + b);
                    buffer.indices.push(base_of_this_section + c);

                    num_vertices_written += 3;
                }

            }
        }

        assert(num_vertices_written_in_this_strip <= (inner_max - 2) * 3 * outer_max);
        num_vertices_written += num_vertices_written_in_this_strip;
    }
    buffer.vertices_per_mesh_part_group.push(num_vertices_written);
}
// We need 'misalignment' because some mesh groups are weirdly misaligned, and mesh alignment happens *with respect to that misalignment* (!!!!!!!!!!)
static int map_load_mesh_group_or_decal_group(LinkedList<MAP_Mesh, The_Arena_Allocator> *meshes, uint32_t misalignment, const char *group_header, const char *end, bool is_decal) {
    ProfileFunction();

    (void)end;
    const char *ptr3 = group_header;
    uint32_t count = 0;
    Read(ptr3, count);
    assert(count > 0);
    // meshes->reserve(count);
    const char *mesh_end = nullptr;
    for (uint32_t offset_index = 0; offset_index < count; offset_index++) {
        int32_t offset = 0;
        Read(ptr3, offset);
        assert(offset > 0);

        if (offset_index == 0) {
            assert(offset == (1 + count) * sizeof(uint32_t));
        }

        const char *header_base = group_header + offset;
        const char *ptr4 = header_base;

        MAP_Mesh mesh = {};
        defer {
            meshes->push(mesh);
        };

        struct Common_Header {
            float bounding_box_a[4];
            float bounding_box_b[4];
            int32_t vertex_sections_header_offset;
            int32_t indices_offset;
            int32_t indices_length;
            uint32_t count;
        };
        Common_Header header = {};
        uint32_t real_header_size = 0; // Semi hacky. Ouch!
        if (is_decal) {
            PH2MAP__Decal_Header decal_header = {};
            Read(ptr4, decal_header);
            header.bounding_box_a[0] = decal_header.bounding_box_a[0];
            header.bounding_box_a[1] = decal_header.bounding_box_a[1];
            header.bounding_box_a[2] = decal_header.bounding_box_a[2];
            header.bounding_box_a[3] = decal_header.bounding_box_a[3];
            header.bounding_box_b[0] = decal_header.bounding_box_b[0];
            header.bounding_box_b[1] = decal_header.bounding_box_b[1];
            header.bounding_box_b[2] = decal_header.bounding_box_b[2];
            header.bounding_box_b[3] = decal_header.bounding_box_b[3];
            header.vertex_sections_header_offset = decal_header.vertex_sections_header_offset;
            header.indices_offset = decal_header.indices_offset;
            header.indices_length = decal_header.indices_length;
            header.count = decal_header.sub_decal_count;

            real_header_size = sizeof(PH2MAP__Decal_Header);
        } else {
            PH2MAP__Mapmesh_Header mapmesh_header = {};
            Read(ptr4, mapmesh_header);
            header.bounding_box_a[0] = mapmesh_header.bounding_box_a[0];
            header.bounding_box_a[1] = mapmesh_header.bounding_box_a[1];
            header.bounding_box_a[2] = mapmesh_header.bounding_box_a[2];
            header.bounding_box_a[3] = mapmesh_header.bounding_box_a[3];
            header.bounding_box_b[0] = mapmesh_header.bounding_box_b[0];
            header.bounding_box_b[1] = mapmesh_header.bounding_box_b[1];
            header.bounding_box_b[2] = mapmesh_header.bounding_box_b[2];
            header.bounding_box_b[3] = mapmesh_header.bounding_box_b[3];
            header.vertex_sections_header_offset = mapmesh_header.vertex_sections_header_offset;
            header.indices_offset = mapmesh_header.indices_offset;
            header.indices_length = mapmesh_header.indices_length;
            header.count = mapmesh_header.mesh_part_group_count;
            // @Note: I saved off some edited maps as having an offset of 0, but it doesn't seem to adversely
            //        impact the in-game parsing, so maybe we can just set it to 0 when it's editor-overridden. (???)
            assert(mapmesh_header.indices_offset + mapmesh_header.indices_length - mapmesh_header.unknown == 0 ||
                   mapmesh_header.indices_offset + mapmesh_header.indices_length - mapmesh_header.unknown >= 6);
            assert(mapmesh_header.indices_offset + mapmesh_header.indices_length - mapmesh_header.unknown <= 20);
            mesh.diff_between_unknown_value_and_index_buffer_end = (uint8_t)(mapmesh_header.indices_offset + mapmesh_header.indices_length - mapmesh_header.unknown);

            real_header_size = sizeof(PH2MAP__Mapmesh_Header);
        }

        assert(PH2CLD__sanity_check_float4(header.bounding_box_a));
        assert(PH2CLD__sanity_check_float4(header.bounding_box_b));
        assert(header.bounding_box_a[0] <= header.bounding_box_b[0]);
        assert(header.bounding_box_a[1] <= header.bounding_box_b[1]);
        assert(header.bounding_box_a[2] <= header.bounding_box_b[2]);
        assert(header.bounding_box_a[3] == 0);
        assert(header.bounding_box_b[3] == 0);
        assert(header.vertex_sections_header_offset >= 0);
        assert(header.indices_offset >= 0);
        assert(header.indices_length >= 0);
        assert(header.indices_length % sizeof(uint16_t) == 0);
        assert(header_base + header.indices_offset + header.indices_length <= end);

        memcpy(mesh.bounding_box_a, header.bounding_box_a, sizeof(float) * 3);
        memcpy(mesh.bounding_box_b, header.bounding_box_b, sizeof(float) * 3);

        float bbox_min[3] = { +INFINITY, +INFINITY, +INFINITY };
        float bbox_max[3] = { -INFINITY, -INFINITY, -INFINITY };

        ptr4 = header_base + header.vertex_sections_header_offset;
        PH2MAP__Vertex_Sections_Header vertex_sections_header = {};
        Read(ptr4, vertex_sections_header);
        assert(vertex_sections_header.vertices_length >= 0);
        assert(vertex_sections_header.vertex_section_count >= 0);
        assert(vertex_sections_header.vertex_section_count <= 4);
        const char *ptr5 = ptr4 + vertex_sections_header.vertex_section_count * sizeof(PH2MAP__Vertex_Section_Header);
        assert(ptr5 == header_base + header.vertex_sections_header_offset + sizeof(PH2MAP__Vertex_Sections_Header) + vertex_sections_header.vertex_section_count * sizeof(PH2MAP__Vertex_Section_Header));
        const char *end_of_previous_section = ptr5;
        mesh.vertex_buffers.reserve(vertex_sections_header.vertex_section_count);
        for (int32_t vertex_section_index = 0; vertex_section_index < vertex_sections_header.vertex_section_count; vertex_section_index++) {
            PH2MAP__Vertex_Section_Header vertex_section_header = {};
            Read(ptr4, vertex_section_header);

            // @Note: How does section_starts work?
            //assert(vertex_section_header.section_starts == 0);

            const char *vertex_section_offset_base = header_base + header.vertex_sections_header_offset + sizeof(PH2MAP__Vertex_Sections_Header) + vertex_sections_header.vertex_section_count * sizeof(PH2MAP__Vertex_Section_Header);

            assert(vertex_section_header.section_starts >= 0);
            if (vertex_section_index == 0) {
                assert(vertex_section_header.section_starts == 0);
            } else {
                assert(vertex_section_header.section_starts > 0);
            }
            assert(vertex_section_header.bytes_per_vertex == 0x14 ||
                vertex_section_header.bytes_per_vertex == 0x18 ||
                vertex_section_header.bytes_per_vertex == 0x20 ||
                vertex_section_header.bytes_per_vertex == 0x24);
            assert(vertex_section_header.section_length > 0);
            assert(vertex_section_header.section_length % vertex_section_header.bytes_per_vertex == 0);
            assert(vertex_section_header.section_length / vertex_section_header.bytes_per_vertex < 65536);

            MAP_Mesh_Vertex_Buffer vertex_buffer = {};
            defer {
                mesh.vertex_buffers.push(vertex_buffer);
            };
            
            vertex_buffer.bytes_per_vertex = vertex_section_header.bytes_per_vertex;

            vertex_buffer.num_vertices = vertex_section_header.section_length / vertex_section_header.bytes_per_vertex;

            assert(ptr5 == end_of_previous_section);
            assert(ptr5 == vertex_section_offset_base + vertex_section_header.section_starts);
            const char *end_of_this_section = ptr5 + vertex_section_header.section_length;
            vertex_buffer.data.resize(vertex_buffer.num_vertices * vertex_buffer.bytes_per_vertex);
            memcpy(vertex_buffer.data.data, ptr5, vertex_buffer.num_vertices * vertex_buffer.bytes_per_vertex);
            for (int i = 0; i < vertex_buffer.num_vertices; i++) {
                float pos[3] = {};
                switch (vertex_section_header.bytes_per_vertex) {
                    case 0x14: {
                        PH2MAP__Vertex14 vert = {};
                        Read(ptr5, vert);
                        assert(PH2CLD__sanity_check_float3(vert.position));
                        assert(PH2CLD__sanity_check_float2(vert.uv));
                        assert(vert.uv[0] > -1);
                        assert(vert.uv[0] < + 2);
                        assert(vert.uv[1] > -1);
                        assert(vert.uv[1] < + 2);
                        memcpy(pos, vert.position, 3 * sizeof(float));
                    } break;
                    case 0x18: {
                        PH2MAP__Vertex18 vert = {};
                        Read(ptr5, vert);
                        assert(PH2CLD__sanity_check_float3(vert.position));
                        assert(PH2CLD__sanity_check_float2(vert.uv));
                        assert(vert.uv[0] > -1);
                        assert(vert.uv[0] < + 2);
                        assert(vert.uv[1] > -1);
                        assert(vert.uv[1] < + 2);
                        memcpy(pos, vert.position, 3 * sizeof(float));
                    } break;
                    case 0x20: {
                        PH2MAP__Vertex20 vert = {};
                        Read(ptr5, vert);
                        assert(PH2CLD__sanity_check_float3(vert.position));
                        // It looks like sometimes this can be NaN (0x7fc00000);
                        // it'd be nice to check if it's 0x7fc00000 and only sanity check otherwise.
                        //assert(PH2CLD__sanity_check_float3(vert.normal));
                        assert(PH2CLD__sanity_check_float2(vert.uv));
                        assert(vert.uv[0] > -1);
                        assert(vert.uv[0] < + 2);
                        assert(vert.uv[1] > -1);
                        assert(vert.uv[1] < + 2);
                        memcpy(pos, vert.position, 3 * sizeof(float));
                    } break;
                    case 0x24: {
                        PH2MAP__Vertex24 vert = {};
                        Read(ptr5, vert);
                        assert(PH2CLD__sanity_check_float3(vert.position));
                        // It looks like sometimes this can be NaN (0x7fc00000);
                        // it'd be nice to check if it's 0x7fc00000 and only sanity check otherwise.
                        //assert(PH2CLD__sanity_check_float3(vert.normal));
                        assert(PH2CLD__sanity_check_float2(vert.uv));
                        assert(vert.uv[0] > -1);
                        assert(vert.uv[0] < + 2);
                        assert(vert.uv[1] > -1);
                        assert(vert.uv[1] < + 2);
                        memcpy(pos, vert.position, 3 * sizeof(float));
                    } break;
                }
                bbox_min[0] = fminf(bbox_min[0], pos[0]);
                bbox_min[1] = fminf(bbox_min[1], pos[1]);
                bbox_min[2] = fminf(bbox_min[2], pos[2]);
                bbox_max[0] = fmaxf(bbox_max[0], pos[0]);
                bbox_max[1] = fmaxf(bbox_max[1], pos[1]);
                bbox_max[2] = fmaxf(bbox_max[2], pos[2]);
            }
            assert(ptr5 == end_of_this_section);
            end_of_previous_section = end_of_this_section;
        }
        assert(end_of_previous_section == header_base + header.indices_offset);

        // assert(bbox_min[0] == header.bounding_box_a[0]);
        // assert(bbox_min[1] == header.bounding_box_a[1]);
        // assert(bbox_min[2] == header.bounding_box_a[2]);
        // assert(bbox_max[0] == header.bounding_box_b[0]);
        // assert(bbox_max[1] == header.bounding_box_b[1]);
        // assert(bbox_max[2] == header.bounding_box_b[2]);

        // @Note: Manually edited files sometimes haven't updated their bounding boxes.
        if (memcmp(bbox_min, header.bounding_box_a, 3 * sizeof(float)) != 0) {
            mesh.bbox_override = true;
            Log("BBox Override");
        }
        if (memcmp(bbox_max, header.bounding_box_b, 3 * sizeof(float)) != 0) {
            mesh.bbox_override = true;
            Log("BBox Override");
        }

        if (vertex_sections_header.vertices_length != (int64_t)header.indices_offset - (int64_t)(header.vertex_sections_header_offset + sizeof(PH2MAP__Vertex_Sections_Header) + vertex_sections_header.vertex_section_count * sizeof(PH2MAP__Vertex_Section_Header))) {
            mesh.vertices_length_override = vertex_sections_header.vertices_length;
        }
        ptr4 = end_of_previous_section;
        assert(header.indices_length % sizeof(uint16_t) == 0);
        int indices_count = header.indices_length / sizeof(uint16_t);
        mesh.indices.reserve(indices_count);
        for (int indices_index = 0; indices_index < indices_count; indices_index++) {
            uint16_t index = 0;
            Read(ptr4, index);
            mesh.indices.push(index);
        }

        // Meshes can *START* misaligned, but they end aligned.
        const char *aligner = ptr4;
        for (; (uintptr_t)aligner % 16 != misalignment; aligner++) {
            assert(*aligner == 0);
        }
        ptr4 = aligner;
        mesh_end = ptr4;

        ptr4 = header_base + real_header_size;
        int indices_index = 0;
        // @Note: We are stuffing decals into map meshes, so we're splitting subdecals
        //        such that half the subdecal's data goes into the mesh part group, and
        //        the rest goes into a single meshpart inside that group. It'd be interesting
        //        to try flattening meshpartgroups completely and encode the tree nesting
        //        implicitly, for better allocations or something, but no biggie.
        // mesh.mesh_part_groups.reserve(header.count);
        for (uint32_t i = 0; i < header.count; i++) {
            MAP_Mesh_Part_Group mesh_part_group = {};
            const char *mesh_part_group_start = ptr4;
            defer {
                mesh.mesh_part_groups.push(mesh_part_group);
            };

            if (is_decal) {
                PH2MAP__Sub_Decal sub_decal = {};
                Read(ptr4, sub_decal);

                assert(sub_decal.section_index < 4);

                mesh_part_group.material_index = sub_decal.material_index;
                mesh_part_group.section_index = sub_decal.section_index;

                mesh_part_group.mesh_parts.reserve(1);
                MAP_Mesh_Part part = {};
                defer {
                    mesh_part_group.mesh_parts.push(part);
                };
                part.strip_length = sub_decal.strip_length;
                part.strip_count = sub_decal.strip_count;
                int first_index = indices_index;
                int last_index = first_index + part.strip_length * part.strip_count;
                indices_index = last_index;
            } else {
                PH2MAP__Mesh_Part_Group_Header mesh_part_group_header = {};
                Read(ptr4, mesh_part_group_header);
            
                assert(mesh_part_group_header.section_index < 4);

                // Log("MeshPartGroup index #%d", mesh_part_group_index);
                mesh_part_group.material_index = mesh_part_group_header.material_index;
                mesh_part_group.section_index = mesh_part_group_header.section_index;

                mesh_part_group.mesh_parts.reserve(mesh_part_group_header.mesh_part_count);
                assert(mesh_part_group_header.mesh_part_count >= 1);
                for (uint32_t mesh_part_index = 0; mesh_part_index < mesh_part_group_header.mesh_part_count; mesh_part_index++) {
                    PH2MAP__Mesh_Part mesh_part = {};
                    Read(ptr4, mesh_part);

                    MAP_Mesh_Part part = {};
                    defer {
                        mesh_part_group.mesh_parts.push(part);
                    };

                    part.strip_length = mesh_part.strip_length;
                    part.strip_count = mesh_part.strip_count;
                    if (mesh_part.invert_reading) {
                        part.strip_length = mesh_part.strip_count;
                        part.strip_count = mesh_part.strip_length;
                    }
                    part.was_inverted = mesh_part.invert_reading;
                    int first_index = indices_index;
                    int last_index = first_index + part.strip_length * part.strip_count;
                    indices_index = last_index;
                    int first_vertex = INT_MAX;
                    int last_vertex = INT_MIN;
                    for (int i = first_index; i < last_index; i++) {
                        if (first_vertex > mesh.indices[i]) {
                            first_vertex = mesh.indices[i];
                        }
                        if (last_vertex < mesh.indices[i]) {
                            last_vertex = mesh.indices[i];
                        }
                    }
                    assert(mesh_part.first_vertex == first_vertex);
                    assert(mesh_part.last_vertex == last_vertex);
                }
            }
        }
    }
    assert(mesh_end);
    return (int)(mesh_end - group_header);
}
static void map_write_struct(Array<uint8_t> *result, const void *px, size_t sizeof_x) {
    ProfileFunction();

    result->resize(result->count + (int64_t)sizeof_x);
    assert(result->data);
    assert(result->count >= (int64_t)sizeof_x);
    memcpy(&(*result)[result->count - (int64_t)sizeof_x], px, sizeof_x);
}
#define Write(x) map_write_struct(result, &(x), sizeof(x))
#define WriteLit(T, x) do { T t = (T)(x); Write(t); } while (0)

// @Note: when I convert this to C89, I'll have to make sure the creation fills the space with zeroes. :)
#define CreateBackpatch(T) (int64_t)(result->resize(result->count + sizeof(T)), (result->count - sizeof(T)))
#define WriteBackpatch(T, backpatch_idx, value) do { *(T *)(&(*result)[(backpatch_idx)]) = (T)(value); } while (0)

static void map_write_mesh_group_or_decal_group(Array<uint8_t> *result, bool decals, LinkedList<MAP_Mesh, The_Arena_Allocator> group, int misalignment) {
    ProfileFunction();

    int64_t aligner = result->count;
    int64_t count = group.count();
    WriteLit(uint32_t, count);
    int64_t offsets_start = result->count;

    for (int64_t i = 0; i < count; i++) {
        WriteLit(uint32_t, 0); // offset will be backpatched
    }

    int mesh_index = 0;
    for (auto &mesh : group) {
        // Mapmesh Header
        int64_t mesh_start = result->count;
        WriteBackpatch(uint32_t, offsets_start + mesh_index * sizeof(uint32_t), mesh_start - offsets_start + sizeof(uint32_t));
        if (mesh.bbox_override) {
            Write(mesh.bounding_box_a);
            WriteLit(float, 0);
            Write(mesh.bounding_box_b);
            WriteLit(float, 0);
        } else { // Compute bounding box
            float bbox_min[3] = { + INFINITY, + INFINITY, + INFINITY };
            float bbox_max[3] = { -INFINITY, -INFINITY, -INFINITY };
            for (auto &vertex_buffer : mesh.vertex_buffers) {
                for (int i = 0; i < vertex_buffer.num_vertices; i++) {
                    float pos[3] = {};
                    switch (vertex_buffer.bytes_per_vertex) {
                        case 0x14: {
                            auto verts = (PH2MAP__Vertex14 *)vertex_buffer.data.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                        case 0x18: {
                            auto verts = (PH2MAP__Vertex18 *)vertex_buffer.data.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                        case 0x20: {
                            auto verts = (PH2MAP__Vertex20 *)vertex_buffer.data.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                        case 0x24: {
                            auto verts = (PH2MAP__Vertex24 *)vertex_buffer.data.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                    }
                    bbox_min[0] = fminf(bbox_min[0], pos[0]);
                    bbox_min[1] = fminf(bbox_min[1], pos[1]);
                    bbox_min[2] = fminf(bbox_min[2], pos[2]);
                    bbox_max[0] = fmaxf(bbox_max[0], pos[0]);
                    bbox_max[1] = fmaxf(bbox_max[1], pos[1]);
                    bbox_max[2] = fmaxf(bbox_max[2], pos[2]);
                }
            }
            // assert(bbox_min[0] == mesh.bounding_box_a[0]); // @Temporary until we can get rid of overrides hopefully!
            // assert(bbox_min[1] == mesh.bounding_box_a[1]);
            // assert(bbox_min[2] == mesh.bounding_box_a[2]);
            // assert(bbox_max[0] == mesh.bounding_box_b[0]);
            // assert(bbox_max[1] == mesh.bounding_box_b[1]);
            // assert(bbox_max[2] == mesh.bounding_box_b[2]);
            Write(bbox_min);
            WriteLit(float, 0);
            Write(bbox_max);
            WriteLit(float, 0);
        }
        auto backpatch_vertex_sections_header_offset = CreateBackpatch(uint32_t);
        auto backpatch_indices_offset = CreateBackpatch(uint32_t);
        WriteLit(int32_t, (int)(mesh.indices.count * sizeof(uint16_t)));
        int64_t backpatch_unknown = 0;
        if (!decals) {
            backpatch_unknown = CreateBackpatch(uint32_t);
        }
        WriteLit(int32_t, mesh.mesh_part_groups.count());

        int indices_index = 0;
        for (auto &mesh_part_group : mesh.mesh_part_groups) {
            WriteLit(uint32_t, mesh_part_group.material_index);
            WriteLit(uint32_t, mesh_part_group.section_index);
            if (decals) {
                assert(mesh_part_group.mesh_parts.count == 1);
                auto &part = mesh_part_group.mesh_parts[0];
                WriteLit(uint32_t, part.strip_length);
                WriteLit(uint32_t, part.strip_count);
            } else {
                WriteLit(uint32_t, (uint32_t)mesh_part_group.mesh_parts.count);
                for (auto &part : mesh_part_group.mesh_parts) {
                    if (part.was_inverted) {
                        WriteLit(uint16_t, (uint16_t)part.strip_count);
                        WriteLit(uint8_t, 1);
                        assert(part.strip_length <= 255);
                        WriteLit(uint8_t, (uint8_t)part.strip_length);
                    } else {
                        WriteLit(uint16_t, (uint16_t)part.strip_length);
                        WriteLit(uint8_t, 0);
                        assert(part.strip_count <= 255);
                        WriteLit(uint8_t, (uint8_t)part.strip_count);
                    }
                    int first_index = indices_index;
                    int last_index = first_index + part.strip_length * part.strip_count;
                    indices_index = last_index;
                    int first_vertex = INT_MAX;
                    int last_vertex = INT_MIN;
                    for (int i = first_index; i < last_index; i++) {
                        if (first_vertex > mesh.indices[i]) {
                            first_vertex = mesh.indices[i];
                        }
                        if (last_vertex < mesh.indices[i]) {
                            last_vertex = mesh.indices[i];
                        }
                    }
                    WriteLit(uint16_t, (uint16_t)first_vertex);
                    WriteLit(uint16_t, (uint16_t)last_vertex);
                }
            }
        }

        // Vertex Sections Header
        WriteBackpatch(uint32_t, backpatch_vertex_sections_header_offset, result->count - mesh_start);
        int64_t backpatch_vertices_length = 0;
        if (mesh.vertices_length_override) {
            WriteLit(uint32_t, mesh.vertices_length_override); // @Temporary! (?)
        } else {
            backpatch_vertices_length = CreateBackpatch(uint32_t);
        }
        WriteLit(uint32_t, mesh.vertex_buffers.count);

        // n Vertex Section Header
        uint32_t rolling_vertex_section_offset = 0;
        for (auto &section : mesh.vertex_buffers) {
            uint32_t section_length = section.num_vertices * section.bytes_per_vertex;
            WriteLit(uint32_t, rolling_vertex_section_offset);
            WriteLit(uint32_t, section.bytes_per_vertex);
            WriteLit(uint32_t, section_length);
            rolling_vertex_section_offset += section_length;
        }
        // n groups of n vertices
        int64_t vertices_start = result->count;
        for (auto &section : mesh.vertex_buffers) {
            map_write_struct(result, section.data.data, section.num_vertices * section.bytes_per_vertex);
        }
        if (!mesh.vertices_length_override) {
            WriteBackpatch(uint32_t, backpatch_vertices_length, result->count - vertices_start);
        }

        // n indices
        WriteBackpatch(uint32_t, backpatch_indices_offset, result->count - mesh_start);
        map_write_struct(result, mesh.indices.data, mesh.indices.count * sizeof(uint16_t));
        if (!decals) {
            WriteBackpatch(int32_t, backpatch_unknown, (result->count - mesh_start) - mesh.diff_between_unknown_value_and_index_buffer_end);
        }

        // Meshes can *START* misaligned, but they end aligned. (aligned *to* weird misalignment that sometimes happens.)
        while (result->count % 16 != misalignment) {
            result->push(0);
        }

        mesh_index++;
    }
    assert(result->count % 16 == misalignment);
}
static int32_t map_compute_file_length(G &g) {
    ProfileFunction();

    int32_t file_length = 0;
    file_length += sizeof(PH2MAP__Header);
    int subfile_count = 0;
    for (auto &sub : g.texture_subfiles) {
        if (sub.came_from_non_numbered_dependency) { // @Temporary i hope!
            continue;
        }
        ++subfile_count;
        file_length += sizeof(PH2MAP__Subfile_Header) + sizeof(PH2MAP__Texture_Subfile_Header);
        for (auto &tex : sub.textures) {
            file_length += sizeof(PH2MAP__BC_Texture_Header) + tex.sprite_count * sizeof(PH2MAP__Sprite_Header) + (int32_t)tex.blob.count;
        }
        file_length += 16;
    }
    for (;;) {
        int geometry_start = 0;
        int geometry_count = 0;
        int geometry_index = 0;
        for (auto &geo : g.geometries) {
            if ((int)geo.subfile_index == subfile_count) {
                if (!geometry_count) {
                    geometry_start = geometry_index;
                }
                ++geometry_count;
            }
            geometry_index++;
        }
        if (geometry_count <= 0) {
            break;
        }
        file_length += sizeof(PH2MAP__Subfile_Header) + sizeof(PH2MAP__Geometry_Subfile_Header);
        int misalignment = 0;
        geometry_index = 0;
        for (auto &geo : g.geometries) {
            defer { geometry_index++; };
            if (geometry_index < geometry_start) {
                continue;
            }
            if (geometry_index >= geometry_start + geometry_count) {
                break;
            }
            file_length += sizeof(PH2MAP__Geometry_Header);
            if (!geo.opaque_meshes.empty()) {
                file_length += (1 + (int)geo.opaque_meshes.count()) * sizeof(uint32_t); // count + offsets
                for (auto &mesh : geo.opaque_meshes) {
                    file_length += sizeof(PH2MAP__Mapmesh_Header);
                    for (auto &mesh_part_group : mesh.mesh_part_groups) {
                        file_length += sizeof(PH2MAP__Mesh_Part_Group_Header);
                        file_length += (int)(mesh_part_group.mesh_parts.count * sizeof(PH2MAP__Mesh_Part));
                    }
                    file_length += sizeof(PH2MAP__Vertex_Sections_Header);
                    file_length += (int)(mesh.vertex_buffers.count * sizeof(PH2MAP__Vertex_Section_Header));
                    for (auto &section : mesh.vertex_buffers) {
                        file_length += section.num_vertices * section.bytes_per_vertex;
                    }
                    file_length += (int)(mesh.indices.count * sizeof(uint16_t));
                    file_length += 15;
                    file_length &= ~15;
                }
            }
            if (!geo.transparent_meshes.empty()) {
                if (geo.has_weird_2_byte_misalignment_before_transparents) {
                    file_length += 2;
                }
                file_length += (1 + (int)geo.transparent_meshes.count()) * sizeof(uint32_t); // count + offsets
                for (auto &mesh : geo.transparent_meshes) {
                    file_length += sizeof(PH2MAP__Mapmesh_Header);
                    for (auto &mesh_part_group : mesh.mesh_part_groups) {
                        file_length += sizeof(PH2MAP__Mesh_Part_Group_Header);
                        file_length += (int)(mesh_part_group.mesh_parts.count * sizeof(PH2MAP__Mesh_Part));
                    }
                    file_length += sizeof(PH2MAP__Vertex_Sections_Header);
                    file_length += (int)(mesh.vertex_buffers.count * sizeof(PH2MAP__Vertex_Section_Header));
                    for (auto &section : mesh.vertex_buffers) {
                        file_length += section.num_vertices * section.bytes_per_vertex;
                    }
                    file_length += (int)(mesh.indices.count * sizeof(uint16_t));
                    if (geo.has_weird_2_byte_misalignment_before_transparents) {
                        misalignment = 2;
                    }
                    while (file_length % 16 != misalignment) {
                        ++file_length;
                    }
                }
            }
            if (!geo.decal_meshes.empty()) {
                if (geo.has_weird_2_byte_misalignment_before_decals) {
                    file_length += 2;
                }
                file_length += (1 + (int)geo.decal_meshes.count()) * sizeof(uint32_t); // count + offsets
                for (auto &decal : geo.decal_meshes) {
                    file_length += sizeof(PH2MAP__Decal_Header);
                    file_length += (int)(decal.mesh_part_groups.count() * sizeof(PH2MAP__Sub_Decal));
                    file_length += sizeof(PH2MAP__Vertex_Sections_Header);
                    file_length += (int)(decal.vertex_buffers.count * sizeof(PH2MAP__Vertex_Section_Header));
                    for (auto &section : decal.vertex_buffers) {
                        file_length += section.num_vertices * section.bytes_per_vertex;
                    }
                    file_length += (int)(decal.indices.count * sizeof(uint16_t));
                    if (geo.has_weird_2_byte_misalignment_before_transparents || geo.has_weird_2_byte_misalignment_before_decals) {
                        misalignment = 2;
                    }
                    while (file_length % 16 != misalignment) {
                        ++file_length;
                    }
                }
            }
        }
        for (auto &mat : g.materials) {
            if ((int)mat.subfile_index == subfile_count) {
                file_length += sizeof(PH2MAP__Material);
            }
        }
        while (file_length % 16 != misalignment) {
            file_length++;
        }
        ++subfile_count;
    }
    return file_length;
}
static void map_write_to_memory(G &g, Array<uint8_t> *result) {
    ProfileFunction();

    int32_t file_length = map_compute_file_length(g);
    result->reserve(file_length);

    // MAP Header
    WriteLit(uint32_t, 0x20010510);
    WriteLit(uint32_t, file_length);
    auto backpatch_subfile_count = CreateBackpatch(uint32_t);
    WriteLit(uint32_t, 0);

    int subfile_count = 0;
    for (auto &sub : g.texture_subfiles) {
        if (sub.came_from_non_numbered_dependency) { // @Temporary i hope!
            continue;
        }

        // Subfile Header
        WriteLit(uint32_t, 2); // subfile type
        auto backpatch_subfile_length = CreateBackpatch(uint32_t);
        WriteLit(uint32_t, 0); // pad
        WriteLit(uint32_t, 0); // pad

        // Texture Subfile Header
        int64_t subfile_start = result->count;
        PH2MAP__Texture_Subfile_Header texture_subfile_header = {};
        texture_subfile_header.magic = 0x19990901;
        texture_subfile_header.always1 = 1;
        Write(texture_subfile_header);

        for (auto &tex : sub.textures) {
            PH2MAP__BC_Texture_Header bc_texture_header = {};
            bc_texture_header.id = tex.id;
            bc_texture_header.width = tex.width;
            bc_texture_header.height = tex.height;
            bc_texture_header.width2 = tex.width;
            bc_texture_header.height2 = tex.height;
            bc_texture_header.sprite_count = tex.sprite_count;
            bc_texture_header.material = tex.material;
            bc_texture_header.material2 = tex.material;
            Write(bc_texture_header);
            assert(tex.sprite_count > 0 && tex.sprite_count <= 64);

            for (int sprite_index = 0; sprite_index < tex.sprite_count; sprite_index++) {
                PH2MAP__Sprite_Header sprite_header = {};
                sprite_header.id = tex.sprite_metadata[sprite_index].id;
                sprite_header.width = bc_texture_header.width;
                sprite_header.height = bc_texture_header.height;
                sprite_header.format = tex.sprite_metadata[sprite_index].format;
                if (sprite_index == tex.sprite_count - 1) {
                    sprite_header.data_length = (uint32_t)tex.blob.count;
                } else {
                    sprite_header.data_length = 0;
                }
                sprite_header.data_length_plus_header = sprite_header.data_length + 16;
                sprite_header.always0x99000000 = 0x99000000;
                Write(sprite_header);
            }
            map_write_struct(result, tex.blob.data, tex.blob.count);
        }
        // textures are read until the first int of the line is 0, and then that line is skipped - hence, we add this terminator sentinel line here
        WriteLit(PH2MAP__BC_End_Sentinel, PH2MAP__BC_End_Sentinel{});

        WriteBackpatch(uint32_t, backpatch_subfile_length, result->count - subfile_start);
        ++subfile_count;
    }

    // GeometryGroup Subfiles
    assert(subfile_count >= 0);
    for (;;) { // Write geometry subfiles and geometries
        int geometry_start = 0;
        int geometry_count = 0;
        int geometry_index = 0;
        for (auto &geo : g.geometries) {
            if ((int)geo.subfile_index == subfile_count) {
                if (!geometry_count) {
                    geometry_start = geometry_index;
                }
                ++geometry_count;
            }
            geometry_index++;
        }
        if (geometry_count <= 0) {
            break;
        }

        // Subfile Header
        WriteLit(uint32_t, 1);
        auto backpatch_subfile_length = CreateBackpatch(uint32_t);
        WriteLit(uint32_t, 0);
        WriteLit(uint32_t, 0);

        // Geometry Subfile Header
        int64_t subfile_start = result->count;
        WriteLit(uint32_t, 0x20010730);
        WriteLit(uint32_t, geometry_count);
        auto backpatch_geometry_size = CreateBackpatch(uint32_t);
        auto backpatch_material_count = CreateBackpatch(uint32_t);

        // @Note: we start out 16-byte aligned here.
        int misalignment = 0;
        geometry_index = 0;
        for (auto &geo : g.geometries) {
            defer { geometry_index++; };
            if (geometry_index < geometry_start) {
                continue;
            }
            if (geometry_index >= geometry_start + geometry_count) {
                break;
            }

            // Geometry Header
            int64_t geometry_header_start = result->count;
            WriteLit(uint32_t, geo.id);
            auto backpatch_group_size = CreateBackpatch(int32_t);
            auto backpatch_opaque_group_offset = CreateBackpatch(int32_t);
            auto backpatch_transparent_group_offset = CreateBackpatch(int32_t);
            auto backpatch_decal_group_offset = CreateBackpatch(int32_t);

            if (!geo.opaque_meshes.empty()) {
                assert(result->count % 16 == 4);
                misalignment = 0;
                WriteBackpatch(int32_t, backpatch_opaque_group_offset, result->count - geometry_header_start);
                map_write_mesh_group_or_decal_group(result, false, geo.opaque_meshes, misalignment);
                assert(result->count % 16 == misalignment);
            }
            if (!geo.transparent_meshes.empty()) {
                if (geo.has_weird_2_byte_misalignment_before_transparents) {
                    result->push(0);
                    result->push(0);
                    misalignment = 2;
                }
                if (geo.opaque_meshes.empty()) {
                    assert(result->count % 16 == 4);
                } else {
                    assert(result->count % 16 == misalignment);
                }
                WriteBackpatch(int32_t, backpatch_transparent_group_offset, result->count - geometry_header_start);
                map_write_mesh_group_or_decal_group(result, false, geo.transparent_meshes, misalignment);
                assert(result->count % 16 == misalignment);
            }
            if (!geo.decal_meshes.empty()) {
                if (geo.has_weird_2_byte_misalignment_before_transparents || geo.has_weird_2_byte_misalignment_before_decals) {
                    while (result->count % 16 != 2) result->push(0);
                    misalignment = 2;
                }
                if (geo.opaque_meshes.empty() && geo.transparent_meshes.empty()) {
                    assert(result->count % 16 == 4);
                } else {
                    assert(result->count % 16 == misalignment);
                }
                WriteBackpatch(int32_t, backpatch_decal_group_offset, result->count - geometry_header_start);
                map_write_mesh_group_or_decal_group(result, true, geo.decal_meshes, misalignment);
                assert(result->count % 16 == misalignment);
            }
            WriteBackpatch(int32_t, backpatch_group_size, result->count - geometry_header_start);
        }

        WriteBackpatch(uint32_t, backpatch_geometry_size, result->count - subfile_start);

        int material_count = 0;
        for (auto &mat : g.materials) {
            if (mat.subfile_index < (uint32_t)subfile_count) {
                continue;
            }
            if (mat.subfile_index > (uint32_t)subfile_count) {
                break;
            }
            ++material_count;
            PH2MAP__Material material = {};
            material.mode = mat.mode;
            material.texture_id = mat.texture_id;
            material.diffuse_color = mat.diffuse_color;
            material.specular_color = mat.specular_color;
            material.specularity = mat.specularity;
            Write(material);
        }
        WriteBackpatch(uint32_t, backpatch_material_count, material_count);

        WriteBackpatch(uint32_t, backpatch_subfile_length, result->count - subfile_start);
        while (result->count % 16 != misalignment) {
            result->push(0);
        }
        ++subfile_count;
    }
    WriteBackpatch(uint32_t, backpatch_subfile_count, subfile_count);
    assert(file_length == result->count);
}
static MAP_Texture *map_get_texture_by_id(G &g, uint32_t id) {
    ProfileFunction();

    for (auto &subfile : g.texture_subfiles) {
        for (auto &tex : subfile.textures) {
            if (tex.id == id) {
                return &tex;
            }
        }
    }

    return nullptr;
}
static MAP_Texture_Buffer *map_get_texture_buffer_by_id(G &g, uint32_t id) {
    // ProfileFunction();

    for (auto &map_tex : g.map_textures) {
        assert(map_tex.subfile_ptr);
        assert(map_tex.texture_ptr);
        if (map_tex.texture_ptr->id == id) {
            return &map_tex;
        }
    }

    return nullptr;
}
static void map_unload(G &g, bool release_only_geometry = false) {
    ProfileFunction();

    if (release_only_geometry) {
        g.release_geometry();
    } else {

        static_cast<Map &>(g) = Map{};

        The_Arena_Allocator::free_all();
        g.clear_undo_redo_stacks();
        g.ui_selected_texture_subfile = nullptr;
        g.ui_selected_texture = nullptr;
    }

    g.staleify_map();
    if (g.opened_map_filename) {
        free(g.opened_map_filename);
        g.opened_map_filename = nullptr;
    }

    for (auto &buf : g.map_buffers) {
        buf.shown = true;
        buf.selected = false;
    }

}

static const char *get_non_numbered_dependency_filename(const char *filename) {
    ProfileFunction();

    int len = (int)strlen(filename);
    int cap = len + 1 + 3; // "folder/aa0" -> "folder/aa.map" expands string by 3 chars max
    char *non_numbered = (char *) malloc(cap);
    if (non_numbered) {
        strncpy(non_numbered, filename, len + 1);
        non_numbered[len] = '\0';
        int i = len - 1;
        for (; i >= 0; i--) {
            if (non_numbered[i] == '/' || non_numbered[i] == '\\') {
                break;
            }
        }
        if (i + 3 < len) {
            i += 3;
            if (non_numbered[i] >= '0' && non_numbered[i] <= '9') {
                non_numbered[i] = '.';
                ++i; assert(i < cap);
                non_numbered[i] = 'm';
                ++i; assert(i < cap);
                non_numbered[i] = 'a';
                ++i; assert(i < cap);
                non_numbered[i] = 'p';
                ++i; assert(i < cap);
                non_numbered[i] = '\0';
                ++i; assert(i <= cap);
            }
        }
    }
    return non_numbered;
}

static float (vertex_format_mode_histogram[4])[6] = {};
static float (texture_format_mode_histogram[4])[6] = {};
static float (mode_unknown_histogram[6])[17] = {};
static float (texture_format_unknown_histogram[4])[17] = {};

static void map_load(G &g, const char *filename, bool is_non_numbered_dependency = false, bool round_trip_test = false) {
    ProfileFunction();

    {
        auto filename16 = utf8_to_utf16(filename);
        if (!filename16) {
            LogErr("Couldn't convert filename! Memory error? Sorry!");
        }
        defer { free(filename16); };

        uint16_t buffer[65536];

        auto result = GetFullPathNameW((LPCWSTR)filename16, (DWORD)countof(buffer), (LPWSTR)buffer, nullptr);
        assert(result > 2 && result < countof(buffer));

        auto absolute_filename = utf16_to_utf8(buffer);
        assert(absolute_filename);
        filename = absolute_filename; // Sometimes @Leak of old filename? Don't care tbh
    }

    map_unload(g, is_non_numbered_dependency);

    if (!is_non_numbered_dependency) { // Semi-garbage code.
        auto non_numbered = get_non_numbered_dependency_filename(filename);
        assert(non_numbered);
        if (non_numbered) {
            defer { free((void *)non_numbered); };
            if (strcmp(filename, non_numbered) != 0) {
                // Log("Loading \"%s\" for base textures", mem);
                map_load(g, non_numbered, true, round_trip_test);
            }
        } else {
            LogErr("MAP Load Error: Couldn't generate non-numbered dependency filename.");
            LogErr("MAP Load Error: Couldn't open non-numbered dependency file.");
        }
    }
    int prev_num_array_resizes = num_array_resizes;
    defer {
        // Log("%d array resizes", num_array_resizes - prev_num_array_resizes);
    };
    
    enum { MAP_FILE_DATA_LENGTH_MAX = 32 * 1024 * 1024 }; // @Temporary?    
    static char filedata_do_not_modify_me_please[MAP_FILE_DATA_LENGTH_MAX]; // @Temporary
    uint32_t file_len_do_not_modify_me_please = 0;
    {
        {
            FILE *f = PH2CLD__fopen(filename, "rb");
            if (!f) {
                LogErr("Failed loading MAP file \"%s\"!", filename);
                return;
            }
            assert(f);
            defer {
                fclose(f);
            };

            // @Temporary @Debug
            const char *ptr = nullptr;
            const char *end = nullptr;
            PH2MAP__Header header = {};
            {
                char *filedata = filedata_do_not_modify_me_please;
                assert((uintptr_t)filedata % 16 == 0);
                uint32_t file_len = (uint32_t)fread(filedata, 1, MAP_FILE_DATA_LENGTH_MAX, f);
                file_len_do_not_modify_me_please = file_len;
                ptr = filedata;
                end = filedata + file_len;
                Read(ptr, header);
                assert(header.magic == 0x20010510);
                assert(header.file_length == file_len);
                assert(header.padding0 == 0);
            }
            // Log("Map \"%s\" has %u subfiles.", filename, header.subfile_count);
            bool has_ever_seen_geometry_subfile = false;
            for (uint32_t subfile_index = 0; subfile_index < header.subfile_count; subfile_index++) {
                PH2MAP__Subfile_Header subfile_header = {};
                Read(ptr, subfile_header);
                // Log("In subfile %u", subfile_index);
                assert(subfile_header.type == 1 || subfile_header.type == 2);
                assert(subfile_header.padding0 == 0);
                assert(subfile_header.padding1 == 0);
                if (subfile_header.type == 1) { // Geometry subfile
                    has_ever_seen_geometry_subfile = true;
                    auto ptr2 = ptr;

                    assert(ptr + subfile_header.length <= end);
                    ptr += subfile_header.length;
                    
                    PH2MAP__Geometry_Subfile_Header geometry_subfile_header = {};
                    Read(ptr2, geometry_subfile_header);
                    assert(geometry_subfile_header.magic == 0x20010730);
                    assert(geometry_subfile_header.geometry_count >= 1);
                    assert(geometry_subfile_header.material_count < 65536);
                    assert(geometry_subfile_header.geometry_size == subfile_header.length - geometry_subfile_header.material_count * sizeof(PH2MAP__Material));
                    // g.geometries.reserve(g.geometries.count + geometry_subfile_header.geometry_count);
                    for (uint32_t geometry_index = 0; geometry_index < geometry_subfile_header.geometry_count; geometry_index++) {
                        // Log("  In geometry %u", geometry_index);
                        const char *geometry_start = ptr2;
                        // assert((uintptr_t)geometry_start % 16 == 0);
                        PH2MAP__Geometry_Header geometry_header = {};
                        Read(ptr2, geometry_header);
                        assert(geometry_header.group_size >= sizeof(PH2MAP__Geometry_Header));
                        assert(geometry_header.group_size < 1024 * 1024 * 1024); // please have map files smaller than a gig, i guess.
                        assert(geometry_header.opaque_group_offset >= 0);
                        assert(geometry_header.transparent_group_offset >= 0);
                        assert(geometry_header.decal_group_offset >= 0);
                        assert(geometry_header.opaque_group_offset < geometry_header.group_size);
                        assert(geometry_header.transparent_group_offset < geometry_header.group_size);
                        assert(geometry_header.decal_group_offset < geometry_header.group_size);
                        // Log("%d", geometry_header.id);

                        ptr2 -= sizeof(PH2MAP__Geometry_Header);

                        MAP_Geometry geometry = {};
                        defer {
                            g.geometries.push(geometry);
                        };

                        geometry.subfile_index = subfile_index;
                        geometry.id = geometry_header.id;

                        int length_sum = 0;
                        length_sum += sizeof(PH2MAP__Geometry_Header);

                        int opaque_group_length = 0;
                        int transparent_group_length = 0;
                        int decal_group_length = 0;
                        if (geometry_header.opaque_group_offset) {
                            assert(geometry_header.opaque_group_offset == length_sum);
                            // Log("Opaque!");
                            const char *mesh_group_header = ptr2 + geometry_header.opaque_group_offset;
                            int bytes = map_load_mesh_group_or_decal_group(&geometry.opaque_meshes, 0, mesh_group_header, ptr2 + geometry_header.group_size, false);
                            opaque_group_length += bytes;
                        }
                        length_sum += opaque_group_length;
                        if (geometry_header.transparent_group_offset) {
                            if (geometry_header.transparent_group_offset == length_sum + 2) {
                                geometry.has_weird_2_byte_misalignment_before_transparents = true;
                                assert(ptr2[length_sum] == 0);
                                assert(ptr2[length_sum + 1] == 0);
                                length_sum += 2;
                            } else {
                                assert(geometry_header.transparent_group_offset == length_sum);
                            }
                            // Log("Transparent!");
                            const char *mesh_group_header = ptr2 + geometry_header.transparent_group_offset;
                            int bytes = map_load_mesh_group_or_decal_group(&geometry.transparent_meshes, geometry.has_weird_2_byte_misalignment_before_transparents ? 2 : 0, mesh_group_header, ptr2 + geometry_header.group_size, false);
                            transparent_group_length += bytes;
                        }
                        length_sum += transparent_group_length;
                        if (geometry_header.decal_group_offset) {
                            if (geometry_header.decal_group_offset == length_sum + 2) {
                                geometry.has_weird_2_byte_misalignment_before_decals = true;
                                assert(ptr2[length_sum] == 0);
                                assert(ptr2[length_sum + 1] == 0);
                                length_sum += 2;
                            } else {
                                assert(geometry_header.decal_group_offset == length_sum);
                            }
                            // Log("Decals!");
                            const char *decal_group_header = ptr2 + geometry_header.decal_group_offset;
                            int bytes = map_load_mesh_group_or_decal_group(&geometry.decal_meshes, geometry.has_weird_2_byte_misalignment_before_transparents || geometry.has_weird_2_byte_misalignment_before_decals ? 2 : 0, decal_group_header, ptr2 + geometry_header.group_size, true);
                            decal_group_length += bytes;
                        }
                        length_sum += decal_group_length;
                        assert(length_sum == geometry_header.group_size);

                        ptr2 += geometry_header.group_size;

                    }
                    // g.materials.reserve(g.materials.count + geometry_subfile_header.material_count);
                    for (uint32_t material_index = 0; material_index < geometry_subfile_header.material_count; material_index++) {
                        PH2MAP__Material material = {};
                        Read(ptr2, material);
                        assert(PH2CLD__sanity_check_float(material.specularity));
                        assert(PH2MAP_material_mode_is_valid(material.mode));
                        assert((material.diffuse_color & 0xff000000) == 0xff000000);
                        assert(material.specular_color == 0 || (material.specular_color & 0xff000000) == 0xff000000);
                        assert(material.specularity >= 0);
                        assert(material.specularity <= 300);
                        static bool specularities[301];
                        if (!specularities[(int)material.specularity]) {
                            specularities[(int)material.specularity] = true;
                            Log("Specularity %f", material.specularity);
                        }

                        if (material.mode == 0) { // 0 - Emissive
                            assert((material.specular_color & 0x00ffffff) == 0);
                            assert(material.specularity == 0);
                        } else if (material.mode == 1) { // 1 - Coloured Diffuse
                            assert((material.specular_color & 0x00ffffff) == 0);
                            assert(material.specularity == 0);
                        } else if (material.mode == 2) { // 2 - Coloured Diffuse + Coloured Specular
                            assert((material.diffuse_color & 0x00ffffff) > 0);
                            assert((material.specular_color & 0x00ffffff) > 0);
                            assert(material.specularity > 0);
                        } else if (material.mode == 3) { // 3 - VantaBlack (totally black) // @Todo: how does the texture format work here?
                            assert((material.diffuse_color & 0x00ffffff) == 0);
                            assert((material.specular_color & 0x00ffffff) == 0);
                            assert(material.specularity == 0);
                        } else if (material.mode == 4) { // 4 - Just Diffuse (material diffuse colour overridden to white)
                            assert((material.diffuse_color & 0x00ffffff) == 0);
                            assert((material.specular_color & 0x00ffffff) == 0);
                            assert(material.specularity == 0);
                        } else if (material.mode == 6) { // 6 - Unknown - also Coloured Diffuse?
                            assert((material.diffuse_color & 0x00ffffff) > 0);
                            assert((material.specular_color & 0x00ffffff) == 0);
                            assert(material.specularity == 0);
                        }

                        MAP_Material mat = {};
                        defer {
                            g.materials.push(mat);
                        };
                        mat.subfile_index = subfile_index;
                        mat.mode = material.mode;
                        mat.texture_id = material.texture_id;
                        mat.diffuse_color = material.diffuse_color;
                        mat.specular_color = material.specular_color;
                        mat.specularity = material.specularity;
                    }
                    assert(ptr2 == ptr);
                } else if (subfile_header.type == 2) { // Texture subfile
                    assert(!has_ever_seen_geometry_subfile);
                    assert(ptr + subfile_header.length <= end);
                    auto end = ptr + subfile_header.length;
                    if (!is_non_numbered_dependency) {
                        // Log("Subfile %d is %d bytes", subfile_index, subfile_header.length);
                    }
                    PH2MAP__Texture_Subfile_Header texture_subfile_header = {};
                    Read(ptr, texture_subfile_header);
                    assert(texture_subfile_header.magic == 0x19990901);
                    assert(texture_subfile_header.pad[0] == 0);
                    assert(texture_subfile_header.pad[1] == 0);
                    assert(texture_subfile_header.always1 == 1);

                    MAP_Texture_Subfile &texture_subfile = *g.texture_subfiles.push(); // @Cleanup: replace ref with ptr here
                    texture_subfile.came_from_non_numbered_dependency = is_non_numbered_dependency;

                    for (; ; ) {
                        {
                            auto ptr2 = ptr;
                            // "read until the first int of the line is 0, and then skip that line"
                            PH2MAP__BC_End_Sentinel bc_end_sentinel = {};
                            Read(ptr2, bc_end_sentinel);
                            if (bc_end_sentinel.line_check == 0) {
                                assert(bc_end_sentinel.zero[0] == 0);
                                assert(bc_end_sentinel.zero[1] == 0);
                                assert(bc_end_sentinel.zero[2] == 0);
                                ptr = ptr2;
                                break;
                            }
                        }

                        PH2MAP__BC_Texture_Header bc_texture_header = {};
                        Read(ptr, bc_texture_header);
                        assert(bc_texture_header.id <= 0xffff);
                        assert(bc_texture_header.width == bc_texture_header.width2);
                        assert(bc_texture_header.height == bc_texture_header.height2);
                        assert(bc_texture_header.sprite_count <= 0xff);
                        assert((bc_texture_header.material >= 0x1 && bc_texture_header.material <= 0x10) ||
                            bc_texture_header.material == 0x28);
                        assert(bc_texture_header.material2 == bc_texture_header.material);
                        assert(bc_texture_header.pad[0] == 0);
                        assert(bc_texture_header.pad[1] == 0);
                        assert(bc_texture_header.pad[2] == 0);

                        MAP_Texture &tex = *texture_subfile.textures.push(); // @Cleanup: replace ref with ptr here
                        tex.id = (uint16_t)bc_texture_header.id;
                        tex.width = bc_texture_header.width;
                        tex.height = bc_texture_header.height;
                        tex.material = (uint8_t)bc_texture_header.material;
                        tex.sprite_count = (uint8_t)bc_texture_header.sprite_count;

                        // Log("texture index is %d, id is %d", (int)g.textures.count - 1, (int)tex.id);

                        for (size_t sprite_index = 0; sprite_index < bc_texture_header.sprite_count; sprite_index++) {
                            PH2MAP__Sprite_Header sprite_header = {};
                            Read(ptr, sprite_header);

                            assert(sprite_header.x == 0);
                            assert(sprite_header.y == 0);
                            assert(sprite_header.width == bc_texture_header.width);
                            assert(sprite_header.height == bc_texture_header.height);

                            assert(sprite_header.id <= 0xffff);
                            assert(sprite_header.format == 0x100 ||
                                sprite_header.format == 0x102 ||
                                sprite_header.format == 0x103 ||
                                sprite_header.format == 0x104);

                            assert(sprite_header.data_length + 16 == sprite_header.data_length_plus_header);
                            assert(sprite_header.pad == 0);
                            assert(sprite_header.always0x99000000 == 0x99000000);

                            auto pixels_data = ptr;
                            uint32_t pixels_len = sprite_header.data_length;
                            auto pixels_end = pixels_data + pixels_len;

                            tex.sprite_metadata[sprite_index].id = (uint16_t)sprite_header.id;
                            tex.sprite_metadata[sprite_index].format = (uint16_t)sprite_header.format;

                            if (sprite_index == bc_texture_header.sprite_count - 1) {
                                assert(pixels_len > 0);

                                tex.blob.resize(pixels_len);
                                assert(tex.blob.data);
                                memcpy(tex.blob.data, pixels_data, tex.blob.count);

                                if (tex.sprite_metadata[sprite_index].format == 0x100) {
                                    tex.format = MAP_Texture_Format_BC1;
                                } else if (tex.sprite_metadata[sprite_index].format == 0x102) {
                                    tex.format = MAP_Texture_Format_BC2;
                                } else if (tex.sprite_metadata[sprite_index].format == 0x103) {
                                    tex.format = MAP_Texture_Format_BC3;
                                } else if (tex.sprite_metadata[sprite_index].format == 0x104) {
                                    tex.format = MAP_Texture_Format_BC3_Maybe;
                                } else {
                                    assert(false);
                                }
                            } else {
                                assert(pixels_len == 0);
                            }
                            assert(pixels_end <= end);
                            ptr = pixels_end;
                        }
                    }
                    assert(ptr == end);
                } else {
                    assert(0);
                }
            }
            assert(end - ptr < 16);
            for (; ptr < end; ptr++) {
                assert(*ptr == 0);
            }
        }
    }
    
    // upload textures and geometries IFF this is a top-level call
    if (!is_non_numbered_dependency) {
        if (round_trip_test) { // Round-trip test
            Array<uint8_t> round_trip = {};
            defer {
                round_trip.release();
            };
            map_write_to_memory(g, &round_trip);
            assert(round_trip.count == file_len_do_not_modify_me_please);
            assert(memcmp(round_trip.data, filedata_do_not_modify_me_please, round_trip.count) == 0);
        }
    }

    g.opened_map_filename = strdup(filename);
    assert(g.opened_map_filename);

    {
        for (MAP_Geometry &geo : g.geometries) {
            LinkedList<MAP_Mesh, The_Arena_Allocator> *lists[3] = { &geo.opaque_meshes, &geo.transparent_meshes, &geo.decal_meshes };
            // MAP_Geometry_Buffer_Source sources[countof(lists)] = { MAP_Geometry_Buffer_Source::Opaque, MAP_Geometry_Buffer_Source::Transparent, MAP_Geometry_Buffer_Source::Decal };
            for (int i = 0; i < countof(lists); ++i) {
                auto &meshes = *lists[i];
                // auto source = sources[i];
                for (MAP_Mesh &mesh : meshes) {
                    for (MAP_Mesh_Part_Group &mpg : mesh.mesh_part_groups) {
                        int bytes_per_vertex = mesh.vertex_buffers[mpg.section_index].bytes_per_vertex;
                        MAP_Material *material = g.materials.at_index(mpg.material_index); 
                        if (!material) continue;

                        int mode = material->mode;
                        if (PH2MAP_material_mode_is_valid(mode)) {
                            if (mode == 6) {
                                mode = 5;
                            }
                        } else {
                            assert(false);
                        }
                        if (bytes_per_vertex == 0x14) {
                            bytes_per_vertex = 0;
                        } else if (bytes_per_vertex == 0x18) {
                            bytes_per_vertex = 1;
                        } else if (bytes_per_vertex == 0x20) {
                            bytes_per_vertex = 2;
                        } else if (bytes_per_vertex == 0x24) {
                            bytes_per_vertex = 3;
                        } else {
                            assert(false);
                        }

                        (vertex_format_mode_histogram[bytes_per_vertex])[mode] += 1;
                    }
                }
            }
        }

        for (MAP_Material &mat : g.materials) {
            MAP_Texture *tex = map_get_texture_by_id(g, mat.texture_id);
            if (!tex) continue;

            int mode = mat.mode;
            int format = tex->format;

            if (PH2MAP_material_mode_is_valid(mode)) {
                if (mode == 6) {
                    mode = 5;
                }
            } else {
                assert(false);
            }
            if (format == MAP_Texture_Format_BC1) {
                format = 0;
            } else if (format == MAP_Texture_Format_BC2) {
                format = 1;
            } else if (format == MAP_Texture_Format_BC3) {
                format = 2;
            } else if (format == MAP_Texture_Format_BC3_Maybe) {
                format = 3;
            } else {
                assert(false);
            }

            (texture_format_mode_histogram[format])[mode] += 1;

            int unknown = tex->material;
            if (unknown >= 1 && unknown <= 16) {
                unknown -= 1;
            } else if (unknown == 0x28) {
                unknown = 16;
            } else {
                assert(false);
            }

            (mode_unknown_histogram[mode])[unknown] += 1;
        }
        for (MAP_Texture_Subfile &sub : g.texture_subfiles) {
            for (MAP_Texture &tex : sub.textures) {
                int unknown = tex.material;
                int format = tex.format;

                if (unknown >= 1 && unknown <= 16) {
                    unknown -= 1;
                } else if (unknown == 0x28) {
                    unknown = 16;
                } else {
                    assert(false);
                }
                if (format == MAP_Texture_Format_BC1) {
                    format = 0;
                } else if (format == MAP_Texture_Format_BC2) {
                    format = 1;
                } else if (format == MAP_Texture_Format_BC3) {
                    format = 2;
                } else if (format == MAP_Texture_Format_BC3_Maybe) {
                    format = 3;
                } else {
                    assert(false);
                }

                (texture_format_unknown_histogram[format])[unknown] += 1;
            }
        }
    }

    g.solo_material = -1;
}
static void map_upload(G &g) {
    ProfileFunction();

    g.map_buffers_count = 0;
    assert(g.map_textures.count == 0);
    for (auto &geo : g.geometries) {
        LinkedList<MAP_Mesh, The_Arena_Allocator> *lists[3] = { &geo.opaque_meshes, &geo.transparent_meshes, &geo.decal_meshes };
        MAP_Geometry_Buffer_Source sources[countof(lists)] = { MAP_Geometry_Buffer_Source::Opaque, MAP_Geometry_Buffer_Source::Transparent, MAP_Geometry_Buffer_Source::Decal };
        for (int i = 0; i < countof(lists); ++i) {
            auto &meshes = *lists[i];
            auto source = sources[i];
            int mesh_index = 0;
            for (MAP_Mesh &mesh : meshes) {

                assert(g.map_buffers_count < g.map_buffers_max);
                auto &map_buffer = g.map_buffers[g.map_buffers_count++];
                map_buffer.source = source;
                map_buffer.id = geo.id;
                map_buffer.subfile_index = geo.subfile_index;
                map_buffer.geometry_ptr = &geo;
                map_buffer.mesh_ptr = &mesh;
                map_buffer.mesh_index = mesh_index;

                {
                    map_buffer.vertices.clear();
                    int vertices_reserve = 0;
                    for (MAP_Mesh_Vertex_Buffer &vertex_buffer : mesh.vertex_buffers) {
                        vertices_reserve += vertex_buffer.num_vertices;
                    }
                    map_buffer.vertices.reserve(vertices_reserve);
                    map_unpack_mesh_vertex_buffer(map_buffer, mesh);
                }

                {
                    map_buffer.indices.clear();
                    int indices_reserve = 0;
                    for (MAP_Mesh_Part_Group &mesh_part_group : mesh.mesh_part_groups) {
                        for (MAP_Mesh_Part &mesh_part : mesh_part_group.mesh_parts) {
                            indices_reserve += 3 * mesh_part.strip_count * mesh_part.strip_length;
                        }
                    }
                    map_buffer.indices.reserve(indices_reserve);
                }

                {
                    map_buffer.vertices_per_mesh_part_group.clear();
                }

                int indices_index = 0;
                for (MAP_Mesh_Part_Group &mesh_part_group : mesh.mesh_part_groups) {
                    map_destrip_mesh_part_group(map_buffer, indices_index, mesh, mesh_part_group);
                }
                assert(map_buffer.indices.count % 3 == 0);

                ++mesh_index;
            }
        }
    }
    for (auto &sub : g.texture_subfiles) {
        for (auto &tex : sub.textures) {
            ProfileScope("Upload texture");

            sg_image_desc d = {};
            d.width = tex.width;
            d.height = tex.height;
            switch (tex.format) {
                case MAP_Texture_Format_BC1:       d.pixel_format = SG_PIXELFORMAT_BC1_RGBA; break;
                case MAP_Texture_Format_BC2:       d.pixel_format = SG_PIXELFORMAT_BC2_RGBA; break;
                case MAP_Texture_Format_BC3:       d.pixel_format = SG_PIXELFORMAT_BC3_RGBA; break;
                case MAP_Texture_Format_BC3_Maybe: d.pixel_format = SG_PIXELFORMAT_BC3_RGBA; break;
                default: assert(false); break;
            };
            d.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            d.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            d.min_filter = SG_FILTER_NEAREST;
            d.mag_filter = SG_FILTER_NEAREST;
            d.max_anisotropy = 16;
            d.data.subimage[0][0] = { tex.blob.data, (size_t)tex.blob.count };
            auto &new_tex = *g.map_textures.push();
            new_tex.tex = sg_make_image(d);
            assert(new_tex.tex.id);
            new_tex.width = tex.width;
            new_tex.height = tex.height;
            new_tex.subfile_ptr = &sub;
            new_tex.texture_ptr = &tex;
        }

        // Log("%lld geometries", g.geometries.count);
        // for (auto &geo : g.geometries) {
        //     Log("    %u meshes", geo.opaque_meshes.count + geo.transparent_meshes.count + geo.decal_count);
        // }
        // Log("%lld texture subfiles", g.texture_subfiles.count);
        // for (auto &sub : g.texture_subfiles) {
        //     Log("    %u textures%s", sub.texture_count, sub.came_from_non_numbered_dependency ? " (from non-numbered dependency)" : "");
        // }
    }
    for (int i = 0; i < g.map_buffers_count; i++) {
        ProfileScope("Update MAP buffer");

        auto &buf = g.map_buffers[i];
        sg_update_buffer(buf.vertex_buffer, sg_range { buf.vertices.data, buf.vertices.count * sizeof(buf.vertices[0]) });
        sg_update_buffer(buf.index_buffer, sg_range { buf.indices.data, buf.indices.count * sizeof(buf.indices[0]) });

        // Log("Vertex buffer for map buffer #%d is %d vertices", i, (int)buf.vertices.count);
        // Log("Index buffer for map buffer #%d is %d indices", i, (int)buf.indices.count);
    }

}
static void test_all_maps(G &g) {
    ProfileFunction();

    struct _finddata_t find_data;
    intptr_t directory = _findfirst("map/*.map", &find_data);
    assert(directory >= 0);
    int spinner = 0;
    int num_tested = 0;

    memset(vertex_format_mode_histogram, 0, sizeof(vertex_format_mode_histogram));
    memset(texture_format_mode_histogram, 0, sizeof(texture_format_mode_histogram));
    memset(mode_unknown_histogram, 0, sizeof(mode_unknown_histogram));
    memset(texture_format_unknown_histogram, 0, sizeof(texture_format_unknown_histogram));

    while (1) {
        char b[260 + sizeof("map/")];
        snprintf(b, sizeof(b), "map/%s", find_data.name);
        if (find_data.time_write < 1100000000) { // 1100000000 is sometime after 2002...
            // Log("Time write: %d", find_data.time_write);
            // Log("Loading map \"%s\"", b);
            // printf("%c\r", "|/-\\"[spinner++ % 4]);
            sapp_set_window_title(b);
            map_load(g, b, false, true);
            ++num_tested;
        }
        if (_findnext(directory, &find_data) < 0) {
            if (errno == ENOENT) break;
            else assert(0);
        }
    }
    _findclose(directory);
    Log("Tested %d maps.", num_tested);
    for (auto &a : vertex_format_mode_histogram) for (auto &x : a) x /= num_tested;
    for (auto &a : texture_format_mode_histogram) for (auto &x : a) x /= num_tested;
    for (auto &a : mode_unknown_histogram) for (auto &x : a) x /= num_tested;
    for (auto &a : texture_format_unknown_histogram) for (auto &x : a) x /= num_tested;
    map_unload(g);
}

static void imgui_do_console(G &g) {
    ProfileFunction();

    if (!g.show_console) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2 { sapp_width() * 0.66f, sapp_height() * 0.66f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2 { sapp_width() * 0.32f, sapp_height() * 0.32f }, ImGuiCond_FirstUseEver);
    ImGui::Begin("Console", &g.show_console, ImGuiWindowFlags_NoCollapse);
    defer {
        ImGui::End();
    };
    {
        ImGui::BeginChild("scrolling", ImVec2(0, -24 * ImGui::GetIO().FontGlobalScale), false, ImGuiWindowFlags_HorizontalScrollbar);
        // @Hack: For some reason pushing the item width as wrap pos wraps at 2/3rds the window width, so just multiply by 1.5 :)
        ImGui::PushTextWrapPos(ImGui::CalcItemWidth() * 3.0f / 2);
        defer {
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        };
        int log_start = (int)log_buf_index - LOG_MAX;
        if (log_start < 0) {
            log_start = 0;
        }
        for (int i = log_start; i < log_buf_index; i++) {
            ImGui::PushStyleColor(ImGuiCol_Text, log_buf[i % LOG_MAX].colour);
            ImGui::TextWrapped("%s", log_buf[i % LOG_MAX].buf);
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    char buf[sizeof(LogMsg::buf)] = {};
    ImGui::Text("Command:");
    ImGui::SameLine();
    if (ImGui::InputTextWithHint("###console input", "help", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        // Process buf
        LogC(IM_COL32_WHITE, "> %s", buf);
        if (memcmp("cld_load ", buf, sizeof("cld_load ") - 1) == 0) {
            char *p = buf + sizeof("cld_load ") - 1;
            while (isspace(*p)) p++;
            cld_load(g, p);
        } else if (memcmp("map_load ", buf, sizeof("map_load ") - 1) == 0) {
            char *p = buf + sizeof("map_load ") - 1;
            while (isspace(*p)) p++;
            map_load(g, p);
        } else if (memcmp("test_all_maps", buf, sizeof("test_all_maps") - 1) == 0) {
            test_all_maps(g);
        } else if (memcmp("help", buf, sizeof("help") - 1) == 0) {
            Log("Command List:");
            LogC(IM_COL32_WHITE, "  cld_load <filename>");
            Log("    - Loads a .CLD (collision data) file.");
            LogC(IM_COL32_WHITE, "  map_load <filename>");
            Log("    - Loads a .MAP (map textures/geometry) file.");
            LogC(IM_COL32_WHITE, "  test_all_maps");
            Log("    - Developer test tool - loads all maps in the map/ folder (relative to the current working directory).");
        } else {
            Log("Unknown command :)");
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

struct Ray_Vs_Sphere_Result {
    bool hit;
    float t;
    // float exit_t;
};

#define ray_vs_sphere_(result, ro_, rd_, so_, r_) \
    do { \
        auto &&ro = (ro_); \
        auto &&rd = (rd_); \
        auto &&so = (so_); \
        auto &&r = (r_); \
        (result).hit = false; \
        HMM_Vec3 oc = { oc.X = ro.X - so.X, oc.Y = ro.Y - so.Y, oc.Z = ro.Z - so.Z }; \
        float b = (oc.X * rd.X) + (oc.Y * rd.Y) + (oc.Z * rd.Z); \
        float c = (oc.X * oc.X) + (oc.Y * oc.Y) + (oc.Z * oc.Z) - r * r; \
        float h = b * b - c; \
        if (h >= 0.0) { \
            h = sqrtf(h); \
            float t = -b - h; \
            /* float exit_t = -b + h; */ \
            if (t >= 0 /* || exit_t >= 0 */) { \
                (result).hit = true; \
                (result).t = t; \
                /* (result).exit_t = exit_t; */ \
            } \
        } \
    } while (0);

// sphere of size ra centered at point so
Ray_Vs_Sphere_Result ray_vs_sphere(HMM_Vec3 ro_, HMM_Vec3 rd_, HMM_Vec3 so_, float r_) {
    Ray_Vs_Sphere_Result result_;
    ray_vs_sphere_(result_, ro_, rd_, so_, r_);
    return result_;
}

static inline float abs(float x) { return x >= 0 ? x : -x; }

struct Ray_Vs_Plane_Result {
    bool hit;
    float t;
    HMM_Vec3 point;
};

#define ray_vs_triangle_(result, ray_pos, ray_dir, a, b, c) do { \
    auto ray_pos_ = (ray_pos); \
    auto ray_dir_ = (ray_dir); \
    auto a_ = (a); \
    auto b_ = (b); \
    auto c_ = (c); \
    /* assuming ray_d is normalized */ \
    result.hit = false; \
    HMM_Vec3 edge0; edge0.X = b_.X - a_.X; edge0.Y = b_.Y - a_.Y; edge0.Z = b_.Z - a_.Z; \
    HMM_Vec3 edge1; edge1.X = c_.X - a_.X; edge1.Y = c_.Y - a_.Y; edge1.Z = c_.Z - a_.Z; \
    /* pvec = HMM_Cross(ray_dir_, edge1); */ \
    HMM_Vec3 pvec; pvec.X = (ray_dir_.Y * edge1.Z) - (ray_dir_.Z * edge1.Y); pvec.Y = (ray_dir_.Z * edge1.X) - (ray_dir_.X * edge1.Z); pvec.Z = (ray_dir_.X * edge1.Y) - (ray_dir_.Y * edge1.X); \
    /* det = HMM_Dot(edge0, pvec); */ \
    float det = (edge0.X * pvec.X) + (edge0.Y * pvec.Y) + (edge0.Z * pvec.Z); \
    if (det < -0.000001f || det > 0.000001f) { \
        float invDet = 1.0f / det; \
        HMM_Vec3 tvec; tvec.X = ray_pos_.X - a_.X; tvec.Y = ray_pos_.Y - a_.Y; tvec.Z = ray_pos_.Z - a_.Z; \
        /* u = HMM_Dot(tvec, pvec) * invDet; */ \
        float u = ((tvec.X * pvec.X) + (tvec.Y * pvec.Y) + (tvec.Z * pvec.Z)) * invDet; \
        if (u >= 0.0f && u <= 1.0f) { \
            HMM_Vec3 qvec = HMM_Cross(tvec, edge0); \
            float v = HMM_Dot(ray_dir_, qvec) * invDet; \
            if (v >= 0.0f && u + v <= 1.0f) { \
                result.t = HMM_Dot(edge1, qvec) * invDet; \
                if (result.t >= 0) { \
                    result.hit = result.t != INFINITY && result.t != -INFINITY; \
                    result.point = ray_pos_ + ray_dir_ * result.t; \
                    /* //@Speed is it faster to add a "wants_normal" bool here for when we don't require the normal? */ \
                    /* result.hit_surface_normal = normalize(cross(edge0, edge1)); */ \
                } \
            } \
        } \
    } \
} while (0)

// @Attribution: thebaker__ https://twitch.tv/thebaker__
static Ray_Vs_Plane_Result ray_vs_triangle(HMM_Vec3 ray_pos, HMM_Vec3 ray_dir,
                                           HMM_Vec3 a, HMM_Vec3 b, HMM_Vec3 c) {
    Ray_Vs_Plane_Result result;
    ray_vs_triangle_(result, ray_pos, ray_dir, a, b, c);
    return result;
}

Ray_Vs_Plane_Result ray_vs_plane(HMM_Vec3 ray_pos, HMM_Vec3 ray_dir, HMM_Vec3 plane_point, HMM_Vec3 plane_normal) {
    // ProfileFunction();

    Ray_Vs_Plane_Result result = {};

    // assert(HMM_Len(plane_normal) != 0);
    // assert(HMM_Len(ray.dir) != 0);
    plane_normal = HMM_Norm(plane_normal);
    
    HMM_Vec3 displacement = plane_point - ray_pos;
    float distance_to_origin = HMM_Dot(displacement, plane_normal);
    float cos_theta = HMM_Dot(ray_dir, plane_normal);
    if (abs(cos_theta) < 0.0001) {
        result.hit = abs(distance_to_origin) < 0.0001;
        result.t = 0;
    } else {
        result.t = distance_to_origin / cos_theta;
        result.hit = result.t >= 0;
    }
    result.point.X = ray_pos.X + ray_dir.X * result.t;
    result.point.Y = ray_pos.Y + ray_dir.Y * result.t;
    result.point.Z = ray_pos.Z + ray_dir.Z * result.t;

    return result;
}

static bool file_exists(const uint16_t *filename16) {
    ProfileFunction();

    DWORD attr = GetFileAttributesW((LPCWSTR)filename16);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
static bool file_exists(LPCWSTR filename16) { return file_exists((const uint16_t *)filename16); }

static bool file_exists(const char *filename8) {
    auto filename16 = utf8_to_utf16(filename8);
    if (!filename16) return false;
    defer { free(filename16); };

    return file_exists(filename16);
}

void *operator new(size_t, void *ptr) { return ptr; }
static void init(void *userdata) {

    spall_ctx = spall_init_file("trace.spall", get_rdtsc_multiplier());
    assert(spall_ctx.data);
    spall_buffer_init(&spall_ctx, &spall_buffer);

    ProfileFunction();

    double init_time = -get_time();
    defer {
        init_time += get_time();
        // Log("Init() took %f seconds.", init_time);
    };
    The_Arena_Allocator::init();
#ifndef NDEBUG
    MoveWindow(GetConsoleWindow(), +1925, 0, 1500, 800, true);
    MoveWindow((HWND)sapp_win32_get_hwnd(), +1990, 50, 1500, 800, true);
    ShowWindow((HWND)sapp_win32_get_hwnd(), SW_MAXIMIZE);
#endif
    G &g = *(G *)userdata;
    new (userdata) G{};
    g.last_time = get_time();
    sg_desc desc = {};
    desc.context = sapp_sgcontext();
    desc.buffer_pool_size = 256;
    desc.logger.func = slog_func;
    sg_setup(&desc);
    simgui_desc_t simgui_desc = {};
    simgui_desc.no_default_font = true;
    simgui_desc.sample_count = sapp_sample_count();
    // @Note: Someday move imgui.ini to appdata? ==> // SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, NULL, &wszPath);
    simgui_desc.ini_filename = "imgui.ini";
    simgui_setup(&simgui_desc);
    if (!file_exists(L"imgui.ini")) {
        // Giga-@Hack.
        ImGui::LoadIniSettingsFromMemory(R"(
[Window][DockSpaceViewport_11111111]
Pos=0,20
Size=1920,1007
Collapsed=0

[Window][Debug##Default]
Pos=60,60
Size=400,400
Collapsed=0

[Window][Console]
Pos=1051,667
Size=869,360
Collapsed=0
DockId=0x00000006,0

[Window][Editor]
Pos=0,20
Size=392,645
Collapsed=0
DockId=0x00000001,0

[Window][Textures]
Pos=0,667
Size=1049,360
Collapsed=0
DockId=0x00000005,0

[Window][Materials]
Pos=0,667
Size=1049,360
Collapsed=0
DockId=0x00000005,1

[Window][Viewport]
Pos=394,20
Size=1265,645
Collapsed=0
DockId=0x00000007,0

[Window][Edit Widget]
Pos=1661,20
Size=259,645
Collapsed=0
DockId=0x00000008,0

[Docking][Data]
DockSpace       ID=0x8B93E3BD Window=0xA787BDB4 Pos=0,20 Size=1920,1007 Split=Y Selected=0x13926F0B
  DockNode      ID=0x00000003 Parent=0x8B93E3BD SizeRef=1536,427 Split=X
    DockNode    ID=0x00000001 Parent=0x00000003 SizeRef=392,789 Selected=0x9F27EDF6
    DockNode    ID=0x00000002 Parent=0x00000003 SizeRef=1142,789 Split=X Selected=0x13926F0B
      DockNode  ID=0x00000007 Parent=0x00000002 SizeRef=1265,645 CentralNode=1 Selected=0x13926F0B
      DockNode  ID=0x00000008 Parent=0x00000002 SizeRef=259,645 Selected=0xDE39558F
  DockNode      ID=0x00000004 Parent=0x8B93E3BD SizeRef=1536,360 Split=X Selected=0x6AE1E39D
    DockNode    ID=0x00000005 Parent=0x00000004 SizeRef=906,360 Selected=0x6AE1E39D
    DockNode    ID=0x00000006 Parent=0x00000004 SizeRef=751,360 Selected=0x49278EEE

)");
    }
    {
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
        io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.ConfigWindowsResizeFromEdges = true;

        ImFontConfig fontCfg;
        fontCfg.FontDataOwnedByAtlas = false;
        fontCfg.OversampleH = 4;
        fontCfg.OversampleV = 4;
        fontCfg.RasterizerMultiply = 1.5f;
        // io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 14, &fontCfg);
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/arial.ttf", 14, &fontCfg);

        // create font texture for the custom font
        unsigned char* font_pixels;
        int font_width, font_height;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        sg_image_desc img_desc = {};
        img_desc.width = font_width;
        img_desc.height = font_height;
        img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        img_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        img_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        img_desc.min_filter = SG_FILTER_LINEAR;
        img_desc.mag_filter = SG_FILTER_LINEAR;
        img_desc.data.subimage[0][0].ptr = font_pixels;
        img_desc.data.subimage[0][0].size = font_width * font_height * 4;
        io.Fonts->TexID = (ImTextureID)(uintptr_t) sg_make_image(&img_desc).id;
    }
    {
        sg_buffer_desc d = {};

        // @Note: 58254 is >2x all CLD faces in all SH2 rooms combined
        // and yields a 4MB buffer per group.
        size_t CLD_MAX_FACES_PER_GROUP = 58254;
        size_t CLD_MAX_TRIANGLES_PER_GROUP = CLD_MAX_FACES_PER_GROUP * 2;
        size_t CLD_MAX_VERTICES_PER_GROUP = CLD_MAX_TRIANGLES_PER_GROUP * 3;
        size_t CLD_MAX_FLOATS_PER_GROUP = CLD_MAX_VERTICES_PER_GROUP * 3;
        size_t CLD_GROUP_BUFFER_SIZE = CLD_MAX_FLOATS_PER_GROUP * sizeof(float);
        d.usage = SG_USAGE_DYNAMIC;
        d.size = CLD_GROUP_BUFFER_SIZE;
        for (auto &buffer : g.cld_face_buffers) {
            buffer.buf = sg_make_buffer(d);
        }
    }
    {
        sg_buffer_desc d = {};
        float vertices[] = {
            -1, -1, 0,
            +1, -1, 0,
            -1, +1, 0,
            -1, +1, 0,
            +1, -1, 0,
            +1, +1, 0,
        };
        d.data = SG_RANGE(vertices);
        g.highlight_vertex_circle_buffer = sg_make_buffer(d);
    }
    {
        sg_pipeline_desc d = {};
        d.shader = sg_make_shader(highlight_vertex_circle_shader_desc(sg_query_backend()));
        d.layout.attrs[ATTR_highlight_vertex_circle_vs_position].format = SG_VERTEXFORMAT_FLOAT3;
        d.alpha_to_coverage_enabled = true;
        g.highlight_vertex_circle_pipeline = sg_make_pipeline(d);
    }
    {
        sg_buffer_desc d = {};
        size_t MAP_MAX_VERTICES_PER_GEOMETRY = 320000;
        size_t MAP_BUFFER_SIZE = MAP_MAX_VERTICES_PER_GEOMETRY * sizeof(MAP_Geometry_Vertex);
        d.usage = SG_USAGE_DYNAMIC;
        d.size = MAP_BUFFER_SIZE;
        for (auto &buffer : g.map_buffers) {
            d.type = SG_BUFFERTYPE_VERTEXBUFFER;
            buffer.vertex_buffer = sg_make_buffer(d);
            d.type = SG_BUFFERTYPE_INDEXBUFFER;
            buffer.index_buffer = sg_make_buffer(d);
        }
    }
#ifndef NDEBUG
    {
        map_load(g, "map/ob01 (2).map");
        cld_load(g, "../cld/cld/ob01.cld");
        // map_load(g, "map/ap64.map");
        // test_all_maps(g);
        // sapp_request_quit();
    }
#endif

    {
        sg_pipeline_desc d = {};
        d.shader = sg_make_shader(cld_shader_desc(sg_query_backend()));
        d.layout.attrs[ATTR_cld_vs_position].format = SG_VERTEXFORMAT_FLOAT3;
        d.depth.write_enabled = true;
        d.depth.compare = SG_COMPAREFUNC_GREATER;
        g.cld_pipeline = sg_make_pipeline(d);
    }
    {
        sg_pipeline_desc d = {};
        d.shader = sg_make_shader(map_shader_desc(sg_query_backend()));
        d.layout.attrs[ATTR_map_vs_in_position].format = SG_VERTEXFORMAT_FLOAT3;
        d.layout.attrs[ATTR_map_vs_in_normal].format = SG_VERTEXFORMAT_FLOAT3;
        d.layout.attrs[ATTR_map_vs_in_color].format = SG_VERTEXFORMAT_UBYTE4N;
        d.layout.attrs[ATTR_map_vs_in_uv].format = SG_VERTEXFORMAT_FLOAT2;
        d.depth.write_enabled = true;
        d.alpha_to_coverage_enabled = true;
        d.depth.compare = SG_COMPAREFUNC_GREATER;
        d.face_winding = SG_FACEWINDING_CCW;
        // d.primitive_type = SG_PRIMITIVETYPE_POINTS;
        // d.primitive_type = SG_PRIMITIVETYPE_LINES;
        d.index_type = SG_INDEXTYPE_UINT32;

        { // Make the pipelines
            d.wireframe = false;
            d.cull_mode = SG_CULLMODE_BACK;
            g.map_pipeline = sg_make_pipeline(d);
            d.cull_mode = SG_CULLMODE_NONE;
            g.map_pipeline_no_cull = sg_make_pipeline(d);
            d.wireframe = true;
            d.cull_mode = SG_CULLMODE_BACK;
            g.map_pipeline_wireframe = sg_make_pipeline(d);
            d.cull_mode = SG_CULLMODE_NONE;
            g.map_pipeline_no_cull_wireframe = sg_make_pipeline(d);
        }


        d.depth.write_enabled = false;
        d.alpha_to_coverage_enabled = false;
        d.colors[0].blend.enabled = true;
        d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

        { // Make the pipelines
            d.wireframe = false;
            d.cull_mode = SG_CULLMODE_BACK;
            g.decal_pipeline = sg_make_pipeline(d);
            d.cull_mode = SG_CULLMODE_NONE;
            g.decal_pipeline_no_cull = sg_make_pipeline(d);
            d.wireframe = true;
            d.cull_mode = SG_CULLMODE_BACK;
            g.decal_pipeline_wireframe = sg_make_pipeline(d);
            d.cull_mode = SG_CULLMODE_NONE;
            g.decal_pipeline_no_cull_wireframe = sg_make_pipeline(d);
        }

    }

    {
        sg_image_desc d = {};
        enum { N = 1024 };
        enum { M = 32 };
        static uint32_t pixels[N][N] = {};
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                pixels[y][x] = (((x * M / N) ^ (y * M / N)) & 1) ? 0xffff00ff : 0xff000000;
            }
        }
        d.width = N;
        d.height = N;
        d.min_filter = SG_FILTER_LINEAR_MIPMAP_LINEAR;
        d.mag_filter = SG_FILTER_LINEAR_MIPMAP_LINEAR;
        d.max_anisotropy = 16;
        d.data.subimage[0][0] = SG_RANGE(pixels);
        g.missing_texture.tex = sg_make_image(d);
        g.missing_texture.width = N;
        g.missing_texture.height = N;
    }
}

// I should ask the community what they know about the units used in the game
const float SCALE = 0.001f;
const float widget_pixel_radius = 30;

static HMM_Mat4 camera_rot(G &g) {
    // ProfileFunction();

    // We pitch the camera by applying a rotation around X,
    // then yaw the camera by applying a rotation around Y.
    auto pitch_matrix = HMM_Rotate_RH(g.pitch, HMM_V3(1, 0, 0));
    auto yaw_matrix = HMM_Rotate_RH(g.yaw, HMM_V3(0, 1, 0));
    return yaw_matrix * pitch_matrix;
}

static float widget_radius(G &g, HMM_Vec3 offset) {
    // ProfileFunction();

    HMM_Vec4 cam_forward = { 0, 0, -1, 0 };
    HMM_Mat4 cam_rot = camera_rot(g);

    cam_forward = HMM_MulM4V4(cam_rot, cam_forward);

    HMM_Vec3 disp = g.cam_pos - offset;

    // float dist = HMM_Len(disp);
    // float cos_theta = HMM_Dot(disp, cam_forward.XYZ) / dist;

    return HMM_Dot(disp, cam_forward.XYZ) * widget_pixel_radius / sapp_heightf() * tanf(g.fov / 2);
}


static Ray screen_to_ray(G &g, HMM_Vec2 mouse_xy) {
    ProfileFunction();

    Ray ray = {};
    ray.pos = { g.cam_pos.X, g.cam_pos.Y, g.cam_pos.Z };
    HMM_Vec2 ndc = { mouse_xy.X, mouse_xy.Y };
    ndc.X = ((ndc.X - g.view_x) / g.view_w) * 2 - 1;
    ndc.Y = ((ndc.Y - g.view_y) / g.view_h) * -2 + 1;
    //Log("NDC %f, %f", ndc.X, ndc.Y);
    HMM_Vec4 ray_dir4 = { ndc.X, ndc.Y, -1, 0 };
    ray_dir4.XY *= tanf(g.fov / 2);
    ray_dir4.X *= g.view_w / g.view_h;
    ray_dir4 = camera_rot(g) * ray_dir4;
    ray.dir = HMM_Norm(ray_dir4.XYZ);
    return ray;
}

struct Ray_Vs_MAP_Result {
    bool hit = false;

    float closest_t = INFINITY;
    MAP_Mesh *hit_mesh = nullptr;
    int hit_vertex_buffer = -1;
    int hit_face = -1;
    int hit_vertex = -1;
    HMM_Vec3 hit_widget_pos = {};
};
static Ray_Vs_MAP_Result ray_vs_map(G &g, HMM_Vec3 ray_pos, HMM_Vec3 ray_dir) {
    ProfileFunction();

    assert(!g.stale());

    Ray_Vs_MAP_Result result = {};
    HMM_Vec4 cam_forward = { 0, 0, -1, 0 };
    {
        HMM_Mat4 cam_rot = camera_rot(g);
        cam_forward = HMM_MulM4V4(cam_rot, cam_forward);
    }
    float widget_radius_factor = (widget_pixel_radius / sapp_heightf() * tanf(g.fov / 2) / SCALE);
    Ray_Vs_Sphere_Result sphere_raycast = {};
    for (MAP_Geometry_Buffer &buf : g.map_buffers) {
        if (&buf - g.map_buffers >= g.map_buffers_count) {
            break;
        }
        if (!buf.shown) continue;

        assert(buf.mesh_ptr);
        MAP_Mesh &mesh = *buf.mesh_ptr;
        for (int vertex_buffer_index = 0; vertex_buffer_index < mesh.vertex_buffers.count; ++vertex_buffer_index) {
            MAP_Mesh_Vertex_Buffer &vertex_buffer = mesh.vertex_buffers[vertex_buffer_index];

            if (buf.selected) for (int vertex_index = 0; vertex_index < vertex_buffer.num_vertices; ++vertex_index) {
                HMM_Vec3 vertex = *(HMM_Vec3 *)&vertex_buffer.data.data[vertex_index * vertex_buffer.bytes_per_vertex];
                // HMM_Vec3 offset = -g.cld_origin() + vertex;
                // float radius = widget_radius(g, offset) / SCALE;
                HMM_Vec3 disp = { g.cam_pos.X - (vertex.X * SCALE), g.cam_pos.Y + (vertex.Y * SCALE), g.cam_pos.Z + (vertex.Z * SCALE), };
                ray_vs_sphere_(sphere_raycast, ray_pos, ray_dir, vertex, (disp.X * cam_forward.X + disp.Y * cam_forward.Y + disp.Z * cam_forward.Z) * widget_radius_factor);
                if (sphere_raycast.hit) {
                    if (sphere_raycast.t >= 0 && sphere_raycast.t < result.closest_t) {
                        result.hit = true;
                        result.closest_t = sphere_raycast.t;
                        result.hit_mesh = &mesh;
                        result.hit_vertex_buffer = vertex_buffer_index;
                        result.hit_face = -1;
                        result.hit_vertex = vertex_index;
                        result.hit_widget_pos = vertex;
                    }
                }
            }

            Ray_Vs_Plane_Result raycast = {};
            assert(buf.indices.count % 3 == 0);
            for (int face = 0; face < buf.indices.count / 3; ++face) {
                HMM_Vec3 a = *(HMM_Vec3 *)&buf.vertices.data[buf.indices.data[face * 3 + 0]];
                HMM_Vec3 b = *(HMM_Vec3 *)&buf.vertices.data[buf.indices.data[face * 3 + 1]];
                HMM_Vec3 c = *(HMM_Vec3 *)&buf.vertices.data[buf.indices.data[face * 3 + 2]];
                ray_vs_triangle_(raycast, ray_pos, ray_dir, a, b, c);
                if (raycast.hit) {
                    if (raycast.t >= 0 && raycast.t < result.closest_t) {
                        HMM_Vec3 normal = HMM_Cross((c - a), (b - a));
                        if (!g.cull_backfaces || HMM_Dot(normal, ray_dir) > 0) {
                            result.hit = true;
                            result.closest_t = raycast.t;
                            result.hit_mesh = &mesh;
                            result.hit_vertex_buffer = vertex_buffer_index;
                            result.hit_face = face;
                            result.hit_vertex = -1;
                            result.hit_widget_pos = {};
                        }
                    }
                }
            }
        }
    }
    return result;
}

struct Ray_Vs_CLD_Result {
    bool hit = false;

    float closest_t = INFINITY;
    int hit_group = -1;
    int hit_face_index = -1;
    int hit_vertex = -1;
    HMM_Vec3 hit_widget_pos = {};
};
static Ray_Vs_CLD_Result ray_vs_cld(G &g, HMM_Vec3 ray_pos, HMM_Vec3 ray_dir) {
    Ray_Vs_CLD_Result result = {};

    assert(!g.stale());

    for (int group = 0; group < 4; group++) {
        PH2CLD_Face *faces = g.cld.group_0_faces;
        size_t num_faces = g.cld.group_0_faces_count;
        if (group == 1) { faces = g.cld.group_1_faces; num_faces = g.cld.group_1_faces_count; }
        if (group == 2) { faces = g.cld.group_2_faces; num_faces = g.cld.group_2_faces_count; }
        if (group == 3) { faces = g.cld.group_3_faces; num_faces = g.cld.group_3_faces_count; }
        for (int face_index = 0; face_index < num_faces; face_index++) {
            PH2CLD_Face *face = &faces[face_index];
            int vertices_to_raycast = 3;
            if (face->quad) {
                vertices_to_raycast = 4;
            }

            for (int vertex_index = 0; vertex_index < vertices_to_raycast; vertex_index++) {
                float (&vertex_floats)[3] = face->vertices[vertex_index];
                HMM_Vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
                //Log("Minv*Pos = %f, %f, %f, %f", ray_pos.X, ray_pos.Y, ray_pos.Z, ray_pos.W);
                //Log("Minv*Dir = %f, %f, %f, %f", ray_dir.X, ray_dir.Y, ray_dir.Z, ray_dir.W);

                //Log("Vertex Pos = %f, %f, %f", vertex.X, vertex.Y, vertex.Z);

                HMM_Vec3 offset = -g.cld_origin() + vertex;
                offset.X *= SCALE;
                offset.Y *= -SCALE;
                offset.Z *= -SCALE;

                HMM_Vec3 widget_pos = vertex;
                float radius = widget_radius(g, offset) / SCALE;

                auto raycast = ray_vs_sphere(ray_pos, ray_dir, widget_pos, radius);
                if (raycast.hit && raycast.t >= 0) {
                    float bias = 0; // @Ugly
                    if ((g.select_cld_group == group) && (g.select_cld_face == face_index)) {
                        bias = 0.01f;
                    }
                    if (raycast.t < result.closest_t + bias) {
                        result.hit = true;
                        result.closest_t = raycast.t;
                        result.hit_group = group;
                        result.hit_face_index = face_index;
                        result.hit_vertex = vertex_index;
                        result.hit_widget_pos = widget_pos;
                    }
                }
            }
        }
    }
    for (int group = 0; group < 4; group++) {
        PH2CLD_Face *faces = g.cld.group_0_faces;
        size_t num_faces = g.cld.group_0_faces_count;
        if (group == 1) { faces = g.cld.group_1_faces; num_faces = g.cld.group_1_faces_count; }
        if (group == 2) { faces = g.cld.group_2_faces; num_faces = g.cld.group_2_faces_count; }
        if (group == 3) { faces = g.cld.group_3_faces; num_faces = g.cld.group_3_faces_count; }
        for (int face_index = 0; face_index < num_faces; face_index++) {
            PH2CLD_Face *face = &faces[face_index];
            int vertices_to_raycast = 3;
            if (face->quad) {
                vertices_to_raycast = 4;
            }
            {
                HMM_Vec3 a = { face->vertices[0][0], face->vertices[0][1], face->vertices[0][2] };
                HMM_Vec3 b = { face->vertices[1][0], face->vertices[1][1], face->vertices[1][2] };
                HMM_Vec3 c = { face->vertices[2][0], face->vertices[2][1], face->vertices[2][2] };
                auto raycast = ray_vs_triangle(ray_pos, ray_dir, a, b, c);
                if (!raycast.hit) {
                    if (face->quad) {
                        HMM_Vec3 d = { face->vertices[3][0], face->vertices[3][1], face->vertices[3][2] };
                        raycast = ray_vs_triangle(ray_pos, ray_dir, a, c, d);
                    }
                }
                if (raycast.hit && raycast.t >= 0) {
                    if (raycast.t < result.closest_t) {
                        result.hit = true;
                        result.closest_t = raycast.t;
                        result.hit_group = group;
                        result.hit_face_index = face_index;
                        result.hit_vertex = -1;
                        result.hit_widget_pos = {};
                    }
                }
            }
        }
    }
    return result;
}


static Ray_Vs_World_Result ray_vs_world(G &g, HMM_Vec3 ray_pos, HMM_Vec3 ray_dir) {
    Ray_Vs_World_Result result = {};
    {
        Ray_Vs_MAP_Result raycast = ray_vs_map(g, ray_pos, ray_dir);

        if (raycast.hit && raycast.closest_t < result.closest_t) {
            result.hit = true;
            result.closest_t = raycast.closest_t;
            result.hit_widget_pos = raycast.hit_widget_pos;

            result.cld = {};

            result.map.hit_mesh = raycast.hit_mesh;
            result.map.hit_vertex_buffer = raycast.hit_vertex_buffer;
            result.map.hit_face = raycast.hit_face;
            result.map.hit_vertex = raycast.hit_vertex;
        }
    }
    if (g.cld.valid) {
        Ray_Vs_CLD_Result raycast = ray_vs_cld(g, ray_pos, ray_dir);

        if (raycast.hit && raycast.closest_t < result.closest_t) {
            result.hit = true;
            result.closest_t = raycast.closest_t;
            result.hit_widget_pos = raycast.hit_widget_pos;

            result.map = {};

            result.cld.hit_group = raycast.hit_group;
            result.cld.hit_face_index = raycast.hit_face_index;
            result.cld.hit_vertex = raycast.hit_vertex;
        }
    }
    return result;
}

static void event(const sapp_event *e_, void *userdata) {
    ProfileFunction();

    G &g = *(G *)userdata;
    const sapp_event &e = *e_;
    if (e.type == SAPP_EVENTTYPE_QUIT_REQUESTED) {
        // @Todo: check for unsaved changes and prompt to make sure the user wants to discard them.
        // sapp_cancel_quit();
    }
    if (e.type == SAPP_EVENTTYPE_UNFOCUSED) {
        g.control_state = ControlState::Normal;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_UP) {
        if (g.control_state == ControlState::Orbiting) {
            if (e.mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
                g.control_state = ControlState::Normal;
            }
        }
        if (g.control_state == ControlState::Dragging) {
            if (e.mouse_button == SAPP_MOUSEBUTTON_LEFT) {
                g.control_state = ControlState::Normal;
            }
        }
    }
    if (e.type == SAPP_EVENTTYPE_KEY_DOWN) {
        if (!e.key_repeat &&
            ((e.key_code == SAPP_KEYCODE_F11) ||
             (e.key_code == SAPP_KEYCODE_ENTER && (e.modifiers & SAPP_MODIFIER_ALT)))) {
            sapp_toggle_fullscreen();
        }
        if (!e.key_repeat && e.key_code == SAPP_KEYCODE_S && (e.modifiers & SAPP_MODIFIER_CTRL)) {
            g.control_s = true;
            if (e.modifiers & SAPP_MODIFIER_SHIFT) {
                g.control_shift_s = true;
            }
        }
        if (!e.key_repeat && e.key_code == SAPP_KEYCODE_O && (e.modifiers & SAPP_MODIFIER_CTRL)) {
            g.control_o = true;
        }
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            if (e.key_code == SAPP_KEYCODE_Z && (e.modifiers & SAPP_MODIFIER_CTRL)) {
                g.control_z = true;
            }
            if (e.key_code == SAPP_KEYCODE_Y && (e.modifiers & SAPP_MODIFIER_CTRL)) {
                g.control_y = true;
            }
        }
    }
    simgui_handle_event(&e);
    if (g.control_state == ControlState::Normal) {
        bool mouse_in_viewport = (e.mouse_x >= g.view_x && e.mouse_x <= g.view_x + g.view_w &&
                                  e.mouse_y >= g.view_y && e.mouse_y <= g.view_y + g.view_h);
        if (!mouse_in_viewport || ImGui::GetIO().WantCaptureKeyboard) {
            return;
        }
    }
    if (g.control_state == ControlState::Normal && e.type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (e.mouse_button == SAPP_MOUSEBUTTON_LEFT) {
            g.click_ray = screen_to_ray(g, { e.mouse_x, e.mouse_y });
            g.clicked = true;
            g.click_result = {};
        }
        if (e.mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            g.control_state = ControlState::Orbiting;
        }
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        g.mouse_x = e.mouse_x;
        g.mouse_y = e.mouse_y;

        g.mouse_ray = screen_to_ray(g, { e.mouse_x, e.mouse_y });

        if (g.control_state == ControlState::Orbiting) {
            g.yaw += -e.mouse_dx * 3 * (0.022f * (TAU32 / 360));
            g.pitch += -e.mouse_dy * 3 * (0.022f * (TAU32 / 360));
        }
        if (g.control_state == ControlState::Dragging) {
            
            if (g.drag_map_mesh && g.drag_map_buffer >= 0 && g.drag_map_vertex >= 0) {
                HMM_Vec2 prev_mouse_pos = { e.mouse_x - e.mouse_dx, e.mouse_y - e.mouse_dy };
                HMM_Vec2 this_mouse_pos = { e.mouse_x, e.mouse_y };
                
                MAP_Mesh &mesh = *g.drag_map_mesh;
                MAP_Mesh_Vertex_Buffer &buf = mesh.vertex_buffers[g.drag_map_buffer];

                auto &vertex_floats = *(float (*)[3])&buf.data[buf.bytes_per_vertex * g.drag_map_vertex];
                HMM_Vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
                
                HMM_Mat4 Minv = HMM_Scale( { 1 / SCALE, 1 / -SCALE, 1 / -SCALE });

                HMM_Vec3 offset = vertex;
                offset.X *= SCALE;
                offset.Y *= -SCALE;
                offset.Z *= -SCALE;

                HMM_Vec3 widget_pos = vertex;

                {
                    Ray click_ray = g.click_ray;
                    Ray this_ray = screen_to_ray(g, this_mouse_pos);
                    HMM_Vec4 click_ray_pos = { 0, 0, 0, 1 };
                    click_ray_pos.XYZ = click_ray.pos;
                    HMM_Vec4 click_ray_dir = {};
                    click_ray_dir.XYZ = click_ray.dir;
                    HMM_Vec4 this_ray_pos = { 0, 0, 0, 1 };
                    this_ray_pos.XYZ = this_ray.pos;
                    HMM_Vec4 this_ray_dir = {};
                    this_ray_dir.XYZ = this_ray.dir;

                    click_ray_pos = Minv * click_ray_pos;
                    click_ray_dir = HMM_Norm(Minv * click_ray_dir);

                    this_ray_pos = Minv * this_ray_pos;
                    this_ray_dir = HMM_Norm(Minv * this_ray_dir);

                    bool aligned_with_camera = true;
                    if (aligned_with_camera) {
                        // Remember you gotta put the plane in the coordinate space of the map file!
                        HMM_Vec3 plane_normal = ((Minv * camera_rot(g)) * HMM_Vec4 { 0, 0, -1, 0 }).XYZ;
                        auto click_raycast = ray_vs_plane(click_ray_pos.XYZ, click_ray_dir.XYZ, widget_pos, plane_normal);
                        auto raycast = ray_vs_plane(this_ray_pos.XYZ, this_ray_dir.XYZ, widget_pos, plane_normal);

                        assert(click_raycast.hit); // How could it not hit if it's aligned to the camera?
                        assert(raycast.hit);
                        HMM_Vec3 drag_offset = click_raycast.point - g.widget_original_pos;
                        HMM_Vec3 target = raycast.point - drag_offset;
                        if (e.modifiers & SAPP_MODIFIER_ALT) {

                            float distsq_min = INFINITY;

                            // for (auto &geo : g.geometries)
                            {

                                // LinkedList<MAP_Mesh, The_Arena_Allocator> *lists[3] = { &geo.opaque_meshes, &geo.transparent_meshes, &geo.decal_meshes };

                                for (MAP_Geometry_Buffer &buf : g.map_buffers) {
                                    // if (&buf - g.map_buffers >= g.map_buffers_count) {
                                    //     break;
                                    // }
                                    // if (!buf.shown) continue;

                                    if (!buf.mesh_ptr) continue;

                                    auto &other_mesh = *buf.mesh_ptr;

                                    for (int vertex_buffer_index = 0;
                                        vertex_buffer_index < other_mesh.vertex_buffers.count;
                                        ++vertex_buffer_index) {
                                        auto &vbuf = other_mesh.vertex_buffers[vertex_buffer_index];

                                        for (int vertex_index = 0;
                                            vertex_index < vbuf.num_vertices;
                                            ++vertex_index) {
                                            if (&other_mesh == g.drag_map_mesh &&
                                                g.drag_map_buffer == vertex_buffer_index &&
                                                g.drag_map_vertex == vertex_index) {
                                                continue;
                                            }

                                            HMM_Vec3 *vertex = (HMM_Vec3 *)&vbuf.data.data[vbuf.bytes_per_vertex * vertex_index];

                                            float distsq =
                                                (vertex->X - target.X) * (vertex->X - target.X) +
                                                (vertex->Y - target.Y) * (vertex->Y - target.Y) +
                                                (vertex->Z - target.Z) * (vertex->Z - target.Z);
                                            if (distsq < 150 * 150) {
                                                if (distsq_min > distsq) {
                                                    distsq_min = distsq;
                                                    target = *vertex;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        vertex_floats[0] = target.X;
                        vertex_floats[1] = target.Y;
                        vertex_floats[2] = target.Z;
                        g.staleify_map();
                    }
                }
            }

            if (g.cld.valid && g.drag_cld_face >= 0 && g.drag_cld_group >= 0 && g.drag_cld_vertex >= 0) {
                HMM_Vec2 prev_mouse_pos = { e.mouse_x - e.mouse_dx, e.mouse_y - e.mouse_dy };
                HMM_Vec2 this_mouse_pos = { e.mouse_x, e.mouse_y };
                
                PH2CLD_Face *faces = g.cld.group_0_faces;
                if (g.drag_cld_group == 1) { faces = g.cld.group_1_faces; }
                if (g.drag_cld_group == 2) { faces = g.cld.group_2_faces; }
                if (g.drag_cld_group == 3) { faces = g.cld.group_3_faces; }

                PH2CLD_Face *face = &faces[g.drag_cld_face];
                
                // @Temporary: @Deduplicate.
                float (&vertex_floats)[3] = face->vertices[g.drag_cld_vertex];
                HMM_Vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
                HMM_Vec3 origin = -g.cld_origin();

                HMM_Mat4 Tinv = HMM_Translate(-origin);
                HMM_Mat4 Sinv = HMM_Scale( { 1 / SCALE, 1 / -SCALE, 1 / -SCALE });
                HMM_Mat4 Minv = Tinv * Sinv;

                HMM_Vec3 offset = -g.cld_origin() + vertex;
                offset.X *= SCALE;
                offset.Y *= -SCALE;
                offset.Z *= -SCALE;

                HMM_Vec3 widget_pos = vertex;

                {
                    Ray click_ray = g.click_ray;
                    Ray this_ray = screen_to_ray(g, this_mouse_pos);
                    HMM_Vec4 click_ray_pos = { 0, 0, 0, 1 };
                    click_ray_pos.XYZ = click_ray.pos;
                    HMM_Vec4 click_ray_dir = {};
                    click_ray_dir.XYZ = click_ray.dir;
                    HMM_Vec4 this_ray_pos = { 0, 0, 0, 1 };
                    this_ray_pos.XYZ = this_ray.pos;
                    HMM_Vec4 this_ray_dir = {};
                    this_ray_dir.XYZ = this_ray.dir;

                    click_ray_pos = Minv * click_ray_pos;
                    click_ray_dir = HMM_Norm(Minv * click_ray_dir);

                    this_ray_pos = Minv * this_ray_pos;
                    this_ray_dir = HMM_Norm(Minv * this_ray_dir);

                    bool aligned_with_camera = true;
                    if (aligned_with_camera) {
                        // Remember you gotta put the plane in the coordinate space of the cld file!
                        HMM_Vec3 plane_normal = ((Sinv * camera_rot(g)) * HMM_Vec4 { 0, 0, -1, 0 }).XYZ;
                        auto click_raycast = ray_vs_plane(click_ray_pos.XYZ, click_ray_dir.XYZ, widget_pos, plane_normal);
                        auto raycast = ray_vs_plane(this_ray_pos.XYZ, this_ray_dir.XYZ, widget_pos, plane_normal);

                        assert(click_raycast.hit); // How could it not hit if it's aligned to the camera?
                        assert(raycast.hit);
                        HMM_Vec3 drag_offset = click_raycast.point - g.widget_original_pos;
                        HMM_Vec3 target = raycast.point - drag_offset;
                        if (e.modifiers & SAPP_MODIFIER_ALT) {
                            float dist_min = INFINITY;
                            for (int group = 0; group < 4; group++) {
                                PH2CLD_Face *faces = g.cld.group_0_faces;
                                size_t num_faces = g.cld.group_0_faces_count;
                                if (group == 1) { faces = g.cld.group_1_faces; num_faces = g.cld.group_1_faces_count; }
                                if (group == 2) { faces = g.cld.group_2_faces; num_faces = g.cld.group_2_faces_count; }
                                if (group == 3) { faces = g.cld.group_3_faces; num_faces = g.cld.group_3_faces_count; }
                                for (int face_index = 0; face_index < num_faces; face_index++) {
                                    if (group == g.drag_cld_group && face_index == g.drag_cld_face) {
                                        continue;
                                    }

                                    PH2CLD_Face *face = &faces[face_index];
                                    int vertices_to_snap = 3;
                                    if (face->quad) {
                                        vertices_to_snap = 4;
                                    }

                                    for (int vertex_index = 0; vertex_index < vertices_to_snap; vertex_index++) {
                                        float (&vertex_floats)[3] = face->vertices[vertex_index];
                                        HMM_Vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };

                                        HMM_Vec3 disp = vertex - target;
                                        float dist = HMM_Len(disp);
                                        if (dist < 150) {
                                            if (dist_min > dist) {
                                                dist_min = dist;
                                                target = vertex;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        vertex_floats[0] = target.X;
                        vertex_floats[1] = target.Y;
                        vertex_floats[2] = target.Z;
                        g.staleify_cld();
                    } else { // @Temporary: only along XZ plane
                        auto click_raycast = ray_vs_plane(click_ray_pos.XYZ, click_ray_dir.XYZ, widget_pos, { 0, 1, 0 });
                        auto raycast = ray_vs_plane(this_ray_pos.XYZ, this_ray_dir.XYZ, widget_pos, { 0, 1, 0 });
                        if (click_raycast.hit && raycast.hit) {
                            // @Note: Drag-offsetting should use a screenspace offset computed when first clicked,
                            //        rather than a new raycast, so that distant vertices don't have problems and
                            //        so that it always looks like it is going directly to your cursor but modulo
                            //        the little screespace offset between your cursor and the centre of the circle.
                            //        For now, let's just have no offset so that it never has problems.
                            HMM_Vec3 drag_offset = {};
                            // HMM_Vec3 drag_offset = click_raycast.point - g.widget_original_pos;
                            HMM_Vec3 target = raycast.point - drag_offset;
                            target.Y = vertex_floats[1]; // Needs to be assigned for precision reasons
                            if (e.modifiers & SAPP_MODIFIER_ALT) {
                                for (auto &f : target.Elements) {
                                    f = roundf(f / 100) * 100;
                                }
                            }
                            vertex_floats[0] = target.X;
                            vertex_floats[2] = target.Z;
                            g.staleify_cld();
                        }
                    }
                }
            }
        }
    }
    if (g.pitch > TAU32 / 4) {
        g.pitch = TAU32 / 4;
    }
    if (g.pitch < -TAU32 / 4) {
        g.pitch = -TAU32 / 4;
    }
    if (g.control_state == ControlState::Orbiting && e.type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        HMM_Vec4 translate = { 0, 0, -e.scroll_y * 0.1f, 0 };
        g.scroll_speed_timer = 0.5f;
        g.scroll_speed += 0.5f;
        if (g.scroll_speed > MOVE_SPEED_MAX - 4) {
            g.scroll_speed = MOVE_SPEED_MAX - 4;
        }
        translate *= powf(2.0f, g.scroll_speed);
        translate = camera_rot(g) * translate;
        g.cam_pos += translate.XYZ;
    }
}

bool is_mesh_part_group_bogged(G &g, MAP_Geometry_Buffer_Source source, MAP_Mesh &mesh, MAP_Mesh_Part_Group &mesh_part_group, int mesh_index, int mpg_index, char (*reason)[8192], int *reason_n) {
    auto &section = mesh.vertex_buffers[mesh_part_group.section_index];
    MAP_Material *material = g.materials.at_index(mesh_part_group.material_index);
    if (!material) {
        return false;
    }

    bool bogged =
        (section.bytes_per_vertex == 0x14 && (material->mode != 0)) ||
        (section.bytes_per_vertex == 0x18 && (material->mode != 3)) ||
        (section.bytes_per_vertex == 0x20 && (material->mode != 1 && material->mode != 2 && material->mode != 6)) ||
        (section.bytes_per_vertex == 0x24 && (material->mode != 4));
    if (!bogged) {
        return false;
    }
    if (*reason_n >= sizeof(*reason) - 1) {
        return false;
    }

    const char *source_str = nullptr;
    const char *vertex_format = nullptr;
    const char *modes = nullptr;
    if (source == MAP_Geometry_Buffer_Source::Opaque) source_str = "Opaque";
    if (source == MAP_Geometry_Buffer_Source::Transparent) source_str = "Transparent";
    if (source == MAP_Geometry_Buffer_Source::Decal) source_str = "Decal";
    if (section.bytes_per_vertex == 0x14) { vertex_format = "XU";   modes = "0"; }
    if (section.bytes_per_vertex == 0x18) { vertex_format = "XCU";  modes = "3"; }
    if (section.bytes_per_vertex == 0x20) { vertex_format = "XNU";  modes = "1, 2, or 6"; }
    if (section.bytes_per_vertex == 0x24) { vertex_format = "XNCU"; modes = "4"; }

    *reason_n += snprintf(*reason + *reason_n, sizeof(*reason) - *reason_n,
                          "%s #%d group %d: %s vertices use mode %s. Material %d has mode %d.\n",
                          source_str, mesh_index, mpg_index,
                          vertex_format, modes,
                          (int)mesh_part_group.material_index, (int)material->mode);
    (*reason)[*reason_n] = '\0';
    return true;
}

bool is_mesh_bogged(G &g, MAP_Geometry_Buffer_Source source, MAP_Mesh &mesh, int mesh_index, char (*reason)[8192], int *reason_n) {
    bool result = false;
    int mpg_index = 0;
    for (MAP_Mesh_Part_Group &mpg : mesh.mesh_part_groups) {
        result |= is_mesh_part_group_bogged(g, source, mesh, mpg, mesh_index, mpg_index, reason, reason_n);
        ++mpg_index;
    }
    return result;
}

bool is_geo_bogged(G &g, MAP_Geometry &geo, char (*reason)[8192], int *reason_n) {
    bool result = false;
    int mesh_index = 0;
    for (MAP_Mesh &mesh : geo.opaque_meshes) {
        result |= is_mesh_bogged(g, MAP_Geometry_Buffer_Source::Opaque, mesh, mesh_index, reason, reason_n);
        ++mesh_index;
    }
    mesh_index = 0;
    for (MAP_Mesh &mesh : geo.transparent_meshes) {
        result |= is_mesh_bogged(g, MAP_Geometry_Buffer_Source::Transparent, mesh, mesh_index, reason, reason_n);
        ++mesh_index;
    }
    mesh_index = 0;
    for (MAP_Mesh &mesh : geo.decal_meshes) {
        result |= is_mesh_bogged(g, MAP_Geometry_Buffer_Source::Decal, mesh, mesh_index, reason, reason_n);
        ++mesh_index;
    }
    return result;
}

ImVec4 bogged_flash_colour(G &g) {
    ImVec4 red = { (0xe0 / 255.0f), (0x3b / 255.0f), (0x0d / 255.0f), 1 };
    ImVec4 white = { 1, 1, 1, 1 };
    float lerp_t = sinf((float)g.t * TAU32) * 0.5f + 0.5f;
    ImVec4 col = ImLerp(red, white, lerp_t);
    return col;
}

struct my_dds_header {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t mip_map_count;
    uint32_t reserved1[11];
    uint32_t pixelformat_size;
    uint32_t pixelformat_flags;
    uint32_t pixelformat_four_cc;
    uint32_t pixelformat_rgb_bit_count;
    uint32_t pixelformat_r_bit_mask;
    uint32_t pixelformat_g_bit_mask;
    uint32_t pixelformat_b_bit_mask;
    uint32_t pixelformat_a_bit_mask;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
char *win_import_or_export_dialog(LPCWSTR formats, LPCWSTR title, bool import = true) {
    ProfileFunction();

    char *result = nullptr;
    assert(formats);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)sapp_win32_get_hwnd();
    ofn.lpstrFilter = formats;
    ofn.nFilterIndex = 1;
    uint16_t buf[65536] = {};
    ofn.lpstrFile = (LPWSTR)buf;
    ofn.nMaxFile = (DWORD)countof(buf) - 1;
    ofn.lpstrTitle = title;
    ofn.Flags |= OFN_NOCHANGEDIR;
    ofn.Flags |= OFN_DONTADDTORECENT;
    ofn.Flags |= OFN_FILEMUSTEXIST;
    if (!import) {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
    }
    if (import ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn)) {
        result = utf16_to_utf8(buf);
        if (result) {
            // Success!
        } else {
            MsgErr("OBJ Load Error", "Error generating filename!!\n\nSorry.");
        }
    } else {
        auto commdlg_err = CommDlgExtendedError();
        if (commdlg_err == 0) {
            // User closed/cancelled. No biggie! :D
        } else {
            MsgErr("OBJ Load Error", "Couldn't open dialog box!!\n\nSorry.");
        }
    }
    return result;
}
#define FailIfFalse(e, s, ...) do { \
        if (!(e)) { \
            MsgErr("DDS Load Error", "DDS import error:\n\n" s "\n\nSorry!", ##__VA_ARGS__); \
            return false; \
        } \
    } while (0)

bool dds_import(char *filename, MAP_Texture &result) {
    ProfileFunction();

    FILE *f = PH2CLD__fopen(filename, "rb");
    FailIfFalse(f, "File \"%s\" couldn't be opened.", filename);
    defer {
        if (f) {
            fclose(f);
        }
    };

    char magic[4];
    FailIfFalse(fread(magic, 4, 1, f) == 1, "File read error. (4 byte magic number read failed)");
    FailIfFalse(strncmp(magic, "DDS\x20", 4) == 0, "File doesn't seem to be a DDS file. (Magic bytes \"DDS\\x20\" not present)");
    my_dds_header header = {};
    static_assert(sizeof(header) == 128 - 4, "");
    FailIfFalse(fread(&header, sizeof(header), 1, f) == 1, "File read error. (Couldn't read header)");
    FailIfFalse(header.size == sizeof(header), "DDS file seems corrupt or unsupported by this editor. (Header's size field was wrong - should be 124, was %d)", header.size);

    // DDSD_CAPS Required in every .dds file. 0x1
    // DDSD_HEIGHT Required in every .dds file. 0x2
    // DDSD_WIDTH Required in every .dds file. 0x4
    // DDSD_PITCH Required when pitch is provided for an uncompressed texture. 0x8
    // DDSD_PIXELFORMAT Required in every .dds file. 0x1000
    // DDSD_MIPMAPCOUNT Required in a mipmapped texture. 0x20000
    // DDSD_LINEARSIZE Required when pitch is provided for a compressed texture. 0x80000
    // DDSD_DEPTH Required in a depth texture. 0x800000
    FailIfFalse(!!(header.flags & DDSD_CAPS), "DDS file seems corrupt. (Header flags doesn't have DDSD_CAPS set)");
    FailIfFalse(!!(header.flags & DDSD_HEIGHT), "DDS file seems corrupt. (Header flags doesn't have DDSD_HEIGHT set)");
    FailIfFalse(!!(header.flags & DDSD_WIDTH), "DDS file seems corrupt. (Header flags doesn't have DDSD_WIDTH set)");
    FailIfFalse( !(header.flags & DDSD_PITCH), "DDS file seems like an uncompressed format --\nthat doesn't work for SH2!\n(Header flags has DDSD_PITCH set)");
    FailIfFalse(!!(header.flags & DDSD_PIXELFORMAT), "DDS file seems corrupt. (Header flags doesn't have DDSD_PIXELFORMAT set)");
    FailIfFalse( !(header.flags & DDSD_MIPMAPCOUNT) || header.mip_map_count <= 1, "DDS file has mipmaps -- that doesn't work for SH2!\n(Header flags has DDSD_MIPMAPCOUNT set and the count is > 1)");
    FailIfFalse(!!(header.flags & DDSD_LINEARSIZE), "DDS file doesn't have compressed length --\nthis must be known to import!\n(Header flags doesn't have DDSD_LINEARSIZE set)");
    FailIfFalse( !(header.flags & DDSD_DEPTH), "DDS file apparently has multiple depth layers --\nthat's doesn't work for SH2!\n(Header flags has DDSD_DEPTH set)");

    FailIfFalse(header.pixelformat_size == 32, "DDS pixel format is broken/corrupt. (Pixel format size field isn't set to 32)");

    FailIfFalse(header.pixelformat_four_cc == FOURCC_DXT1 ||
        header.pixelformat_four_cc == FOURCC_DXT2 ||
        header.pixelformat_four_cc == FOURCC_DXT3 ||
        header.pixelformat_four_cc == FOURCC_DXT4 ||
        header.pixelformat_four_cc == FOURCC_DXT5,
        "DDS file has a format that isn't\n"
        "DXT1/DXT2/DXT3/DXT4/DXT5 (BC1/BC2/BC3) --\n"
        "that doesn't work for SH2!\n"
        "(FourCC == \"%c%c%c%c\")",
        ((header.pixelformat_four_cc >>  0) & 255),
        ((header.pixelformat_four_cc >>  8) & 255),
        ((header.pixelformat_four_cc >> 16) & 255),
        ((header.pixelformat_four_cc >> 24) & 255));

    FailIfFalse((header.width & (header.width - 1)) == 0, "DDS texture must be a power of 2 in width. (Width is %d)", header.width);
    FailIfFalse((header.height & (header.height - 1)) == 0, "DDS texture must be a power of 2 in height. (Height is %d)", header.height);
    FailIfFalse(header.width <= 4096, "DDS texture must be less than 4096x4096. Width is too big! (Texture is %dx%d)", header.width, header.height);
    FailIfFalse(header.height <= 4096, "DDS texture must be less than 4096x4096. Height is too big! (Texture is %dx%d)", header.width, header.height);

    FailIfFalse(header.pitch_or_linear_size > 0, "DDS texture looks to be empty! (Compressed texture size is %d)", header.pitch_or_linear_size);
    FailIfFalse(header.pitch_or_linear_size <= 4096 * 4096 * 4 / 4, "DDS texture can't be too big! (Compressed texture size is %d)", header.pitch_or_linear_size); // rough estimate, it's whatever.

    MAP_Texture tex = {};
    defer { // @Errdefer
        tex.release();
    };
    tex.id = 0xffff;
    tex.width = (uint16_t)header.width;
    tex.height = (uint16_t)header.height;
    tex.material = 1;
    tex.sprite_count = 1;
    tex.sprite_metadata[0].id = 1;
    switch (header.pixelformat_four_cc) {
        case FOURCC_DXT1: { tex.format = MAP_Texture_Format_BC1; } break;
        case FOURCC_DXT2: { tex.format = MAP_Texture_Format_BC2; } break;
        case FOURCC_DXT3: { tex.format = MAP_Texture_Format_BC2; } break;
        case FOURCC_DXT4: { tex.format = MAP_Texture_Format_BC3; } break;
        case FOURCC_DXT5: { tex.format = MAP_Texture_Format_BC3; } break;
        default: { assert(false); } break;
    }
    // @Note: *PRETTY* sure this is fine??
    switch (tex.format) {
        case MAP_Texture_Format_BC1:       { tex.sprite_metadata[0].format = 0x100; } break;
        case MAP_Texture_Format_BC2:       { tex.sprite_metadata[0].format = 0x102; } break;
        case MAP_Texture_Format_BC3:       { tex.sprite_metadata[0].format = 0x103; } break;
        case MAP_Texture_Format_BC3_Maybe: { tex.sprite_metadata[0].format = 0x104; } break;
        default: { assert(false); } break;
    }

    tex.blob.resize(header.pitch_or_linear_size);
    FailIfFalse(tex.blob.data, "Texture data couldn't be allocated in memory.");
    FailIfFalse(fread(tex.blob.data, tex.blob.count, 1, f) == 1, "File read error reading the texture data. File too small?");

    result = tex;
    tex = {};

    return true;
}

bool export_dds(MAP_Texture tex, char *filename) {
    ProfileFunction();

    FILE *f = PH2CLD__fopen(filename, "wb");
    if (!f) {
        MsgErr("DDS Export Error", "Couldn't open file \"%s\"!!", filename);
        return false;
    }
    defer {
        fclose(f);
    };

    assert(fwrite("DDS\x20", 4, 1, f) == 1);
    my_dds_header header = {};
    header.size = 0x7c;
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
    header.height = tex.height;
    header.width = tex.width;
    header.pitch_or_linear_size = (uint32_t)tex.blob.count;
    header.pixelformat_size = 32;
    header.pixelformat_flags = 4;
    // @Note: *PRETTY* sure this is fine??
    switch (tex.format) {
        case MAP_Texture_Format_BC1: { header.pixelformat_four_cc = FOURCC_DXT1; } break;
        case MAP_Texture_Format_BC2: { header.pixelformat_four_cc = FOURCC_DXT3; } break;
        case MAP_Texture_Format_BC3: { header.pixelformat_four_cc = FOURCC_DXT5; } break;
        default: { assert(false); } break;
    }
    header.caps = DDSCAPS_TEXTURE;
    assert(fwrite(&header, sizeof(header), 1, f) == 1);
    assert(fwrite(tex.blob.data, tex.blob.count, 1, f) == 1);

    return true;
}

#if _MSC_VER && !defined(__clang__)
#define NO_SANITIZE __declspec(no_sanitize_address)
#else
#define NO_SANITIZE __attribute__((no_sanitize_address))
#endif

static void NO_SANITIZE no_asan_memcpy(void *destination, void *source, size_t count) {
    ProfileFunction();

    for (size_t i = 0; i < count; i++) {
        ((char *)destination)[i] = ((char *)source)[i];
    }
}

static HMM_Vec4 PH2MAP_u32_to_bgra(uint32_t u) {
    // ProfileFunction();

    HMM_Vec4 bgra = {
        ((u >> 0) & 0xff) * (1.0f / 255),
        ((u >> 8) & 0xff) * (1.0f / 255),
        ((u >> 16) & 0xff) * (1.0f / 255),
        ((u >> 24) & 0xff) * (1.0f / 255),
    };
    return bgra;
}
static uint32_t PH2MAP_bgra_to_u32(HMM_Vec4 bgra) {
    ProfileFunction();

    uint32_t u = (uint32_t)(clamp(bgra.X, 0.0f, 1.0f) * 255) << 0 |
                 (uint32_t)(clamp(bgra.Y, 0.0f, 1.0f) * 255) << 8 |
                 (uint32_t)(clamp(bgra.Z, 0.0f, 1.0f) * 255) << 16 |
                 (uint32_t)(clamp(bgra.W, 0.0f, 1.0f) * 255) << 24;
    return u;
}

struct MAP_OBJ_Import_Material {
    uint16_t index_in_materials_array = 0;
    uint8_t mode = 0;
    uint32_t diffuse_color = 0;
    uint32_t specular_color = 0;
    union {
        uint32_t specularity_u32 = 0;
        float specularity_f32;
    };
    uint16_t texture_id = 0;
    bool texture_was_found = false;
    uint8_t texture_unknown = 0;

    char dds_filename_buf[1024] = {};

    Array<PH2MAP__Vertex24> unstripped_verts = {};

    void release() {
        ProfileFunction();

        unstripped_verts.release();
    }
};

static bool map_obj_import_materials_equivalent(const MAP_OBJ_Import_Material &a, const MAP_OBJ_Import_Material &b) {
    if (a.index_in_materials_array != b.index_in_materials_array) return false;
    if (a.mode != b.mode) return false;
    if (a.diffuse_color != b.diffuse_color) return false;
    if (a.specular_color != b.specular_color) return false;
    if (a.specularity_u32 != b.specularity_u32) return false;
    if (a.texture_id != b.texture_id) return false;
    if (a.texture_was_found != b.texture_was_found) return false;
    if (a.texture_unknown != b.texture_unknown) return false;

    return true;
}

static bool parse_material_name(char *s, MAP_OBJ_Import_Material &result) {
    ProfileFunction();

    result = {};

    char *start = strstr(s, "PH2_");
    char *end = strstr(s, "_PH2");

    if (!start) {
        return false;
    }

    if (!end) {
        return false;
    }

    if (end - start != 47) {
        return false;
    }

    bool underscores_good = true;
    for (int i = 0; i < 47; ++i) {
        const char *p = "PH2_0000_0_00000000_00000000_00000000_0000_0_00_PH2" + i;
        if (*p == '_') {
            if (start[i] != '_') {
                underscores_good = false;
            }
        }
    }
    if (!underscores_good) {
        return false;
    }

    unsigned int mat_index = 0;
    unsigned int mat_mode = 0;
    unsigned int mat_diffuse_color = 0;
    unsigned int mat_specular_color = 0;
    unsigned int mat_specularity = 0;

    unsigned int tex_id = 0;
    unsigned int tex_found = 0;
    unsigned int tex_unknown = 0;

    int matches = sscanf(start,
                         "PH2_%x_%x_%x_%x_%x_%x_%x_%x_PH2",
                         &mat_index,
                         &mat_mode,
                         &mat_diffuse_color,
                         &mat_specular_color,
                         &mat_specularity,
                         &tex_id,
                         &tex_found,
                         &tex_unknown);
    if (matches != 8) {
        return false;
    }

    result.index_in_materials_array = (uint16_t)mat_index;
    result.mode = (uint8_t)mat_mode;
    result.diffuse_color = mat_diffuse_color;
    result.specular_color = mat_specular_color;
    result.specularity_u32 = mat_specularity;
    result.texture_id = (uint16_t)tex_id;
    result.texture_was_found = tex_found;
    result.texture_unknown = (uint8_t)tex_unknown;

    return true;
}

static void parse_mtl(FILE *mtl, Array<MAP_OBJ_Import_Material> &materials) {
    ProfileFunction();

    char b[1024] = {};
    while (fgets(b, sizeof(b) - 1, mtl)) {
        if (char *lf = strrchr(b, '\n')) *lf = 0;

        {
            char *end = b + strlen(b) - 1;
            while (end >= b && isspace(end[0])) {
                --end;
            }
            end[1] = '\0';
        }

        char *s = b;
        while (isspace(s[0])) {
            ++s;
        }

        if (materials.count > 0) {
            if (memcmp(s, "map_Kd ", 7) == 0) {
                s += 7;
                while (isspace(s[0])) {
                    ++s;
                }

                auto &arr = materials[materials.count - 1].dds_filename_buf;
                strncpy(arr, s, sizeof(arr));

                continue;
            }
        }

        if (memcmp(s, "newmtl ", 7) != 0) {
            continue;
        }

        MAP_OBJ_Import_Material mat = {};
        if (!parse_material_name(s, mat)) {
            continue;
        }

        materials.push(mat);
    }
}

static void viewport_callback(const ImDrawList* dl, const ImDrawCmd* cmd);
static void frame(void *userdata) {
    ProfileFunction();

#ifdef _WIN32
    if (in_assert) return;
#endif

    G &g = *(G *)userdata;
    float dt = 0;
    defer {
        g.t += dt;
    };
    {
        auto next = get_time();
        dt = (float)(next - g.last_time);
        g.last_time = next;
        g.dt_history[sapp_frame_count() % countof(g.dt_history)] = dt;
    }
    simgui_new_frame({ sapp_width(), sapp_height(), dt, sapp_dpi_scale() });
    char *obj_file_buf = nullptr;
    defer {
        free(obj_file_buf);
    };

    // static unsigned long long asdfasdf; if (!asdfasdf++) obj_file_buf = strdup("map/ap64.map.obj");

    char *dds_file_buf = nullptr;
    defer {
        free(dds_file_buf);
    };
    char *requested_save_filename = nullptr;
    defer {
        free(requested_save_filename);
    };
    char *obj_export_name = nullptr;
    defer {
        free(obj_export_name);
    };

    if (ImGui::BeginMainMenuBar()) {
        defer { ImGui::EndMainMenuBar(); };
        if (ImGui::BeginMenu("File")) {
            defer { ImGui::EndMenu(); };
            if (ImGui::MenuItem("Open...", "Ctrl-O")) {
                g.control_o = true;
            }
            if (ImGui::MenuItem("Save MAP", "Ctrl-S")) {
                g.control_s = true;
            }
            if (ImGui::MenuItem("Save MAP As...", "Ctrl-Shift-S")) {
                g.control_s = false;
                g.control_shift_s = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import OBJ Model...")) {
                obj_file_buf = win_import_or_export_dialog(L"Wavefront OBJ\0" "*.obj\0"
                                                            "All Files\0" "*.*\0",
                                                           L"Open OBJ", true);
            }
            if (ImGui::MenuItem("Import DDS Texture...")) {
                dds_file_buf = win_import_or_export_dialog(L"DDS Texture File\0" "*.dds\0"
                                                            "All Files\0" "*.*\0",
                                                           L"Open DDS", true);
            }
            bool any_selected = false;
            for (auto &buf : g.map_buffers) {
                if (&buf - g.map_buffers >= g.map_buffers_count) {
                    break;
                }
                if (buf.selected) {
                    any_selected = true;
                }
            }
            if (ImGui::MenuItem("Export Selected as OBJ...", nullptr, nullptr, any_selected)) {
                obj_export_name = win_import_or_export_dialog(L"Wavefront OBJ\0" "*.obj\0"
                                                               "All Files\0" "*.*\0",
                                                              L"Save OBJ", false);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                sapp_request_quit();
            }
        }
        if (ImGui::BeginMenu("Edit")) {
            defer { ImGui::EndMenu(); };
            if (ImGui::MenuItem("Undo", "Ctrl-Z")) {
                g.control_z = true;
            }
            if (ImGui::MenuItem("Redo", "Ctrl-Y")) {
                g.control_y = true;
            }
        }
        if (ImGui::BeginMenu("View")) {
            defer { ImGui::EndMenu(); };
            ImGui::MenuItem("Editor", nullptr, &g.show_editor);
            ImGui::MenuItem("Viewport", nullptr, &g.show_viewport);
            ImGui::MenuItem("Edit Widget", nullptr, &g.show_edit_widget);
            ImGui::MenuItem("Textures", nullptr, &g.show_textures);
            ImGui::MenuItem("Materials", nullptr, &g.show_materials);
            ImGui::MenuItem("Console", nullptr, &g.show_console);
        }
        if (ImGui::BeginMenu("About")) {
            defer { ImGui::EndMenu(); };
            ImGui::MenuItem("Psilent pHill 2 Editor v0.001", nullptr, false, false);
            ImGui::Separator();
#define URL "https://github.com/pmttavara/ph2"
            if (ImGui::MenuItem(URL)) {
                // Okay, we'll be nice people and ask for confirmation.
                if (MessageBoxA(0,
                    "This will open your browser to " URL ". Go?",
                    "Open Site",
                    MB_YESNO | MB_ICONINFORMATION | MB_TASKMODAL) == IDYES) {
                    ShellExecuteW(0, L"open", L"" URL, 0, 0, SW_SHOW);
                }
            }
        }
        double frametime = 0.0f;
        uint64_t last_frame = sapp_frame_count();
        uint64_t first_frame = max((int64_t)(sapp_frame_count() - countof(g.dt_history) + 1), 0);
        for (auto i = first_frame; i <= last_frame; i++) {
            frametime += g.dt_history[sapp_frame_count() % countof(g.dt_history)];
        }
        ImGui::SameLine(sapp_widthf() / sapp_dpi_scale() - 60.0f);
        ImGui::Text("%.0f FPS", (last_frame - first_frame) / frametime);
        ImGui::SameLine(sapp_widthf() / sapp_dpi_scale() - 250.0f);
        ImGui::Text("%.2f MB head, %.2f MB used", The_Arena_Allocator::arena_head / (1024.0 * 1024.0), The_Arena_Allocator::bytes_used / (1024.0 * 1024.0));
    }
    if (g.control_state != ControlState::Normal); // drop CTRL-S etc when orbiting/dragging
    else if (g.control_o) {
        char *load = win_import_or_export_dialog(L"Silent Hill 2 Files (*.map; *.cld)\0" "*.map;*.cld;*.map.bak\0",
                                                 L"Open", true);
        defer {
            free(load);
        };
        if (load) {
            size_t n = strlen(load);
            char *slash = max(strrchr(load, '/'), strrchr(load, '\\'));
            char *dot = strrchr(load, '.');
            if (dot > slash) {
                if (strcmp(dot, ".cld") == 0) {
                    cld_load(g, load);
                } else if (strcmp(dot, ".map") == 0) {
                    map_load(g, load);
                } else {
                    size_t mapbaklen = (sizeof(".map.bak") - 1);
                    if (n >= mapbaklen && strcmp(load + n - mapbaklen, ".map.bak") == 0) {
                        map_load(g, load);
                    } else {
                        MsgErr("File Load Error", "The file \"%s\"\ndoesn't have the file extension .CLD or .MAP.\nThe editor can only open files ending in .CLD or .MAP. Sorry!!", load);
                    }
                }
            }
        }
    } else if (g.control_shift_s) {
        requested_save_filename = win_import_or_export_dialog(L"Silent Hill 2 MAP File\0" "*.map\0"
                                                               "All Files\0" "*.*\0",
                                                              L"Save MAP", false);
    } else if (g.control_s) {
        requested_save_filename = strdup(g.opened_map_filename);
    }
    // God, this is dumb. Lol. But it works!!!
    g.control_s = false;
    g.control_shift_s = false;
    g.control_o = false;
    ImGui::DockSpaceOverViewport(nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    imgui_do_console(g);
    sapp_lock_mouse(g.control_state == ControlState::Orbiting);
    // sapp_show_mouse(g.control_state != ControlState::Dragging);
    g.scroll_speed_timer -= dt;
    if (g.scroll_speed_timer < 0) {
        g.scroll_speed_timer = 0;
        g.scroll_speed = 0;
    }
    if (g.control_state == ControlState::Orbiting) {
        float rightwards = ((float)KEY('D') - (float)KEY('A')) * dt;
        float forwards   = ((float)KEY('S') - (float)KEY('W')) * dt;
        HMM_Vec4 translate = {rightwards, 0, forwards, 0};
        if (HMM_Len(translate) == 0) {
            g.move_speed = MOVE_SPEED_DEFAULT;
        } else {
            g.move_speed += dt;
        }
        float move_speed = g.move_speed;
        if (KEY(VK_SHIFT)) {
            move_speed += 4;
        }
        if (move_speed > MOVE_SPEED_MAX) {
            move_speed = MOVE_SPEED_MAX;
        }
        translate *= powf(2.0f, move_speed);
        translate = camera_rot(g) * translate;
        g.cam_pos += translate.XYZ;
    }
    ImGui::GetStyle().Alpha = 1;
    // if (g.control_state != ControlState::Normal) {
    //     ImGui::GetStyle().Alpha = 0.333f;
    // }
    auto get_meshes = [&] (MAP_Geometry_Buffer &buf) -> LinkedList<MAP_Mesh, The_Arena_Allocator> & {
        switch (buf.source) {
            default: assert(false);
            case MAP_Geometry_Buffer_Source::Opaque: return buf.geometry_ptr->opaque_meshes;
            case MAP_Geometry_Buffer_Source::Transparent: return buf.geometry_ptr->transparent_meshes;
            case MAP_Geometry_Buffer_Source::Decal: return buf.geometry_ptr->decal_meshes;
        }
    };
        if (obj_file_buf) {
            FILE *f = PH2CLD__fopen(obj_file_buf, "r");
            if (f) {
                defer {
                    fclose(f);
                };

                Array<HMM_Vec3> obj_positions = {}; defer { obj_positions.release(); };
                Array<HMM_Vec2> obj_uvs = {}; defer { obj_uvs.release(); };
                Array<HMM_Vec3> obj_normals = {}; defer { obj_normals.release(); };
                Array<uint32_t> obj_colours = {}; defer { obj_colours.release(); };

                bool should_import_colours = true;

                Array<PH2MAP__Vertex24> materialless_unstripped_verts = {}; defer { materialless_unstripped_verts.release(); };
                Array<MAP_OBJ_Import_Material> import_materials = {};
                defer {
                    for (MAP_OBJ_Import_Material &mat : import_materials) {
                        mat.unstripped_verts.release();
                    }
                };

                Array<PH2MAP__Vertex24> *current_verts = &materialless_unstripped_verts;

                char b[1024] = {};
                while (fgets(b, sizeof(b) - 1, f)) {
                    if (char *lf = strrchr(b, '\n')) *lf = 0;

                    const char *s = b;

                    if (memcmp(s, "#MRGB ", 6) == 0) {
                        s += 6;

                        auto accum_hex_digit = [] (const char **s, unsigned int *digit) -> bool {
                            int c = (*s)[0];
                            if (c >= 'A' && c <= 'F') {
                                *digit |= c - 'A' + 10;
                            } else if (c >= 'a' && c <= 'f') {
                                *digit |= c - 'a' + 10;
                            } else if (c >= '0' && c <= '9') {
                                *digit |= c - '0';
                            } else {
                                return false;
                            }
                            ++*s;

                            return true;
                        };

                        for (;;) {
                            unsigned int argb = 0;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break; argb <<= 4;
                            if (!accum_hex_digit(&s, &argb)) break;

                            obj_colours.push(argb);
                        }

                    }
                }

                rewind(f);

                HMM_Vec3 center = {};
                while (fgets(b, sizeof(b) - 1, f)) {
                    if (char *lf = strrchr(b, '\n')) *lf = 0;
                    char directive[3] = {};

                    char *s = b;
                    while (isspace(s[0])) {
                        ++s;
                    }

                    if (memcmp(s, "mtllib ", 7) == 0) {
                        s += 7;

                        while (isspace(s[0])) {
                            ++s;
                        }

                        FILE *mtl = PH2CLD__fopen(s, "r");
                        if (!mtl) {
                            // maybe it was a relative path; search the .OBJ's folder.
                            auto obj_file_buf_n = strlen(obj_file_buf);
                            auto s_n = strlen(s);
                            char *absolute = (char *)calloc(obj_file_buf_n + s_n + 1, 1);
                            memcpy(absolute, obj_file_buf, obj_file_buf_n + 1);
                            char *slash = max(strrchr(absolute, '/'), strrchr(absolute, '\\'));
                            if (!slash) {
                                continue;
                            }

                            memcpy(slash + 1, s, s_n + 1);
                            mtl = PH2CLD__fopen(absolute, "r");
                            if (!mtl) {
                                continue;
                            }
                        }

                        defer {
                            fclose(mtl);
                        };

                        parse_mtl(mtl, import_materials);

                        continue;

                    }

                    if (memcmp(s, "usemtl ", 7) == 0) {
                        s += 7;

                        while (isspace(s[0])) {
                            ++s;
                        }

                        MAP_OBJ_Import_Material mat = {};
                        if (!parse_material_name(s, mat)) {
                            continue;
                        }

                        for (MAP_OBJ_Import_Material &import_material : import_materials) {
                            if (map_obj_import_materials_equivalent(mat, import_material)) {
                                current_verts = &import_material.unstripped_verts;
                                break;
                            }
                        }

                        continue;

                    }

                    char args[6][64] = {};
                    int matches = sscanf(b, " %s %s %s %s %s %s %s ", directive, args[0], args[1], args[2], args[3], args[4], args[5]);
                    if (strcmp("v", directive) == 0) {
                        // Position
                        assert(matches == 4 || matches == 7);
                        // Log("Position: (%s, %s, %s)", args[0], args[1], args[2]);
                        auto &pos = *obj_positions.push();
                        pos.X = (float)atof(args[0]);
                        pos.Y = (float)atof(args[1]);
                        pos.Z = (float)atof(args[2]);
                        HMM_Vec4 colour = {1, 1, 1, 1};
                        if (matches == 7) {
                            colour.Z = (float)atof(args[3]);
                            colour.Y = (float)atof(args[4]);
                            colour.X = (float)atof(args[5]);
                        }
                        auto colour_u32 = PH2MAP_bgra_to_u32(colour);
                        if (obj_positions.count > obj_colours.count) {
                            assert(obj_positions.count == obj_colours.count + 1);
                            obj_colours.push(colour_u32);
                        } else {
                            assert(obj_positions.count > 0);
                            assert(obj_positions.count <= obj_colours.count);
                            if (matches == 7) {
                                obj_colours[obj_positions.count - 1] = colour_u32;
                            }
                        }
                        center += pos;
                    } else if (strcmp("vt", directive) == 0) {
                        // UV
                        assert(matches == 3);
                        // Log("UV: (%s, %s)", args[0], args[1]);
                        auto &uv = *obj_uvs.push();
                        uv.X = (float)atof(args[0]);
                        uv.Y = 1 - (float)atof(args[1]); // @Note: gotta flip from OpenGL convention to D3D convention so importing from e.g. Blender works. (@Todo: Is this actually true?)
                    } else if (strcmp("vn", directive) == 0) {
                        // Normal
                        assert(matches == 4);
                        // Log("Normal: (%s, %s, %s)", args[0], args[1], args[2]);
                        auto &normal = *obj_normals.push();
                        normal.X = (float)atof(args[0]);
                        normal.Y = (float)atof(args[1]);
                        normal.Z = (float)atof(args[2]);
                    } else if (strcmp("f", directive) == 0) {
                        assert(obj_positions.count <= obj_colours.count);
                        // Triangle/Quad
                        assert(matches == 4 || matches == 5);
                        // Log("Triangle/Quad: (%s, %s, %s%s%s)", args[0], args[1], args[2], matches == 5 ? ", " : "", matches == 5 ? args[3] : "");
                        PH2MAP__Vertex24 verts_to_push[4] = {};
                        for (int i = 0; i < matches - 1; i++) {
                            PH2MAP__Vertex24 vert = {};
                            int index_pos = 0;
                            int index_uv = 0;
                            int index_normal = 0;
                            int sub_matches = sscanf(args[i], "%d/%d/%d", &index_pos, &index_uv, &index_normal);
                            if (sub_matches >= 1) { // %d[...]
                                assert(index_pos > 0);
                                vert.position[0] = obj_positions[index_pos - 1].X;
                                vert.position[1] = obj_positions[index_pos - 1].Y;
                                vert.position[2] = obj_positions[index_pos - 1].Z;
                                vert.color = obj_colours[index_pos - 1];
                            } else {
                                assert(false); // @Lazy
                            }
                            if (sub_matches == 3) { // %d/%d/%d
                                assert(index_uv > 0);
                                assert(index_normal > 0);
                                vert.uv[0] = obj_uvs[index_uv - 1].X;
                                vert.uv[1] = obj_uvs[index_uv - 1].Y;
                                vert.normal[0] = obj_normals[index_normal - 1].X;
                                vert.normal[1] = obj_normals[index_normal - 1].Y;
                                vert.normal[2] = obj_normals[index_normal - 1].Z;
                            } else if (sub_matches == 2) { // %d/%d
                                assert(index_uv > 0);
                                vert.uv[0] = obj_uvs[index_uv - 1].X;
                                vert.uv[1] = obj_uvs[index_uv - 1].Y;
                            } else if (sub_matches == 1) { // %d[...]
                                int dummy = 0;
                                sub_matches = sscanf(args[i], "%d//%d", &dummy, &index_normal);
                                assert(dummy == index_pos);
                                if (sub_matches == 2) { // %d//%d
                                    assert(index_normal > 0);
                                    vert.normal[0] = obj_normals[index_normal - 1].X;
                                    vert.normal[1] = obj_normals[index_normal - 1].Y;
                                    vert.normal[2] = obj_normals[index_normal - 1].Z;
                                } else {
                                    assert(false);
                                }
                            }
                            verts_to_push[i] = vert;
                        }
                        auto push_wound = [&] (int a, int b, int c) {
                            PH2MAP__Vertex24 vert_a = verts_to_push[a];
                            PH2MAP__Vertex24 vert_b = verts_to_push[b];
                            PH2MAP__Vertex24 vert_c = verts_to_push[c];
                            // Infer face winding by comparing cross product to normal
                            HMM_Vec3 v0 = { vert_a.position[0], vert_a.position[1], vert_a.position[2] };
                            HMM_Vec3 v1 = { vert_b.position[0], vert_b.position[1], vert_b.position[2] };
                            HMM_Vec3 v2 = { vert_c.position[0], vert_c.position[1], vert_c.position[2] };

                            HMM_Vec3 n0 = { vert_a.normal[0], vert_a.normal[1], vert_a.normal[2] };
                            HMM_Vec3 n1 = { vert_b.normal[0], vert_b.normal[1], vert_b.normal[2] };
                            HMM_Vec3 n2 = { vert_c.normal[0], vert_c.normal[1], vert_c.normal[2] };

                            HMM_Vec3 wound_normal = HMM_Cross((v1 - v0), (v2 - v0));
                            HMM_Vec3 given_normal = HMM_Norm(n0 + n1 + n2);

                            float dot = HMM_Dot(wound_normal, given_normal);

                            // bool is_wound_right = (dot >= 0);
                            // if (is_wound_right) {
                                current_verts->push(vert_a);
                                current_verts->push(vert_b);
                                current_verts->push(vert_c);
                            // } else {
                            //     current_verts->push(vert_b);
                            //     current_verts->push(vert_a);
                            //     current_verts->push(vert_c);
                            // }
                        };
                        push_wound(0, 1, 2);
                        if (matches == 5) { // Quad - upload another triangle
                            push_wound(0, 2, 3);
                        }
                    }
                    memset(b, 0, sizeof b);
                }

                assert(obj_positions.count);
                if (obj_positions.count) {
                    center /= (float)obj_positions.count;
                    g.cam_pos = center;
                    g.cam_pos.X *= 1 * SCALE;
                    g.cam_pos.Y *= -1 * SCALE;
                    g.cam_pos.Z *= -1 * SCALE;
                }

                Log("We got %lld positions, %lld uvs, %lld normals.", obj_positions.count, obj_uvs.count, obj_normals.count);

                {
                    MAP_Geometry geo = {};
                    if (!g.geometries.empty()) {
                        geo.subfile_index = ((MAP_Geometry *)g.geometries.sentinel->prev)->subfile_index;
                    }
                    g.geometries.push(geo);
                }

                for (int i = 0; i < 1 + import_materials.count; ++i) {
                    Array<PH2MAP__Vertex24> *verts = &materialless_unstripped_verts;
                    if (i > 0) {
                        verts = &import_materials[i - 1].unstripped_verts;
                        Log("Built %lld unstripped vertices for material %d.", verts->count, import_materials[i - 1].index_in_materials_array);
                    } else {
                        Log("Built %lld unstripped vertices for (no material).", verts->count);
                    }

                    if (verts->count <= 0) {
                        continue;
                    }

                    int num_meshes_to_add = ((int)verts->count + 65534) / 65535;
                    assert(num_meshes_to_add >= 1);

                    unsigned int material_index = 0;

                    if (i > 0) {

                        // Deduplicate or insert material
                        MAP_OBJ_Import_Material &import_material = import_materials[i - 1];

                        bool material_deduplicated = false;

                        if (MAP_Material *mat = g.materials.at_index(import_material.index_in_materials_array)) {
                            if ((mat->mode == import_material.mode) &&
                                (mat->diffuse_color == import_material.diffuse_color) &&
                                (mat->specular_color == import_material.specular_color) &&
                                (mat->specularity == import_material.specularity_f32) &&
                                (mat->texture_id == import_material.texture_id)) {

                                material_index = import_material.index_in_materials_array;
                                material_deduplicated = true;
                            }
                        }
                        
                        if (!material_deduplicated) {
                            int i = 0;
                            for (MAP_Material &mat : g.materials) {
                                defer { i++; };

                                if ((mat.mode == import_material.mode) &&
                                    (mat.diffuse_color == import_material.diffuse_color) &&
                                    (mat.specular_color == import_material.specular_color) &&
                                    (mat.specularity == import_material.specularity_f32) &&
                                    (mat.texture_id == import_material.texture_id)) {

                                    material_index = i;
                                    material_deduplicated = true;

                                    break;
                                }
                            }
                        }

                        if (!material_deduplicated) {
                            MAP_Material mat = {};
                            if (!g.materials.empty()) {
                                mat.subfile_index = ((MAP_Material *)g.materials.sentinel->prev)->subfile_index;
                            }
                            mat.mode = import_material.mode;
                            mat.diffuse_color = import_material.diffuse_color;
                            mat.specular_color = import_material.specular_color;
                            mat.specularity = import_material.specularity_f32;
                            mat.texture_id = import_material.texture_id;
                            material_index = (int)g.materials.count();
                            g.materials.push(mat);
                        }

                        // Deduplicate or insert texture
                        if (import_material.texture_was_found) {
                            bool texture_deduplicated = false;

                            if (MAP_Texture *tex = map_get_texture_by_id(g, import_material.texture_id)) {
                                texture_deduplicated = true;
                            }

                            if (!texture_deduplicated) {
                                MAP_Texture tex = {};
                                bool success = dds_import(import_material.dds_filename_buf, tex);
                                if (success) {
                                    tex.id = import_material.texture_id;
                                    tex.material = import_material.texture_unknown;

                                    if (g.texture_subfiles.empty()) {
                                        g.texture_subfiles.push();
                                    }
                                    ((MAP_Texture_Subfile *)g.texture_subfiles.sentinel->prev)->textures.push(tex);
                                }
                            }
                        }
                    }

                    for (int i = 0; i < num_meshes_to_add; ++i) {

                        int input_vertex_start = (i * 65535);
                        int input_vertex_count = i < num_meshes_to_add - 1 ? 65535 : (verts->count % 65535);

                        const PH2MAP__Vertex24 *input_verts_data = &(*verts)[input_vertex_start];

                        size_t index_count = input_vertex_count;
                        Array<unsigned int> remap = {}; defer { remap.release(); };
                        remap.resize(index_count); // allocate temporary memory for the remap table
                        size_t vertex_count = meshopt_generateVertexRemap(&remap[0], NULL, index_count, &input_verts_data[0], index_count, sizeof(input_verts_data[0]));

                        Array<unsigned int> list_indices = {}; defer { list_indices.release(); };
                        Array<PH2MAP__Vertex24> vertices = {}; defer { vertices.release(); };

                        list_indices.resize(index_count);
                        vertices.resize(vertex_count);

                        meshopt_remapIndexBuffer(list_indices.data, NULL, index_count, &remap[0]);
                        meshopt_remapVertexBuffer(vertices.data, &input_verts_data[0], index_count, sizeof(input_verts_data[0]), &remap[0]);

                        meshopt_optimizeVertexCacheStrip(list_indices.data, list_indices.data, index_count, vertex_count);

                        meshopt_optimizeOverdraw(list_indices.data, list_indices.data, index_count, &vertices[0].position[0], vertex_count, sizeof(vertices[0]), 1.05f);

                        meshopt_optimizeVertexFetch(vertices.data, list_indices.data, index_count, vertices.data, vertex_count, sizeof(vertices[0]));

                        Array<unsigned int> strip_indices = {}; defer { strip_indices.release(); };
                        strip_indices.resize(meshopt_stripifyBound(index_count));
                        // We'd like to use restart indices, but these meshes might be decals;
                        // decals don't have MeshParts, which means everything has to be 1 strip.
                        // (Or we have to autodetect bundles of strips of a particular length
                        // and let that be a mesh part, but we don't know that that'll work ahead
                        // of time!)
                        unsigned int restart_index = 0; // ~0u;
                        size_t strip_size = meshopt_stripify(&strip_indices[0], list_indices.data, index_count, vertex_count, restart_index);

                        strip_indices.resize(strip_size);

                        auto &geo = *(MAP_Geometry *)g.geometries.sentinel->prev;

                        MAP_Mesh &mesh = *geo.opaque_meshes.push();

                        MAP_Mesh_Vertex_Buffer &buf = *mesh.vertex_buffers.push();
                        buf.num_vertices = (int)vertices.count;
                        if (should_import_colours) {
                            buf.bytes_per_vertex = sizeof(vertices[0]);
                            buf.data.resize(vertices.count * sizeof(vertices[0]));
                            assert(buf.data.data);
                            memcpy(buf.data.data, vertices.data, vertices.count * sizeof(vertices[0]));
                        } else {
                            static Array<PH2MAP__Vertex20> vertices20 = {};
                            vertices20.clear();
                            for (auto &vert24 : vertices) {
                                vertices20.push({
                                                vert24.position[0], vert24.position[1], vert24.position[2],
                                                vert24.normal[0], vert24.normal[1], vert24.normal[2],
                                                vert24.uv[0], vert24.uv[1]
                                });
                            }

                            buf.bytes_per_vertex = sizeof(vertices20[0]);
                            buf.data.resize(vertices20.count * sizeof(vertices20[0]));
                            assert(buf.data.data);
                            memcpy(buf.data.data, vertices20.data, vertices20.count * sizeof(vertices20[0]));
                        }

                        MAP_Mesh_Part_Group &group = *mesh.mesh_part_groups.push();
                        group.material_index = material_index;
                        group.section_index = 0;

                        {
                            MAP_Mesh_Part *part = group.mesh_parts.push();
                            part->strip_count = 1;
                            part->strip_length = 0;

                            for (int i = 0; i < strip_indices.count;) {
                                if (strip_indices[i] == ~0u) {

                                    strip_indices.remove_ordered(i);

                                    part = group.mesh_parts.push();
                                    part->strip_count = 1;
                                    part->strip_length = 0;

                                } else {
                                    assert(strip_indices.data[i] < 65536);
                                    ++part->strip_length;
                                    ++i;
                                }
                            }

                        }

                        mesh.indices.reserve(strip_indices.count);
                        mesh.indices.count = strip_indices.count;
                        assert(mesh.indices.data);
                        for (int i = 0; i < strip_indices.count; ++i) {
                            mesh.indices.data[i] = (uint16_t)strip_indices.data[i];
                        }

                    }
                }

                MsgInfo("OBJ Import", "Imported!");
            } else {
                MsgErr("OBJ Load Error", "Couldn't open file \"%s\"!!", obj_file_buf);
            }
        }
        if (dds_file_buf) { // Texture import
            MAP_Texture tex = {};
            bool success = dds_import(dds_file_buf, tex);
            if (success) {
                MsgInfo("DDS Import", "Imported!");

                if (g.texture_subfiles.empty()) {
                    g.texture_subfiles.push();
                }
                ((MAP_Texture_Subfile *)g.texture_subfiles.sentinel->prev)->textures.push(tex);
            }
        }
    auto export_to_obj = [&] {
        char *mtl_export_name = mprintf("%s.mtl", obj_export_name);
        if (!mtl_export_name) {
            MsgErr("OBJ Export Error", "Couldn't build MTL filename for \"%s\".", obj_export_name);
            return;
        }
        FILE *obj = PH2CLD__fopen(obj_export_name, "w");
        if (!obj) {
            MsgErr("OBJ Export Error", "Couldn't open file \"%s\"!!", obj_export_name);
            return;
        }
        defer {
            fclose(obj);
        };
        FILE *mtl = PH2CLD__fopen(mtl_export_name, "w");
        if (!mtl) {
            MsgErr("OBJ Export Error", "Couldn't open file \"%s\"!!", mtl_export_name);
            return;
        }
        defer {
            fclose(mtl);
        };

        fprintf(obj, "# .MAP mesh export from Psilent pHill 2 Editor (" URL ")\n");
        fprintf(mtl, "# .MAP mesh export from Psilent pHill 2 Editor (" URL ")\n");
        fprintf(obj, "# Exported from filename: %s\n", g.opened_map_filename);
        fprintf(mtl, "# Exported from filename: %s\n", g.opened_map_filename);
        char time_buf[sizeof("YYYY-MM-DD HH:MM:SS UTC")];
        {
            time_t t = time(nullptr);
            auto result = strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
            assert(result);
        }
        fprintf(obj, "# Exported at %s\n", time_buf);
        fprintf(mtl, "# Exported at %s\n", time_buf);

        fprintf(obj, "\n");
        fprintf(obj, "mtllib %s\n", mtl_export_name);
        fprintf(obj, "\n");

        Array<int> vertices_per_selected_buffer = {};
        defer {
            vertices_per_selected_buffer.release();
        };

        for (auto &buf : g.map_buffers) {
            if (!buf.selected || &buf - g.map_buffers >= g.map_buffers_count) continue;
            vertices_per_selected_buffer.push((int)buf.vertices.count);
            for (auto &v : buf.vertices) {
                auto c = PH2MAP_u32_to_bgra(v.color);
                fprintf(obj, "v %f %f %f %f %f %f\n", v.position[0], v.position[1], v.position[2], c.Z, c.Y, c.X);
                fprintf(obj, "vt %f %f\n", v.uv[0], 1 - v.uv[1]); // @Note: gotta flip from D3D convention to OpenGL convention so Blender import works. (@Todo: Is this actually true?)
                fprintf(obj, "vn %f %f %f\n", v.normal[0], v.normal[1], v.normal[2]);
            }
        }
        fprintf(obj, "\n");
        {
            int colours_printed = 0;
            for (auto &buf : g.map_buffers) {
                if (!buf.selected || &buf - g.map_buffers >= g.map_buffers_count) continue;
                for (auto &v : buf.vertices) {
                    if (colours_printed % 64 == 0) {
                        fprintf(obj, "\n#MRGB ");
                    }
                    fprintf(obj, "%08x", v.color);
                    ++colours_printed;
                }
            }
        }
        fprintf(obj, "\n\n");

        static bool material_touched[65536];
        memset(material_touched, 0, sizeof(material_touched));

        int materials_count = (int)g.materials.count();
        assert(materials_count >= 0 && materials_count < 65536);

        int materials_referenced = 0;
        int unique_materials_referenced = 0;

        static bool texture_id_touched[65536];
        memset(texture_id_touched, 0, sizeof(texture_id_touched));

        int textures_referenced = 0;
        int unique_textures_referenced = 0;

        int index_base = 0;
        int selected_buffer_index = 0;
        for (MAP_Geometry_Buffer &buf : g.map_buffers) {
            if (&buf - g.map_buffers >= g.map_buffers_count) {
                break;
            }
            if (buf.selected) {
                defer { selected_buffer_index++; };
                int geo_index = 0;
                int mesh_index = 0;
                // @Lazy @Hack @@@
                for (auto &geo : g.geometries) {
                    mesh_index = 0;
                    for (auto &mesh : geo.opaque_meshes) {
                        if (&mesh == buf.mesh_ptr) {
                            goto double_break;
                        }
                        mesh_index++;
                    }
                    mesh_index = 0;
                    for (auto &mesh : geo.transparent_meshes) {
                        if (&mesh == buf.mesh_ptr) {
                            goto double_break;
                        }
                        mesh_index++;
                    }
                    mesh_index = 0;
                    for (auto &mesh : geo.decal_meshes) {
                        if (&mesh == buf.mesh_ptr) {
                            goto double_break;
                        }
                        mesh_index++;
                    }
                    geo_index++;
                }
                double_break:;
                MAP_Mesh &mesh = *buf.mesh_ptr;

                const char *source = "";
                if (buf.source == MAP_Geometry_Buffer_Source::Opaque) { source = "Opaque"; }
                else if (buf.source == MAP_Geometry_Buffer_Source::Transparent) { source = "Transparent"; }
                else if (buf.source == MAP_Geometry_Buffer_Source::Decal) { source = "Decal"; }

                fprintf(obj, "o Geometry_%d_%s_Mesh_%d\n", geo_index, source, mesh_index);
                int indices_start = 0;
                int mesh_part_group_index = 0;
                for (MAP_Mesh_Part_Group &mpg : mesh.mesh_part_groups) {
                    fprintf(obj, " g Geometry_%d_%s_Mesh_%d_MeshPartGroup_%d\n", geo_index, source, mesh_index, mesh_part_group_index);

                    int mat_index = mpg.material_index;

                    if (mat_index >= 0 && mat_index < 65536) {
                        if (mat_index < materials_count) {
                            materials_referenced++;
                            if (!material_touched[mat_index]) {
                                material_touched[mat_index] = true;
                                unique_materials_referenced++;
                            }
                            MAP_Material *mat_ = g.materials.at_index(mat_index);
                            assert(mat_);
                            MAP_Material &mat = *mat_;

                            auto map_tex = map_get_texture_buffer_by_id(g, mat.texture_id);
                            if (map_tex) {
                                assert(map_tex->texture_ptr);
                            }

                            fprintf(obj, "  usemtl PH2_%04x_%01x_%08x_%08x_%08x_%04x_%01x_%02x_PH2\n",
                                    (((uint16_t)mat_index) & 0xFFFF),
                                    (((uint8_t)mat.mode) & 0xF),
                                    (((uint32_t)mat.diffuse_color) & 0xFFFFFFFF),
                                    (((uint32_t)mat.specular_color) & 0xFFFFFFFF),
                                    ((*(uint32_t *)&mat.specularity) & 0xFFFFFFFF),
                                    (((uint16_t)mat.texture_id) & 0xFFFF),
                                    (((uint8_t)!!(map_tex && map_tex->texture_ptr)) & 0xF),
                                    (((uint8_t)(map_tex ? map_tex->texture_ptr->material : 0)) & 0xF));
                        } else {
                            fprintf(obj, "  # Note: This mesh part group referenced a material that couldn't be found in the file's material list at the time (index was %d, but there were only %d materials).\n", mat_index, materials_count);
                        }
                    } else {
                        fprintf(obj, "  # Note: This mesh part group tried to reference a material that is out of bounds (index was %d, minimum is 0, maximum is 65535).\n", mat_index);
                    }
                    defer { mesh_part_group_index++; };
                    int num_indices = buf.vertices_per_mesh_part_group[mesh_part_group_index];
                    assert(num_indices % 3 == 0);
                    for (int i = 0; i < num_indices; i += 3) {
                        int a = index_base + buf.indices[indices_start + i] + 1;
                        int b = index_base + buf.indices[indices_start + i + 1] + 1;
                        int c = index_base + buf.indices[indices_start + i + 2] + 1;
                        fprintf(obj, "  f %d/%d/%d %d/%d/%d %d/%d/%d\n", a, a, a, b, b, b, c, c, c);
                    }
                    indices_start += num_indices;
                    fprintf(obj, "\n");
                }
                fprintf(obj, "\n");

                index_base += vertices_per_selected_buffer[selected_buffer_index];
            }
        }

        fprintf(mtl, "# Used with file: %s\n", obj_export_name);
        fprintf(mtl, "\n");

        int mat_index = 0;
        for (MAP_Material &mat : g.materials) {
            defer { mat_index++; };
            if (!material_touched[mat_index]) continue;

            char *tex_export_name = mprintf("%s.tex_%d.dds", obj_export_name, mat.texture_id);
            if (!tex_export_name) {
                MsgErr("OBJ Export Error", "Couldn't build texture export string for texture ID #%d!", mat.texture_id);
                return;
            }
            defer { free(tex_export_name); };

            assert(mat.texture_id >= 0);
            assert(mat.texture_id < 65536);
            auto map_tex = map_get_texture_buffer_by_id(g, mat.texture_id);
            if (map_tex) {
                assert(map_tex->tex.id);
                MAP_Texture *tex = map_tex->texture_ptr;
                assert(tex);

                assert(tex->id >= 0); // Probably redundant but whatever!
                assert(tex->id < 65536);
                textures_referenced++;
                if (!texture_id_touched[tex->id]) {
                    texture_id_touched[tex->id] = true;
                    unique_textures_referenced++;

                    bool export_success = export_dds(*tex, tex_export_name);
                    if (!export_success) {
                        return;
                    }
                }
            }

            fprintf(mtl, "newmtl PH2_%04x_%01x_%08x_%08x_%08x_%04x_%01x_%02x_PH2\n",
                    (((uint16_t)mat_index) & 0xFFFF),
                    (((uint8_t)mat.mode) & 0xF),
                    (((uint32_t)mat.diffuse_color) & 0xFFFFFFFF),
                    (((uint32_t)mat.specular_color) & 0xFFFFFFFF),
                    ((*(uint32_t *)&mat.specularity) & 0xFFFFFFFF),
                    (((uint16_t)mat.texture_id) & 0xFFFF),
                    (((uint8_t)!!(map_tex && map_tex->texture_ptr)) & 0xF),
                    (((uint8_t)(map_tex ? map_tex->texture_ptr->material : 0)) & 0xF));
            fprintf(mtl, "  Ka 0.0 0.0 0.0\n");
            {
                auto c = PH2MAP_u32_to_bgra(mat.diffuse_color);
                fprintf(mtl, "  Kd %f %f %f\n", c.Z, c.Y, c.X);
            }
            {
                auto c = PH2MAP_u32_to_bgra(mat.specular_color);
                float spec_intensity = mat.specularity / 50.0f; // @Todo: ????????????
                c *= spec_intensity;
                fprintf(mtl, "  Ks %f %f %f\n", c.Z, c.Y, c.X);
            }
            // fprintf(mtl, "  d 1.0\n");

            if (map_tex) {
                fprintf(mtl, "  map_Kd %s\n", tex_export_name);
                assert(map_tex->texture_ptr);
                if (map_tex->texture_ptr->format != MAP_Texture_Format_BC1) {
                    fprintf(mtl, "  map_d -imfchan m %s\n", tex_export_name);
                }
            } else {
                fprintf(mtl, "  # Note: This material references a texture that couldn't be found in the file's texture list at the time (Texture ID was %d).\n", mat.texture_id);
            }

            fprintf(mtl, "\n");
        }

        assert(mat_index == materials_count); // Ensure we checked all materials.

        Log("Material deduplicator: %d unique materials referenced out of %d total.", unique_materials_referenced, materials_referenced);
        Log("Texture deduplicator: %d unique textures referenced out of %d total.", unique_textures_referenced, textures_referenced);

        MsgInfo("OBJ Export", "Exported!");
    };
    if (obj_export_name) {
        export_to_obj();
    }

    auto texture_preview_tooltip = [&g] (int tex_id) {
        ImGui::BeginTooltip();

        auto tex = map_get_texture_buffer_by_id(g, tex_id);
        if (tex) {

            const int DIM = 256;

            assert(tex);
            assert(tex->tex.id);
            float w = (float)tex->width;
            float h = (float)tex->height;
            float aspect = w / h;
            if (w > DIM) {
                w = DIM;
                h = w / aspect;
            }
            if (h > DIM) {
                h = DIM;
                w = h * aspect;
            }

            if (w > 0 && h > 0) {
                ImGui::Image((ImTextureID)(uintptr_t)tex->tex.id, ImVec2(w, h));
            }

        } else {
            ImGui::SameLine(0, 0);
            ImGui::Text("(No such texture)");
        }

        ImGui::EndTooltip();
    };

    if (g.show_editor) {
        ImGui::Begin("Editor", &g.show_editor, ImGuiWindowFlags_NoCollapse);
        defer {
            ImGui::End();
        };
        {
            auto iterate_mesh = [&] (const char *str, int i, MAP_Mesh &mesh) {
                int num_untouched = 0;
                int num_untouched_per_buf[4] = {};

                // @Todo: Do we still need this if we are only selecting on mesh granularity now?
                static bool (vertices_touched[4])[65536];
                static int (vertex_remap[4])[65536];

                for (auto &buf : mesh.vertex_buffers) {
                    int buf_index = (int)(&buf - mesh.vertex_buffers.data);
                    assert(buf_index >= 0);
                    assert(buf_index < 4);
                    memset(vertices_touched[buf_index], 0, buf.num_vertices);
                    int indices_index = 0;
                    for (auto &group : mesh.mesh_part_groups) {
                        if ((int)group.section_index == buf_index) {
                            for (auto &part : group.mesh_parts) {
                                for (int j = 0; j < part.strip_length * part.strip_count; j++) {
                                    int vert_index = mesh.indices[indices_index];
                                    vertices_touched[buf_index][vert_index] = true;
                                    ++indices_index;
                                }
                            }
                        } else {
                            for (auto &part : group.mesh_parts) {
                                indices_index += part.strip_length * part.strip_count;
                            }
                        }
                    }
                    for (int j = 0; j < buf.num_vertices; j++) {
                        if (!vertices_touched[buf_index][j]) {
                            num_untouched_per_buf[buf_index]++;
                        }
                    }
                }
                for (auto &num : num_untouched_per_buf) {
                    assert(num >= 0);
                    num_untouched += num;
                }
                if (num_untouched == 0) {
                    return;
                }
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, 0xff0d3be0);
                    ImGui::Text("%s #%d has %d unreferenced vertices", str, i, num_untouched);
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();
                if (!ImGui::Button("Prune (Delete)")) {
                    return;
                }
                for (auto &buf : mesh.vertex_buffers) {
                    int buf_index = (int)(&buf - mesh.vertex_buffers.data);
                    assert(buf_index >= 0);
                    assert(buf_index < 4);
                    if (!num_untouched_per_buf[buf_index]) {
                        continue;
                    }
                    int rolling_fixup = 0;
                    for (int vert_index_to_remove = 0; vert_index_to_remove < buf.num_vertices; vert_index_to_remove++) {
                        if (vertices_touched[buf_index][vert_index_to_remove]) {
                            vertex_remap[buf_index][vert_index_to_remove] = vert_index_to_remove - rolling_fixup;
                        } else {
                            rolling_fixup++;
                            vertex_remap[buf_index][vert_index_to_remove] = -1; // shouldn't ever be touched!!!
                        }
                    }
                    int num_untouched_vertices = rolling_fixup;
                    assert(num_untouched_vertices == num_untouched_per_buf[buf_index]);
                    int indices_index = 0;
                    for (auto &group : mesh.mesh_part_groups) {
                        for (auto &part : group.mesh_parts) {
                            for (int j = 0; j < part.strip_length * part.strip_count; j++) {
                                uint16_t &vert_index = mesh.indices[indices_index];
                                if ((int)group.section_index == buf_index) {
                                    int remapped = vertex_remap[buf_index][vert_index];
                                    assert(remapped >= 0);
                                    assert(remapped < buf.num_vertices - num_untouched_vertices);
                                    vert_index = (uint16_t)remapped;
                                }
                                ++indices_index;
                            }
                        }
                    }
                    for (int vert_index_to_remove = 0; vert_index_to_remove < buf.num_vertices;) {
                        if (!vertices_touched[buf_index][vert_index_to_remove]) {
                            // Ordered removal from the vertex buffer
                            memmove(buf.data.data +  vert_index_to_remove      * buf.bytes_per_vertex,
                                    buf.data.data + (vert_index_to_remove + 1) * buf.bytes_per_vertex,
                                    (buf.num_vertices - vert_index_to_remove - 1) * buf.bytes_per_vertex);
                            buf.data.resize(buf.num_vertices * buf.bytes_per_vertex);
                            assert(buf.data.data);
                            // Ordered removal from the vertices_touched array
                            memmove(&vertices_touched[buf_index][vert_index_to_remove],
                                    &vertices_touched[buf_index][vert_index_to_remove + 1],
                                    (buf.num_vertices - vert_index_to_remove - 1) * sizeof(vertices_touched[buf_index][0]));
                            --buf.num_vertices;
                        } else {
                            ++vert_index_to_remove;
                        }
                    }
                    buf.data.shrink_to_fit();
                }
            };
            for (auto &geo : g.geometries) {
                int i = 0;
                for (auto &mesh : geo.opaque_meshes) {
                    iterate_mesh("Opaque Mesh", i, mesh);
                    i++;
                }
            }
            for (auto &geo : g.geometries) {
                int i = 0;
                for (auto &mesh : geo.transparent_meshes) {
                    iterate_mesh("Transparent Mesh", i, mesh);
                    i++;
                }
            }
            for (auto &geo : g.geometries) {
                int i = 0;
                for (auto &mesh : geo.decal_meshes) {
                    iterate_mesh("Decal Mesh", i, mesh);
                    i++;
                }
            }
        }
        if (ImGui::CollapsingHeader("CLD Subgroups", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("CLD Subgroup Visibility Buttons");
            ImGui::Indent();
            defer {
                ImGui::Unindent();
                ImGui::PopID();
            };
            bool all_subgroups_on = true;
            for (int i = 0; i < 16; i++) {
                if (i) ImGui::SameLine();
                ImGui::PushID(i);
                if (ImGui::Checkbox("###CLD Subgroup Visible", &g.subgroup_visible[i])) {
                    g.staleify_cld();
                }
                if (!g.subgroup_visible[i]) {
                    all_subgroups_on = false;
                }
                ImGui::PopID();
            }
            ImGui::SameLine();
            if (!all_subgroups_on) {
                if (ImGui::Button("All")) {
                    for (int i = 0; i < 16; i++) {
                        g.subgroup_visible[i] = true;
                    }
                    g.staleify_cld();
                }
            } else {
                if (ImGui::Button("None")) {
                    for (int i = 0; i < 16; i++) {
                        g.subgroup_visible[i] = false;
                    }
                    g.staleify_cld();
                }
            }
        }
        ImGui::Separator();
#ifndef NDEBUG
        ImGui::Text("%lld undo frames", g.undo_stack.count);
        ImGui::Text("%lld redo frames", g.redo_stack.count);
        ImGui::Separator();
#endif
        if (ImGui::CollapsingHeader("MAP Geometries", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool disabled = (g.map_buffers_count <= 0);
            if (disabled) {
                ImGui::BeginDisabled();
            }
            defer {
                if (disabled) {
                    ImGui::EndDisabled();
                }
            };
            bool all_buffers_shown = g.map_buffers_count > 0;
            bool all_buffers_selected = g.map_buffers_count > 0;
            for (int i = 0; i < g.map_buffers_count; i++) {
                if (!g.map_buffers[i].shown) {
                    all_buffers_shown = false;
                }
                if (!g.map_buffers[i].selected) {
                    all_buffers_selected = false;
                }
            }
            if (all_buffers_shown ? ImGui::Button("Hide All") : ImGui::Button("Show All")) {
                g.solo_material = -1;
                for (int i = 0; i < g.map_buffers_count; i++) {
                    g.map_buffers[i].shown = !all_buffers_shown;
                }
                for (int i = g.map_buffers_count; i < g.map_buffers_max; i++) {
                    g.map_buffers[i].shown = true;
                }
            }
            ImGui::SameLine(); if (all_buffers_selected ? ImGui::Button("Select None") : ImGui::Button("Select All")) {
                g.solo_material = -1;
                for (int i = 0; i < g.map_buffers_count; i++) {
                    g.map_buffers[i].selected = !all_buffers_selected;
                }
                for (int i = g.map_buffers_count; i < g.map_buffers_max; i++) {
                    g.map_buffers[i].selected = false;
                }
                g.select_cld_group = -1; // @Cleanup: cld selection clear function?
                g.select_cld_face = -1;
                // @Note: Bleh!!!
                g.overall_center_needs_recalc = true;
            }

            auto visit_mesh_buffer = [&] (MAP_Mesh &mesh, auto &&lambda) -> void {
                for (auto &b : g.map_buffers) {
                    if (&b - g.map_buffers >= g.map_buffers_count) {
                        break;
                    }
                    if (b.mesh_ptr == &mesh) {
                        lambda(b);
                        break;
                    }
                }
            };

            auto deselect_all_buffers_and_check_for_multi_select = [&] (auto &&check) -> bool {
                bool was_multi = false;
                for (auto &b : g.map_buffers) {
                    if (&b - g.map_buffers >= g.map_buffers_count) {
                        break;
                    }
                    was_multi |= check(b);
                }

                for (auto &b : g.map_buffers) b.selected = false;

                return was_multi;
            };

            auto map_buffer_ui = [&] (MAP_Geometry_Buffer &buf) {
                auto &meshes = get_meshes(buf);
                const char *source = "I made it up";
                switch (buf.source) {
                    case MAP_Geometry_Buffer_Source::Opaque: { source = "Opaque"; } break;
                    case MAP_Geometry_Buffer_Source::Transparent: { source = "Transparent"; } break;
                    case MAP_Geometry_Buffer_Source::Decal: { source = "Decal"; } break;
                    default: { assert(false); } break;
                }

                if (!g.map_must_update) { // @HACK !!!!!!!!!!!!!!!! @@@@@@ !!!!!!!!
                    auto &mesh = *buf.mesh_ptr;

                    for (int i = 0; i < mesh.vertex_buffers.count; ++i) {
                        auto &section = mesh.vertex_buffers[i];

                        const char *vertex_format = "(Unknown format)";
                        int vertex_format_combo = 4;
                        if (section.bytes_per_vertex == 0x14) {
                            vertex_format = "(XU)";
                            vertex_format_combo = 0;
                        } else if (section.bytes_per_vertex == 0x18) {
                            vertex_format = "(XCU)";
                            vertex_format_combo = 1;
                        } else if (section.bytes_per_vertex == 0x20) {
                            vertex_format = "(XNU)";
                            vertex_format_combo = 2;
                        } else if (section.bytes_per_vertex == 0x24) {
                            vertex_format = "(XNCU)";
                            vertex_format_combo = 3;
                        }

                        char b[256]; snprintf(b, sizeof(b), "Vertex Section #%d %s", i, vertex_format);
                        if (!ImGui::TreeNodeEx(b, ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen)) continue;
                        defer { ImGui::TreePop(); };

                        int new_vertex_format_combo = vertex_format_combo;
                        if (vertex_format_combo < 0 || vertex_format_combo > 3) {
                            continue;
                        }
                        bool changed = ImGui::Combo("Mode##Dropdown between valid states", &new_vertex_format_combo,
                                                    "XU (Pos, UV)\0"
                                                    "XCU (Pos, Colour, UV)\0"
                                                    "XNU (Pos, Normal, UV)\0"
                                                    "XNCU (Pos, Normal, Colour, UV)\0"
                                                    "\0");
                        if (!changed) {
                            continue;
                        }

                        if (vertex_format_combo == 2 && new_vertex_format_combo == 3) {
                            Array<PH2MAP__Vertex20> original = {};
                            defer {
                                original.release();
                            };
                            original.resize(section.num_vertices);
                            assert(sizeof(original[0]) == section.bytes_per_vertex);
                            assert(original.count * sizeof(original[0]) == section.num_vertices * section.bytes_per_vertex);
                            memcpy(original.data, section.data.data, section.num_vertices * section.bytes_per_vertex);

                            section.bytes_per_vertex = 0x24;
                            section.data.resize(section.num_vertices * section.bytes_per_vertex);
                            for (int i = 0; i < section.num_vertices; ++i) {
                                *(PH2MAP__Vertex24 *)&(section.data[i * section.bytes_per_vertex]) = {
                                    original[i].position[0], original[i].position[1], original[i].position[2],
                                    original[i].normal[0], original[i].normal[1], original[i].normal[2],
                                    0xffffffff,
                                    original[i].uv[0], original[i].uv[1]
                                };
                            }
                        } else if (vertex_format_combo == 3 && new_vertex_format_combo == 2) {
                            Array<PH2MAP__Vertex24> original = {};
                            defer {
                                original.release();
                            };
                            original.resize(section.num_vertices);
                            assert(sizeof(original[0]) == section.bytes_per_vertex);
                            assert(original.count * sizeof(original[0]) == section.num_vertices * section.bytes_per_vertex);
                            memcpy(original.data, section.data.data, section.num_vertices * section.bytes_per_vertex);

                            section.bytes_per_vertex = 0x20;
                            section.data.resize(section.num_vertices * section.bytes_per_vertex);
                            for (int i = 0; i < section.num_vertices; ++i) {
                                *(PH2MAP__Vertex20 *)&(section.data[i * section.bytes_per_vertex]) = {
                                    original[i].position[0], original[i].position[1], original[i].position[2],
                                    original[i].normal[0], original[i].normal[1], original[i].normal[2],
                                    original[i].uv[0], original[i].uv[1]
                                };
                            }
                        } else if (vertex_format_combo == 2 && new_vertex_format_combo == 0) {
                            Array<PH2MAP__Vertex20> original = {};
                            defer {
                                original.release();
                            };
                            original.resize(section.num_vertices);
                            assert(sizeof(original[0]) == section.bytes_per_vertex);
                            assert(original.count * sizeof(original[0]) == section.num_vertices * section.bytes_per_vertex);
                            memcpy(original.data, section.data.data, section.num_vertices * section.bytes_per_vertex);

                            section.bytes_per_vertex = 0x14;
                            section.data.resize(section.num_vertices * section.bytes_per_vertex);
                            for (int i = 0; i < section.num_vertices; ++i) {
                                *(PH2MAP__Vertex14 *)&(section.data[i * section.bytes_per_vertex]) = {
                                    original[i].position[0], original[i].position[1], original[i].position[2],
                                    original[i].uv[0], original[i].uv[1]
                                };
                            }
                        }
                    }

                    int mesh_part_group_index = 0; // @Lazy
                    for (auto &mesh_part_group : mesh.mesh_part_groups) {
                        const char *vertex_format = "(Unknown format)";
                        auto &section = mesh.vertex_buffers[mesh_part_group.section_index];
                        if (section.bytes_per_vertex == 0x14) {
                            vertex_format = "(XU)";
                        } else if (section.bytes_per_vertex == 0x18) {
                            vertex_format = "(XCU)";
                        } else if (section.bytes_per_vertex == 0x20) {
                            vertex_format = "(XNU)";
                        } else if (section.bytes_per_vertex == 0x24) {
                            vertex_format = "(XNCU)";
                        }
                        char b[256]; snprintf(b, sizeof(b), "Mesh Part Group #%d - %d Mesh Parts %s", mesh_part_group_index, (int)mesh_part_group.mesh_parts.count, vertex_format);
                        defer { mesh_part_group_index++; };

                        char bogged_reason[8192] = {};
                        int  bogged_reason_n = 0;
                        bool bogged = is_mesh_part_group_bogged(g, buf.source, mesh, mesh_part_group, buf.mesh_index, (int)mesh_part_group_index, &bogged_reason, &bogged_reason_n);
                        ImGui::PushStyleColor(ImGuiCol_Text, bogged ? bogged_flash_colour(g) : ImGui::GetStyle().Colors[ImGuiCol_Text]);

                        bool ret = ImGui::TreeNodeEx(b, ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen);

                        ImGui::PopStyleColor();
                        if (bogged && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", bogged_reason);
                        }

                        if (!ret) {
                            continue;
                        }
                        ImGui::PushID(&mesh_part_group);
                        defer {
                            ImGui::PopID();
                            ImGui::TreePop();
                        };

                        int mat_index = mesh_part_group.material_index;
                        ImGui::Columns(2, nullptr, false);
                        if (ImGui::InputInt("Material", &mat_index)) {
                            // int mat_count = (int)g.materials.count() + (int)!!g.materials.empty();
                            // mat_index = (mat_index % mat_count + mat_count) % mat_count;
                            mesh_part_group.material_index = (uint16_t)mat_index;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();

                            MAP_Material *material = nullptr;

                            int tex_id = 0;

                            if (mat_index >= 0 && mat_index < 65536) {
                                material = g.materials.at_index(mat_index);
                                if (material) {
                                    assert(material->texture_id >= 0);
                                    assert(material->texture_id < 65536);
                                    tex_id = material->texture_id;
                                }
                            }

                            if (material) {
                                ImGui::Text("Material #%d => Texture ID #%d ", mat_index, tex_id);
                            } else {
                                ImGui::Text("Material #%d (No such material)", mat_index);
                            }

                            ImGui::EndTooltip();

                            if (material) {
                                texture_preview_tooltip(tex_id);
                            }
                        }
                        ImGui::Columns(1);
                    }
                }
            };

            int i = 0;
            ImGui::BeginDisabled(g.stale() || g.control_state == ControlState::Dragging);
            defer { ImGui::EndDisabled(); };
            for (auto& geo : g.geometries) {
                defer { i++; };
                ImGui::PushID("MAP Selection Nodes");
                ImGui::PushID(&geo);
                defer {
                    ImGui::PopID();
                    ImGui::PopID();
                };

                LinkedList<MAP_Mesh, The_Arena_Allocator> *const the_mesh_arrays[3] = {&geo.opaque_meshes, &geo.transparent_meshes, &geo.decal_meshes};

                bool empty = true;
                bool all_on = true;
                bool any_on = false;
                bool all_selected = true;
                bool any_selected = false;
                for (auto &meshes : the_mesh_arrays) {
                    for (auto &mesh : *meshes) {
                        visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                            empty = false;
                            assert(b.geometry_ptr == &geo);
                            all_on &= b.shown;
                            any_on |= b.shown;
                            all_selected &= b.selected;
                            any_selected |= b.selected;
                        });
                    }
                }
                if (empty) {
                    ImGui::BeginDisabled();
                }
                bool pressed;
                if (!all_on && any_on) {
                    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                    pressed = ImGui::Checkbox("###Geometry All Visible", &all_on);
                    ImGui::PopItemFlag();
                } else {
                    pressed = ImGui::Checkbox("###Geometry All Visible", &all_on);
                }
                if (pressed) {
                    g.solo_material = -1;
                    for (auto &meshes : the_mesh_arrays) {
                        for (auto &mesh : *meshes) {
                            visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                                assert(b.geometry_ptr == &geo);
                                b.shown = all_on;
                            });
                        }
                    }
                }
                if (empty) {
                    ImGui::EndDisabled();
                }

                char b[512]; snprintf(b, sizeof b, "Geo #%d (ID %d)", i, geo.id);
                ImGui::SameLine();
                auto flags = ImGuiTreeNodeFlags_OpenOnArrow |
                    ImGuiTreeNodeFlags_OpenOnDoubleClick |
                    ImGuiTreeNodeFlags_AllowItemOverlap |
                    (any_selected * ImGuiTreeNodeFlags_Selected);
                if (empty) {
                    flags = ImGuiTreeNodeFlags_Leaf;
                }

                char bogged_reason[8192] = {};
                int  bogged_reason_n = 0;
                bool bogged = is_geo_bogged(g, geo, &bogged_reason, &bogged_reason_n);
                ImGui::PushStyleColor(ImGuiCol_Text, bogged ? bogged_flash_colour(g) : ImGui::GetStyle().Colors[ImGuiCol_Text]);

                ImGui::SameLine(); bool ret = /*empty ? (ImGui::Text(b), false) : */ImGui::TreeNodeEx(b, flags);
                defer { if (ret) ImGui::TreePop(); };

                ImGui::PopStyleColor();
                if (bogged && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", bogged_reason);
                }

                if (!empty && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    bool orig = all_selected;
                    bool was_multi = false;
                    if (!ImGui::GetIO().KeyShift) {
                        was_multi = deselect_all_buffers_and_check_for_multi_select([&] (MAP_Geometry_Buffer &b) -> bool {
                            bool inside = false;
                            for (auto &meshes : the_mesh_arrays) {
                                for (auto &mesh : *meshes) {
                                    visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b2) {
                                        if (&b2 == &b) {
                                            inside = true;
                                        }
                                    });
                                }
                            }
                            return (!inside && b.selected);
                        });
                    }
                    bool selected = was_multi || !orig;
                    for (auto &meshes : the_mesh_arrays) {
                        for (auto &mesh : *meshes) {
                            visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                                b.selected = selected;
                            });
                        }
                    }
                    g.select_cld_group = -1;
                    g.select_cld_face = -1;
                    // @Note: Bleh.
                    g.overall_center_needs_recalc = true;
                }
                {
                    ImGui::SameLine();
                    // ImGui::BeginDisabled(empty);
                    if (ImGui::SmallButton("Delete") /*&& empty*/) {
                        g.geometries.remove_ordered(&geo);
                        geo.release();
                        break;
                    }
                    // ImGui::EndDisabled();
                }
                if (!ret) { continue; }

                for (auto &meshes : the_mesh_arrays) {
                    ImGui::PushID(meshes);
                    defer {
                        ImGui::PopID();
                    };

                    if (meshes == &geo.transparent_meshes && !geo.transparent_meshes.empty() && !geo.opaque_meshes.empty()) {
                        ImGui::Separator();
                    }
                    if (meshes == &geo.decal_meshes && !geo.decal_meshes.empty() && (!geo.opaque_meshes.empty() || !geo.transparent_meshes.empty())) {
                        ImGui::Separator();
                    }

                    int mesh_index = 0;
                    for (auto &mesh : *meshes) {

                        defer { mesh_index++; };

                        ImGui::PushID(mesh_index);
                        defer {
                            ImGui::PopID();
                        };

                        bool empty = true;
                        bool all_on = true;
                        bool any_on = false;
                        bool all_selected = true;
                        bool any_selected = false;
                        visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                            assert(b.geometry_ptr == &geo);
                            empty = false;
                            all_on &= b.shown;
                            any_on |= b.shown;
                            all_selected &= b.selected;
                            any_selected |= b.selected;
                        });
                        // assert(!empty);

                        bool pressed;
                        if (!all_on && any_on) {
                            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                            pressed = ImGui::Checkbox("###Mesh All Part Groups Visible", &all_on);
                            ImGui::PopItemFlag();
                        } else {
                            pressed = ImGui::Checkbox("###Mesh All Part Groups Visible", &all_on);
                        }
                        if (pressed) {
                            g.solo_material = -1;
                            visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                                assert(b.geometry_ptr == &geo);
                                b.shown = all_on;
                            });
                        }

                        MAP_Geometry_Buffer_Source source = MAP_Geometry_Buffer_Source::Opaque;
                        char *source_str = "???";
                        if (meshes == &geo.opaque_meshes)           { source_str = "Opaque";      source = MAP_Geometry_Buffer_Source::Opaque; }
                        else if (meshes == &geo.transparent_meshes) { source_str = "Transparent"; source = MAP_Geometry_Buffer_Source::Transparent; }
                        else if (meshes == &geo.decal_meshes)       { source_str = "Decal";       source = MAP_Geometry_Buffer_Source::Decal; }
                        char b[512]; snprintf(b, sizeof b, "%s Mesh #%d", source_str, mesh_index);
                        auto flags = ImGuiTreeNodeFlags_OpenOnArrow |
                            ImGuiTreeNodeFlags_AllowItemOverlap |
                            ImGuiTreeNodeFlags_OpenOnDoubleClick |
                            (any_selected * ImGuiTreeNodeFlags_Selected);

                        char bogged_reason[8192] = {};
                        int  bogged_reason_n = 0;
                        bool bogged = is_mesh_bogged(g, source, mesh, mesh_index, &bogged_reason, &bogged_reason_n);
                        ImGui::PushStyleColor(ImGuiCol_Text, bogged ? bogged_flash_colour(g) : ImGui::GetStyle().Colors[ImGuiCol_Text]);

                        ImGui::SameLine(); bool ret = ImGui::TreeNodeEx(b, flags);

                        ImGui::PopStyleColor();
                        if (bogged && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", bogged_reason);
                        }

                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                            bool orig = all_selected;
                            bool was_multi = false;
                            if (!ImGui::GetIO().KeyShift) {
                                was_multi = deselect_all_buffers_and_check_for_multi_select([&] (MAP_Geometry_Buffer &b) -> bool {
                                    bool inside = false;
                                    visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b2) {
                                        if (&b2 == &b) {
                                            inside = true;
                                        }
                                    });
                                    return (!inside && b.selected);
                                });
                            }
                            bool selected = was_multi || !orig;
                            visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                                b.selected = selected;
                            });
                            g.select_cld_group = -1;
                            g.select_cld_face = -1;
                            // @Note: Bleh.
                            g.overall_center_needs_recalc = true;
                        }
                        defer { if (ret) ImGui::TreePop(); };

                        auto add_to = [] (LinkedList<MAP_Mesh, The_Arena_Allocator> *meshes, MAP_Mesh *mesh) {
                            if (!meshes->sentinel) {
                                meshes->init();
                            }
                            node_insert_before(meshes->sentinel, mesh);
                        };
                        if (meshes == &geo.opaque_meshes) {
                            ImGui::SameLine(); if (ImGui::SmallButton("-> Transparent")) {
                                meshes->remove_ordered(&mesh);
                                add_to(&geo.transparent_meshes, &mesh);
                                break;
                            }
                            ImGui::SameLine(); if (ImGui::SmallButton("-> Decal")) {
                                meshes->remove_ordered(&mesh);
                                add_to(&geo.decal_meshes, &mesh);
                                break;
                            }
                        }
                        if (meshes == &geo.transparent_meshes) {
                            ImGui::SameLine(); if (ImGui::SmallButton("-> Opaque")) {
                                meshes->remove_ordered(&mesh);
                                add_to(&geo.opaque_meshes, &mesh);
                                break;
                            }
                            ImGui::SameLine(); if (ImGui::SmallButton("-> Decal")) {
                                meshes->remove_ordered(&mesh);
                                add_to(&geo.decal_meshes, &mesh);
                                break;
                            }
                        }
                        if (meshes == &geo.decal_meshes) {
                            ImGui::SameLine(); if (ImGui::SmallButton("-> Opaque")) {
                                meshes->remove_ordered(&mesh);
                                add_to(&geo.opaque_meshes, &mesh);
                                break;
                            }
                            ImGui::SameLine(); if (ImGui::SmallButton("-> Transparent")) {
                                meshes->remove_ordered(&mesh);
                                add_to(&geo.transparent_meshes, &mesh);
                                break;
                            }
                        }

                        if (!ret) { continue; }

                        visit_mesh_buffer(mesh, [&] (MAP_Geometry_Buffer &b) {
                            assert(b.geometry_ptr == &geo);
                            ImGui::PushID(&b);
                            defer {
                                ImGui::PopID();
                            };

                            map_buffer_ui(b);
                        });
                    }
                }
            }
        }
    }
    if (g.show_edit_widget) {
        defer {
            ImGui::End();
        };
        if (ImGui::Begin("Edit Widget", &g.show_edit_widget, ImGuiWindowFlags_NoCollapse)) {
            PH2CLD_Face *face = nullptr;
            bool is_quad = false;
            if (g.select_cld_group >= 0 && g.select_cld_face >= 0) {
                PH2CLD_Face *faces = g.cld.group_0_faces;
                size_t num_faces = g.cld.group_0_faces_count;
                if (g.select_cld_group == 1) { faces = g.cld.group_1_faces; num_faces = g.cld.group_1_faces_count; }
                if (g.select_cld_group == 2) { faces = g.cld.group_2_faces; num_faces = g.cld.group_2_faces_count; }
                if (g.select_cld_group == 3) { faces = g.cld.group_3_faces; num_faces = g.cld.group_3_faces_count; }

                face = &faces[g.select_cld_face];
                is_quad = face->quad;
            }
            {
                if (!face) {
                    ImGui::BeginDisabled();
                }
                defer {
                    if (!face) {
                        ImGui::EndDisabled();
                    }
                };
                ImGui::Text("CLD Face");
                if (ImGui::Checkbox("Quad", &is_quad)) {
                    face->quad = is_quad;
                    if (face->quad) {
                        // Build a parallelogram out of vertices v0, v1, and v2. v3 = v2 + (v0 - v1)
                        face->vertices[3][0] = face->vertices[2][0] + face->vertices[0][0] - face->vertices[1][0];
                        face->vertices[3][1] = face->vertices[2][1] + face->vertices[0][1] - face->vertices[1][1];
                        face->vertices[3][2] = face->vertices[2][2] + face->vertices[0][2] - face->vertices[1][2];
                    } else {
                        face->vertices[3][0] = 0;
                        face->vertices[3][1] = 0;
                        face->vertices[3][2] = 0;
                    }
                    g.staleify_cld();
                }
            }
            ImGui::Separator();
            int num_map_bufs_selected = 0;
            for (auto &buf : g.map_buffers) {
                if (&buf - g.map_buffers >= g.map_buffers_count) {
                    break;
                }
                if (buf.selected) {
                    num_map_bufs_selected++;
                }
            }
            if (!num_map_bufs_selected) {
                ImGui::BeginDisabled();
            }
            defer {
                if (!num_map_bufs_selected) {
                    ImGui::EndDisabled();
                }
            };
            ImGui::Text("MAP Mesh Part Group");
            ImGui::Text("%d selected", num_map_bufs_selected);

            bool go_to = ImGui::Button("Go To Center");
            ImGui::NewLine();
            bool del = ImGui::Button(num_map_bufs_selected > 1 ? "Delete All###Delete Mesh Part Groups" : "Delete###Delete Mesh Part Groups");
            bool duplicate = ImGui::Button(num_map_bufs_selected > 1 ? "Duplicate All###Duplicate Mesh Part Groups" : "Duplicate###Duplicate Mesh Part Groups");
            bool move = false;
            ImGui::NewLine();
            ImGui::Text("Move:");
            ImGui::SameLine(); if (ImGui::Button("+X")) { g.displacement = { +100, 0, 0 }; }
            ImGui::SameLine(); if (ImGui::Button("-X")) { g.displacement = { -100, 0, 0 }; }
            ImGui::SameLine(); if (ImGui::Button("+Y")) { g.displacement = { 0, +100, 0 }; }
            ImGui::SameLine(); if (ImGui::Button("-Y")) { g.displacement = { 0, -100, 0 }; }
            ImGui::SameLine(); if (ImGui::Button("+Z")) { g.displacement = { 0, 0, +100 }; }
            ImGui::SameLine(); if (ImGui::Button("-Z")) { g.displacement = { 0, 0, -100 }; }
            bool scale = false;
            ImGui::Text("By:");
            ImGui::SameLine(); ImGui::DragFloat3("###Displacement", &g.displacement.X, 10);
            ImGui::SameLine(); if (ImGui::Button("Cancel###Displacement")) {
                g.displacement = {};
            }
            ImGui::NewLine();
            ImGui::Text("Scale:");
            ImGui::SameLine(); if (ImGui::Button("- X")) { g.scaling_factor = { - 1, + 1, + 1 }; }
            ImGui::SameLine(); if (ImGui::Button("X*2")) { g.scaling_factor = { + 2, + 1, + 1 }; }
            ImGui::SameLine(); if (ImGui::Button("- Y")) { g.scaling_factor = { + 1, - 1, + 1 }; }
            ImGui::SameLine(); if (ImGui::Button("Y*2")) { g.scaling_factor = { + 1, + 2, + 1 }; }
            ImGui::SameLine(); if (ImGui::Button("- Z")) { g.scaling_factor = { + 1, + 1, - 1 }; }
            ImGui::SameLine(); if (ImGui::Button("Z*2")) { g.scaling_factor = { + 1, + 1, + 2 }; }
            ImGui::Text("By:");
            ImGui::SameLine(); ImGui::DragFloat3("###Scaling", &g.scaling_factor.X, 0.01f);
            ImGui::SameLine(); if (ImGui::Button("Cancel###Scaling")) {
                g.scaling_factor = { 1, 1, 1 };
            }
            if (ImGui::Button("Apply###Displacement and Scaling")) {
                move = true;
                scale = true;
            }
            ImGui::NewLine();

            if (g.overall_center_needs_recalc) {
                g.overall_center = {}; // @Lazy
            }
            int overall_center_sum = 0;

            auto process_mesh = [&] (MAP_Geometry_Buffer &buf, bool duplicate, bool move, bool scale, bool get_center) {
                auto &meshes = get_meshes(buf);
                if (move || scale || get_center) {
                    auto &mesh = *buf.mesh_ptr;
                    mesh.bbox_override = false;

                    assert(The_Arena_Allocator::contains(&mesh, sizeof(mesh)));

                    int vertices_count = 0;
                    HMM_Vec3 center = {};
                    for (auto &vertex_buffer : mesh.vertex_buffers) {
                        for (int i = 0; i < vertex_buffer.num_vertices; i++) {
                            vertices_count++;
                            float (*position)[3] = (float(*)[3])(vertex_buffer.data.data + vertex_buffer.bytes_per_vertex * i);
                            center.X += (*position)[0];
                            center.Y += (*position)[1];
                            center.Z += (*position)[2];
                            if (scale) {
                                (*position)[0] -= g.overall_center.X;
                                (*position)[1] -= g.overall_center.Y;
                                (*position)[2] -= g.overall_center.Z;
                                (*position)[0] *= g.scaling_factor.X;
                                (*position)[1] *= g.scaling_factor.Y;
                                (*position)[2] *= g.scaling_factor.Z;
                                (*position)[0] += g.overall_center.X;
                                (*position)[1] += g.overall_center.Y;
                                (*position)[2] += g.overall_center.Z;
                            }
                            if (move) {
                                (*position)[0] += g.displacement.X;
                                (*position)[1] += g.displacement.Y;
                                (*position)[2] += g.displacement.Z;
                            }
                        }
                    }
                    // Log("Moved/Scaled!");
                    if (get_center) {
                        center /= (float)vertices_count + !vertices_count;
                        g.overall_center += center;
                        overall_center_sum += 1;
                    }
                }
                if (duplicate) {
                    auto &mesh = *buf.mesh_ptr;

                    MAP_Mesh &new_mesh = *meshes.push();
                    new_mesh.bbox_override = false;
                    new_mesh.indices.resize(mesh.indices.count);
                    assert(new_mesh.indices.data);
                    memcpy(new_mesh.indices.data, mesh.indices.data, mesh.indices.count * sizeof(mesh.indices[0]));

                    new_mesh.vertex_buffers.reserve(mesh.vertex_buffers.count);
                    assert(new_mesh.vertex_buffers.data);
                    for (auto &vertex_buffer : mesh.vertex_buffers) {
                        MAP_Mesh_Vertex_Buffer &new_vertex_buffer = *new_mesh.vertex_buffers.push();
                        assert(&new_vertex_buffer);
                        new_vertex_buffer.bytes_per_vertex = vertex_buffer.bytes_per_vertex;
                        new_vertex_buffer.num_vertices = vertex_buffer.num_vertices;
                        int n = vertex_buffer.num_vertices * vertex_buffer.bytes_per_vertex;
                        assert(n == vertex_buffer.data.count);
                        new_vertex_buffer.data.resize(n);
                        assert(new_vertex_buffer.data.data);
                        memcpy(new_vertex_buffer.data.data, vertex_buffer.data.data, n);
                    }

                    for (auto &mpg : mesh.mesh_part_groups) {
                        MAP_Mesh_Part_Group &new_mpg = *new_mesh.mesh_part_groups.push();
                        assert(&new_mpg);
                        new_mpg.material_index = mpg.material_index;
                        new_mpg.section_index = mpg.section_index;
                        new_mpg.mesh_parts.resize(mpg.mesh_parts.count);
                        assert(new_mpg.mesh_parts.data);
                        memcpy(new_mpg.mesh_parts.data, mpg.mesh_parts.data, mpg.mesh_parts.count * sizeof(mpg.mesh_parts[0]));
                    }

                    assert(The_Arena_Allocator::contains(&new_mesh, sizeof(new_mesh)));

                    assert(!g.geometries.empty());

                    // Log("Duped!");
                }
            };
            for (auto &buf : g.map_buffers) {
                if (buf.selected) {
                    process_mesh(buf, false, false, false, g.overall_center_needs_recalc);
                }
            }
            if (overall_center_sum) {
                g.overall_center /= (float)overall_center_sum;
            }
            for (auto &buf : g.map_buffers) {
                if (buf.selected) {
                    process_mesh(buf, duplicate, move, scale, false);
                }
            }
            if (go_to) {
                g.cam_pos = g.overall_center;
                g.cam_pos.X *= 1 * SCALE;
                g.cam_pos.Y *= -1 * SCALE;
                g.cam_pos.Z *= -1 * SCALE;
            }
            if (g.overall_center_needs_recalc) { // By here, we'll have calced the center
                g.overall_center_needs_recalc = false;
            }
            if (move) {
                g.overall_center += g.displacement;
            }
            if (move || scale) { // By here, we'll have performed ops.
                g.scaling_factor = { 1, 1, 1 };
                g.displacement = {};
            }

            auto clear_out_mesh_for_deletion = [&] (MAP_Geometry_Buffer &buf) {
                auto &meshes = get_meshes(buf);
                MAP_Mesh &mesh = *buf.mesh_ptr;
                mesh.release(); // @Lazy.
            };
            if (del) {
                for (auto &buf : g.map_buffers) {
                    if (&buf - g.map_buffers >= g.map_buffers_count) {
                        break;
                    }
                    if (buf.selected) {
                        clear_out_mesh_for_deletion(buf);
                        buf.selected = false;
                    }
                }
                g.overall_center_needs_recalc = true;
            }
            for (auto &geo : g.geometries) {
                auto prune_empty_meshes = [&] (LinkedList<MAP_Mesh, The_Arena_Allocator> &meshes) {
                    for (auto mesh = meshes.begin(); mesh != meshes.end();) {
                        auto next = mesh.node->next;
                        if ((*mesh).mesh_part_groups.empty()) {
                            mesh->release();
                            meshes.remove_ordered(mesh.node);
                        }
                        mesh.node = (MAP_Mesh *)next;
                    }
                };
                prune_empty_meshes(geo.opaque_meshes);
                prune_empty_meshes(geo.transparent_meshes);
                prune_empty_meshes(geo.decal_meshes);
            }
        }
    }

    {
        if (requested_save_filename) {
            Array<uint8_t> filedata = {};
            defer {
                filedata.release();
            };
            map_write_to_memory(g, &filedata);
            bool success = false;
            if (requested_save_filename) {
                time_t backup_time = time(nullptr); // default to current-time if we can't determine the mtime
                struct _stat st = {};
                if (!_stat(requested_save_filename, &st)) {
                    if (st.st_mtime) {
                        backup_time = st.st_mtime;
                    }
                }
                char buf[sizeof("YYYY-MM-DD_HH-MM-SS")];
                char *bak_filename = nullptr;
                // @Hack ughhhhhh @@@ @@@ @@@ I just don't want to add a new scope here (IMO these should be returns from a func, not nested ifs)
                if (strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", gmtime(&backup_time))) {
                    bak_filename = mprintf("%s.%s.map.bak.zip", requested_save_filename, buf);
                }
                if (bak_filename) {
                    uint16_t *filename16 = utf8_to_utf16(requested_save_filename);
                    if (filename16) {
                        // If the file exists, we start with the presumption that we need to back it up.
                        bool backup = file_exists(filename16);
                        // However, if there are already backups, and the most recent one is identical to the original file, we don't need to back it up.
                        auto check_latest_backup = [&] {
                            HANDLE orig_win = CreateFileW((LPWSTR)filename16, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (orig_win == INVALID_HANDLE_VALUE) return false;
                            defer { CloseHandle(orig_win); };
                            LARGE_INTEGER orig_len = {};
                            if (!GetFileSizeEx(orig_win, &orig_len)) return false;
                            uint64_t n = (uint64_t)orig_len.QuadPart;
                            char *glob = mprintf("%s.*.map.bak.zip", requested_save_filename);
                            if (!glob) return false;
                            defer { free(glob); };
                            uint16_t *glob16 = utf8_to_utf16(glob);
                            if (!glob16) return false;
                            defer { free(glob16); };
                            struct _wfinddata64_t newest_valid_backup = {};
                            {
                                struct _wfinddata64_t filedata = {};
                                intptr_t directory = _wfindfirst64((wchar_t *)glob16, &filedata);
                                if (directory < 0) return false;
                                defer { _findclose(directory); };
                                uint64_t newest_date = 0;
                                while (_wfindnext64(directory, &filedata) >= 0) {
                                    int periods_found = 0;
                                    auto dot = wcslen(filedata.name) + 1;
                                    while (periods_found < 4 && dot-- > 0) {
                                        if (filedata.name[dot] == '/' || filedata.name[dot] == '\\') {
                                            break; // directory found before all the dots.
                                        } else if (filedata.name[dot] == '.') {
                                            ++periods_found;
                                        }
                                    }
                                    if (periods_found == 4) {
                                        uint64_t y = 0, m = 1, d = 1, h = 1, min = 1, s = 1;
                                        int matches = swscanf((wchar_t *)(filedata.name + dot), L".%llu-%llu-%llu_%llu-%llu-%llu.map.bak.zip",
                                            &y, &m, &d, &h, &min, &s);
                                        if (matches == 6) {
                                            if (y < (1ull << (63 - 40)) && m <= 12 && d <= 31 && h <= 23 && min <= 59 && s <= 60) {
                                                uint64_t lexicographic = (y << 40) | (m << 32) | (d << 24) | (h << 16) | (min << 8) | s;
                                                if (newest_date <= lexicographic) {
                                                    newest_date = lexicographic;
                                                    newest_valid_backup = filedata;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            if (errno != ENOENT) return false;
                            auto slash = (uint16_t *)max(wcsrchr((wchar_t *)filename16, '/'), wcsrchr((wchar_t *)filename16, '\\'));
                            char *bak_filepath = mprintf("%.*ls%ls", slash ? (int)(slash - filename16 + 1) : 0, filename16, newest_valid_backup.name);
                            if (!bak_filepath) return false;
                            defer { free(bak_filepath); };
                            struct zip_t *zip = zip_open(bak_filepath, 0, 'r');
                            if (!zip) return false;
                            defer { zip_close(zip); };
                            auto newest_valid_backup_name8 = utf16_to_utf8((uint16_t *)newest_valid_backup.name);
                            if (!newest_valid_backup_name8) return false;
                            defer { free(newest_valid_backup_name8); };
                            char *dotzip = strrchr(newest_valid_backup_name8, '.');
                            assert(dotzip && dotzip[0] == '.');
                            dotzip[0] = 0; // @Hack
                            if (zip_entry_open(zip, newest_valid_backup_name8)) return false;
                            dotzip[0] = '.';
                            defer { zip_entry_close(zip); };
                            if (n != zip_entry_size(zip)) return false;
                            FILE *f = PH2CLD__fopen(requested_save_filename, "rb");
                            if (!f) return false;
                            defer { fclose(f); };
                            // @Speed: Youchie!
                            auto bufa = (char *)malloc(n * 2);
                            auto bufb = bufa + n;
                            defer { free(bufa); };
                            if (fread(bufa, 1, n, f) != n) return false;
                            if ((uint64_t)zip_entry_noallocread(zip, bufb, n) != n) return false;
                            bool equal = (memcmp(bufa, bufb, n) == 0);
                            if (!equal) return false;
                            // Transfer ownership of the backup name string
                            free(bak_filename);
                            bak_filename = newest_valid_backup_name8;
                            newest_valid_backup_name8 = nullptr;
                            Log("Relying on existing backup \"%s\".", bak_filename);
                            return true;
                        };
                        if (check_latest_backup()) {
                            backup = false;
                        }
                        uint16_t *bak_filename16 = utf8_to_utf16(bak_filename);
                        if (bak_filename16) {
                            // If the original file existed, we'll try to backup, and backup_succeded will reflect success.
                            // If the file didn't exist, we can't back anything up, so backup_succeeded will be 'true' in
                            // order to drive the editor to save the file without complaining about lacking a backup.
                            // @Todo: Copy the initial saved level so it can always be written out as a backup.
                            bool backup_succeeded = true;
                            auto try_backup = [&] {
                                struct zip_t *zip = zip_open(bak_filename, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
                                if (!zip) return false;
                                bool delete_zip = true;
                                defer { if (delete_zip) DeleteFileW((LPWSTR)bak_filename16); }; // @Errdefer
                                defer { zip_close(zip); };
                                char *filename_pathless = max(max(strrchr(bak_filename, '/'), strrchr(bak_filename, '\\')) + 1, bak_filename);
                                char *dotzip = strrchr(filename_pathless, '.');
                                assert(dotzip && dotzip[0] == '.');
                                dotzip[0] = 0; // @Hack
                                if (zip_entry_open(zip, filename_pathless)) return false;
                                dotzip[0] = '.';
                                if (zip_entry_fwrite(zip, requested_save_filename)) return false;
                                if (zip_entry_close(zip)) return false;
                                delete_zip = false;
                                Log("Created new backup \"%s\".", filename_pathless);
                                return true;
                            };
                            if (backup) {
                                backup_succeeded = try_backup();
                            }
                            bool should_save = false;
                            if (backup_succeeded) {
                                should_save = true;
                            } else {
                                if (MessageBoxA(0,
                                    "A backup of this file couldn't be written to disk.\n\nDo you want to save, and overwrite the file, without any backup?",
                                    "Save Backup Failed",
                                    MB_YESNO | MB_ICONWARNING | MB_TASKMODAL) == IDYES) {
                                    should_save = true;
                                }
                            }
                            if (should_save) {
                                FILE *f = _wfopen((wchar_t *)filename16, L"wb");
                                if (f) {
                                    if (fwrite(filedata.data, 1, filedata.count, f) == (int)filedata.count) {
                                        Log("Saved to \"%s\".", requested_save_filename);
                                        success = true;
                                        // Semi awful: commandeer the requested save filename
                                        free(g.opened_map_filename);
                                        g.opened_map_filename = requested_save_filename;
                                        requested_save_filename = nullptr;
                                    } else {
                                        Log("Couldn't write file data!!!");
                                    }
                                    fclose(f);
                                } else {
                                    Log("Couldn't open %s for writing", requested_save_filename);
                                }
                                auto restore_from_backup = [&] {
                                    struct zip_t *zip = zip_open(bak_filename, 0, 'r');
                                    if (!zip) return false;
                                    defer { zip_close(zip); };
                                    char *filename_pathless = max(max(strrchr(bak_filename, '/'), strrchr(bak_filename, '\\')) + 1, bak_filename);
                                    char *dotzip = strrchr(filename_pathless, '.');
                                    assert(dotzip && dotzip[0] == '.');
                                    dotzip[0] = 0; // @Hack
                                    if (zip_entry_open(zip, filename_pathless)) return false;
                                    dotzip[0] = '.';
                                    if (zip_entry_fread(zip, requested_save_filename)) return false;
                                    if (zip_entry_close(zip)) return false;
                                    return true;
                                };
                                if (!success) {
                                    if (MessageBoxA(0,
                                        "The file couldn't be written to disk.\n\nDo you want to attempt to restore from a backup?",
                                        "Save Failed",
                                        MB_YESNO | MB_ICONERROR | MB_TASKMODAL) == IDYES) {
                                        // Attempt to recover file contents by overwriting with the backup.
                                        if (restore_from_backup()) {
                                            // If we had written a backup just now, prompt to delete it.
                                            if (backup_succeeded) {
                                                if (MessageBoxA(0,
                                                    "The file was restored from a freshly made backup.\nDo you want to delete this redundant backup?",
                                                    "Restore Succeded",
                                                    MB_YESNO | MB_ICONWARNING | MB_TASKMODAL) == IDYES) {
                                                    if (DeleteFileW((LPCWSTR)bak_filename16)) {
                                                        MsgInfo("Backup Deleted", "The backup was deleted.");
                                                    } else {
                                                        MsgErr("Backup Deletion Failed", "The backup couldn't be deleted. Sorry!");
                                                    }
                                                }
                                            } else {
                                                MsgInfo("Restore Succeded", "The file was restored from:\n\"%s\".", bak_filename);
                                            }
                                        } else {
                                            MsgErr("Restore Failed", "The file couldn't be restored from a backup. Sorry!");
                                        }
                                    } else {
                                        Log("User declined to restore from backup.");
                                    }
                                }
                            } else {
                                Log("Couldn't create backup and user declined to overwrite.");
                            }
                            free(bak_filename16);
                        } else {
                            Log("Couldn't convert backup filename to utf16!!!");
                        }
                        free(filename16);
                    } else {
                        Log("Couldn't convert filename to utf16!!!");
                    }
                    free(bak_filename);
                } else {
                    Log("Couldn't build backup filename!!!");
                }
            } else {
                Log("No file loaded!!!");
            }
        }
    }
    {
        auto s = mprintf("Psilent pHill 2 Editor%s%s", g.opened_map_filename ? " - " : "", g.opened_map_filename ? g.opened_map_filename : "");
        if (s) {
            sapp_set_window_title(s);
            free(s);
        }
    }
    if (g.map_must_update) {
        g.ui_selected_texture_subfile = nullptr;
        g.ui_selected_texture = nullptr;
    }
    if (g.show_textures) {
        ImGui::SetNextWindowPos(ImVec2 { 60, sapp_height() * 0.98f - 280 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(256, 256), ImGuiCond_FirstUseEver);
        ImGui::Begin("Textures", &g.show_textures, ImGuiWindowFlags_NoCollapse);
        defer {
            ImGui::End();
        };
        auto size = ImGui::GetWindowSize();
        ImGui::BeginChild("texture_list", ImVec2(120, size.y - 50));

        const char *numbered_map_filename = g.opened_map_filename;
        const char *non_numbered_filename = g.opened_map_filename;
        defer { free((void *)non_numbered_filename); };
        if (numbered_map_filename) {
            int len = (int)strlen(numbered_map_filename);
            int i = len - 1;
            for (; i >= 0; i--) {
                if (numbered_map_filename[i] == '/' || numbered_map_filename[i] == '\\') {
                    ++i;
                    break;
                }
            }
            numbered_map_filename += i;

            non_numbered_filename = get_non_numbered_dependency_filename(numbered_map_filename);
        }

        bool has_ever_seen_a_subfile_from_the_non_numbered_dependency = false;
        bool has_ever_seen_a_subfile_from_the_main_numbered_map = false;
        int subfile_number = 0;
        for (auto &sub : g.texture_subfiles) {
            ImGui::PushID(&sub);
            defer {
                ImGui::PopID();
            };

            const float indent = ImGui::GetStyle().IndentSpacing / 2;

            if (sub.came_from_non_numbered_dependency) {
                if (!has_ever_seen_a_subfile_from_the_non_numbered_dependency) {
                    has_ever_seen_a_subfile_from_the_non_numbered_dependency = true;
                    subfile_number = 0;

                    ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", non_numbered_filename);
                    ImGui::Separator();
                }
            } else {
                if (!has_ever_seen_a_subfile_from_the_main_numbered_map) {
                    has_ever_seen_a_subfile_from_the_main_numbered_map = true;
                    subfile_number = 0;

                    if (has_ever_seen_a_subfile_from_the_non_numbered_dependency) {
                        ImGui::NewLine();
                    }
                    ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", numbered_map_filename);
                    ImGui::Separator();
                }
            }

            ImGui::Indent(indent);
            defer { ImGui::Unindent(indent); };

            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Subfile %d", subfile_number);
            ImGui::Separator();

            for (auto &tex : sub.textures) {
                ImGui::Indent(indent);
                defer { ImGui::Unindent(indent); };

                char b[16]; snprintf(b, sizeof b, "ID %d", tex.id);
                bool selected = g.ui_selected_texture_subfile == &sub && g.ui_selected_texture == &tex;
                if (ImGui::Selectable(b, selected)) {
                    if (selected) {
                        g.ui_selected_texture_subfile = nullptr;
                        g.ui_selected_texture = nullptr;
                    } else {
                        g.ui_selected_texture_subfile = &sub;
                        g.ui_selected_texture = &tex;
                    }
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    texture_preview_tooltip(tex.id);
                }
            }

            subfile_number++;
        }

        ImGui::EndChild();

        ImGui::SameLine(0, 0);
        if (g.ui_selected_texture_subfile && g.ui_selected_texture) {
            ImGui::BeginChild("texture_panel");

            ImGui::PushID(g.ui_selected_texture_subfile);
            ImGui::PushID(g.ui_selected_texture);

            defer {
                ImGui::PopID();
                ImGui::PopID();
                ImGui::EndChild();
            };

            assert(g.ui_selected_texture_subfile->textures.has_node(g.ui_selected_texture));

            bool show_image = false;

            ImGui::SameLine(0, 0);
            ImGui::BeginChild("Editable stuff in the texture panel", ImVec2{0, 45});

            bool non_numbered = (g.ui_selected_texture_subfile->came_from_non_numbered_dependency);
            ImGui::BeginDisabled(non_numbered);

            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                g.ui_selected_texture_subfile->textures.remove_ordered(g.ui_selected_texture);
                g.ui_selected_texture->release();
                g.ui_selected_texture_subfile = nullptr;
                g.ui_selected_texture = nullptr;
            } else {
                show_image = true;
                auto &sub = *g.ui_selected_texture_subfile;
                auto &tex = *g.ui_selected_texture;
                char *dds_export_path = nullptr;
                ImGui::SameLine(); if (ImGui::Button("Export DDS...")) {
                    dds_export_path = win_import_or_export_dialog(L"DDS Texture File\0" "*.dds\0"
                                                                  "All Files\0" "*.*\0",
                                                                  L"Save DDS", false);
                }
                if (dds_export_path) {
                    bool success = export_dds(tex, dds_export_path);
                    if (success) {
                        MsgInfo("DDS Export", "Exported!");
                    }
                }

                char *dds_import_buf = nullptr;
                ImGui::SameLine(); if (ImGui::Button("Replace with DDS...")) {
                    dds_import_buf = win_import_or_export_dialog(L"DDS Texture File\0" "*.dds\0"
                                                                  "All Files\0" "*.*\0",
                                                                 L"Open DDS", true);
                }
                bool import_success = false;
                if (dds_import_buf) { // Texture import
                    MAP_Texture new_tex = {};
                    import_success = dds_import(dds_import_buf, new_tex);
                    if (import_success) {
                        (Node &)new_tex = (Node &)tex;
                        new_tex.id = tex.id;
                        new_tex.material = tex.material;
                        tex.release();
                        tex = new_tex;
                    }
                }

                ImGui::SameLine();
                if (tex.format == MAP_Texture_Format_BC1)       ImGui::Text("%dx%d - Format 0x%03x: BC1 (RGB - Opaque)", tex.width, tex.height, tex.format);
                if (tex.format == MAP_Texture_Format_BC2)       ImGui::Text("%dx%d - Format 0x%03x: BC2 (RGBA - Transparent/Decal)", tex.width, tex.height, tex.format);
                if (tex.format == MAP_Texture_Format_BC3)       ImGui::Text("%dx%d - Format 0x%03x: BC3 (RGBA - Transparent/Decal)", tex.width, tex.height, tex.format);
                if (tex.format == MAP_Texture_Format_BC3_Maybe) ImGui::Text("%dx%d - Format 0x%03x: BC3 (RGBA - Transparent/Decal)", tex.width, tex.height, tex.format);

                ImGui::NewLine();
                {
                    ImGui::SameLine(0, 0);
                    ImGui::Columns(2);
                    {
                        int x = tex.id;
                        ImGui::InputInt("ID", &x);
                        x = clamp(x, 0, 65535);
                        tex.id = (uint16_t)x;
                    }
                    ImGui::NextColumn();
                    {
                        int x = tex.material;
                        ImGui::InputInt("\"Material\" (?)", &x);
                        x = clamp(x, 0, 255);
                        tex.material = (uint8_t)x;
                    }
                    ImGui::Columns(1);
                }
            }
            ImGui::EndDisabled();

            ImGui::EndChild();
            if (non_numbered && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open \"%s\" to edit this texture.", non_numbered_filename);
            }

            if (show_image) {
                auto &tex = *g.ui_selected_texture;
                float w = (float)tex.width;
                float h = (float)tex.height;
                float aspect = w / h;
                ImGui::NewLine();
                ImGui::SameLine();
                ImGui::Checkbox("Actual Size", &g.texture_actual_size);
                if (!g.texture_actual_size) {
                    w = size.x - 149; // yucky hack
                    h = size.y - 109; // yucky hack
                    if (w > size.x - 150) {
                        w = size.x - 150;
                        h = w / aspect;
                    }
                    if (h > size.y - 110) {
                        h = size.y - 110;
                        w = h * aspect;
                    }
                }
                MAP_Texture_Buffer *map_texture = nullptr;
                for (auto &map_tex : g.map_textures) {
                    assert(map_tex.subfile_ptr);
                    assert(map_tex.texture_ptr);
                    if (map_tex.subfile_ptr == g.ui_selected_texture_subfile &&
                        map_tex.texture_ptr == g.ui_selected_texture) {
                        map_texture = &map_tex;
                        break;
                    }
                }
                assert(map_texture);
                assert(map_texture->tex.id);
                if (w > 0 && h > 0) {
                    ImGui::NewLine();
                    ImGui::SameLine();
                    ImGui::Image((ImTextureID)(uintptr_t)map_texture->tex.id, ImVec2(w, h));
                }
            }
        }
    }

    if (g.solo_material > g.materials.count()) {
        g.solo_material = -1;
    }

    if (g.show_materials) {
        ImGui::SetNextWindowPos(ImVec2 { 60 + 256 + 20, sapp_height() * 0.98f - 280 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(512, 256), ImGuiCond_FirstUseEver);
        ImGui::Begin("Materials", &g.show_materials, ImGuiWindowFlags_NoCollapse);
        defer {
            ImGui::End();
        };
        MAP_Material *delete_mat = nullptr;
        int i = 0;
        {
            ImGui::Columns(6, nullptr, false);
            ImGui::NextColumn();
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Mode");
            ImGui::NextColumn();
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Texture ID");
            ImGui::NextColumn();
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Diffuse Colour");
            ImGui::NextColumn();
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Specular Colour");
            ImGui::NextColumn();
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Specularity");
            ImGui::Columns(1);
        }
        for (auto &mat : g.materials) {
            ImGui::PushID("Material iteration");
            ImGui::PushID(&mat);
            ImGui::Columns(6, nullptr, false);
            bool res = ImGui::TreeNodeEx("Material Tree Node", ImGuiTreeNodeFlags_AllowItemOverlap, "");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::Text("Texture ID #%d ", mat.texture_id);
                ImGui::EndTooltip();
                texture_preview_tooltip(mat.texture_id);
            }
            ImGui::SameLine();
            bool soloed = (g.solo_material == i);
            if (ImGui::SmallButton(soloed ? "U" : "S")) {
                soloed = !soloed;
                if (soloed) {
                    g.solo_material = i;
                } else {
                    g.solo_material = -1;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", soloed ?
                                  "Unsolo this material - show all faces in the level" :
                                  "Solo this material - only show faces with this material");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("D###Delete material")) {
                delete_mat = &mat;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("Delete this material (!)");
            }
            ImGui::SameLine();
            {
                ImGui::Text("#%d", i);
                ImGui::NextColumn();
                if (PH2MAP_material_mode_is_valid(mat.mode)) {
                    const char *mode_names[7] = {
                        "0: Emissive",
                        "1: Diffuse",
                        "2: Specular + Diffuse",
                        "3: Vantablack",
                        "4: Vertex Colour",
                        "",
                        "6: Unknown Diffuse",
                    };
                    ImGui::Text(mode_names[mat.mode]);
                } else {
                    ImGui::Text("%d (invalid)", mat.mode);
                }
                ImGui::NextColumn();
                if (PH2MAP_material_mode_is_valid(mat.mode)) {
                    char b[64]; snprintf(b, sizeof b, "%d###Texture ID", mat.texture_id);
                    auto c = ImVec4{0.2f, 0.2f, 0.2f, 1.0f};
                    ImGui::PushStyleColor(ImGuiCol_Button, c);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
                    ImGui::SmallButton(b);
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        texture_preview_tooltip(mat.texture_id);
                    }
                }
                if (mat.mode == 0 || mat.mode == 1 || mat.mode == 2 || mat.mode == 6) {
                    uint32_t u = mat.diffuse_color;
                    int b = ((u >> 0) & 0xff), g = ((u >> 8) & 0xff), r = ((u >> 16) & 0xff), a = ((u >> 24) & 0xff);

                    ImGui::NextColumn();
                    ImGui::Text("(%d,%d,%d)", r, g, b);
                }
                if (mat.mode == 2) {
                    uint32_t u = mat.specular_color;
                    int b = ((u >> 0) & 0xff), g = ((u >> 8) & 0xff), r = ((u >> 16) & 0xff), a = ((u >> 24) & 0xff);

                    ImGui::NextColumn();
                    ImGui::Text("(%d,%d,%d)", r, g, b);
                    ImGui::NextColumn();
                    ImGui::Text("%f", mat.specularity);
                }
            }
            ImGui::Columns(1);
            if (res) {
                defer { ImGui::TreePop(); };

                ImGui::Columns(2, nullptr, false);
                if (PH2MAP_material_mode_is_valid(mat.mode)) {
                    int x = mat.mode - (mat.mode == 6); // 6 => 5
                    bool changed = ImGui::Combo("Mode##Dropdown between valid states", &x,
                                 "0: Emissive?\0"
                                 "1: Diffuse: Vertex Normals\0"
                                 "2: Specular + Diffuse\0"
                                 "3: Unknown - Vantablack?\0"
                                 "4: Diffuse: Vertex Colours + Normals\0"
                                 "6: Unknown - also Colored Diffuse?\0"
                                 "\0");
                    if (changed && x >= 0 && x <= 5) {
                        x += (x == 5); // 5 => 6
                        mat.mode = (int16_t)x;
                        if (mat.mode == 0) {
                            mat.specular_color = 0xff000000;
                            mat.specularity = 0;
                        } else if (mat.mode == 1) {
                            mat.specular_color = 0xff000000;
                            mat.specularity = 0;
                        } else if (mat.mode == 2) {
                            mat.diffuse_color |= 0xff000000;
                            if ((mat.diffuse_color & 0x00ffffff) == 0) {
                                mat.diffuse_color = 0xffffffff;
                            }
                            mat.specular_color |= 0xff000000;
                            if ((mat.specular_color & 0x00ffffff) == 0) {
                                mat.specular_color = 0xffffffff;
                            }
                            if (mat.specularity <= 0) {
                                mat.specularity = 100;
                            }
                        } else if (mat.mode == 3) { // @Todo: how does the texture format work here?
                            mat.diffuse_color = 0xff000000;
                            mat.specular_color = 0xff000000;
                            mat.specularity = 0;
                        } else if (mat.mode == 4) {
                            mat.diffuse_color = 0xff000000;
                            mat.specular_color = 0xff000000;
                            mat.specularity = 0;
                        } else if (mat.mode == 6) {
                            mat.diffuse_color |= 0xff000000;
                            if ((mat.diffuse_color & 0x00ffffff) == 0) {
                                mat.diffuse_color = 0xffffffff;
                            }
                            mat.specular_color = 0xff000000;
                            mat.specularity = 0;
                        }
                    }
                } else {
                    int x = mat.mode;
                    ImGui::InputInt("Mode##Int because invalid", &x);
                    mat.mode = (int16_t)x;
                }
                ImGui::NextColumn();
                {
                    int x = mat.texture_id;
                    ImGui::InputInt("TexID", &x);
                    x = clamp(x, 0, 65535);
                    mat.texture_id = (uint16_t)x;
                    if (ImGui::IsItemHovered()) {
                        texture_preview_tooltip(mat.texture_id);
                    }
                }
                ImGui::Columns(1);

                if (!PH2MAP_material_mode_is_valid(mat.mode) ||
                    mat.mode == 0 || mat.mode == 1 || mat.mode == 2 || mat.mode == 6) {
                    auto bgra = PH2MAP_u32_to_bgra(mat.diffuse_color);
                    ImVec4 rgba = { bgra.Z, bgra.Y, bgra.X, bgra.W };
                    bool changed = ImGui::ColorEdit3("Diffuse Color (before gamma)", &rgba.x);
                    bgra = { rgba.z, rgba.y, rgba.x, rgba.w };
                    mat.diffuse_color = PH2MAP_bgra_to_u32(bgra);
                    if (mat.mode == 2 || mat.mode == 6) {
                        if (changed) {
                            mat.diffuse_color |= 0xff000000;
                            if ((mat.diffuse_color & 0x00ffffff) == 0) {
                                mat.diffuse_color |= 1;
                            }
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                            ImGui::SetTooltip("Note: Mode %d materials can't be fully black. Sorry! :(", mat.mode);
                        }
                    }
                }

                if (!PH2MAP_material_mode_is_valid(mat.mode) || mat.mode == 2) {
                    auto bgra = PH2MAP_u32_to_bgra(mat.specular_color);
                    ImVec4 rgba = { bgra.Z, bgra.Y, bgra.X, bgra.W };
                    bool changed = ImGui::ColorEdit3("Specular Color (before gamma)", &rgba.x);
                    bgra = { rgba.z, rgba.y, rgba.x, rgba.w };
                    mat.specular_color = PH2MAP_bgra_to_u32(bgra);
                    if (mat.mode == 2) {
                        if (changed) {
                            mat.specular_color |= 0xff000000;
                            if ((mat.specular_color & 0x00ffffff) == 0) {
                                mat.specular_color |= 1;
                            }
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                            ImGui::SetTooltip("Note: Mode %d materials can't be fully black. Sorry! :(", mat.mode);
                        }
                    }
                }

                if (!PH2MAP_material_mode_is_valid(mat.mode) || mat.mode == 2) {
                    bool changed = ImGui::SliderFloat("Specularity", &mat.specularity, 0, 300.0f, "%.3f");
                    if (changed && mat.specularity <= 0) {
                        mat.specularity = 0.001f;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                        ImGui::SetTooltip("Note: Mode 2 materials can't have 0 specularity. Sorry! :(");
                    }
                }

                ImGui::NewLine();
            }

            ImGui::Separator();
            ImGui::PopID();
            ImGui::PopID();
            i++;
        }
        if (delete_mat) {
            g.materials.remove_ordered(delete_mat);
            // @Todo @@@: Index patching on EVERY SINGLE mesh group!!!!!!!!!!!!!!!!! @@@
        }
        if (ImGui::Button("New Material")) {
            MAP_Material mat = {};
            if (!g.materials.empty()) {
                mat.subfile_index = ((MAP_Material *)g.materials.sentinel->prev)->subfile_index;
            }
            mat.mode = 1;
            mat.texture_id = 0;
            mat.diffuse_color = 0xffffffff;
            mat.specular_color = 0xff000000;
            mat.specularity = 0;
            g.materials.push(mat);
        }
    }
    if (g.show_viewport) {
        ImGui::SetNextWindowPos(ImVec2 { sapp_width() * 0.5f, 20 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(sapp_width() * 0.5f, sapp_height() * 0.5f - 20), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1);
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0, 0, 0, 1});
        defer {
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        };
        if (ImGui::Begin("Viewport", &g.show_viewport, ImGuiWindowFlags_NoCollapse)) {
            ImGui::Columns(3);
            ImGui::SliderAngle("Camera FOV", &g.fov, FOV_MIN * (360 / TAU32), FOV_MAX * (360 / TAU32));
            ImGui::NextColumn();
            if (ImGui::Button("Reset Camera")) {
                g.cam_pos = {};
                g.pitch = 0;
                g.yaw = 0;
                g.fov = FOV_DEFAULT;
                g.bg_col = BG_COL_DEFAULT;
                g.solo_material = -1;
            }
            ImGui::SameLine(); ImGui::Text("(%.0f, %.0f, %.0f)", g.cam_pos.X / SCALE, g.cam_pos.Y / -SCALE, g.cam_pos.Z / -SCALE);
            ImGui::NextColumn();
            ImGui::ColorEdit3("BG Colour", &g.bg_col.X);
            ImGui::Columns(1);
            ImGui::Checkbox("Textures", &g.textured);
            ImGui::SameLine(); ImGui::Checkbox("Lighting Colours", &g.use_lighting_colours);
            ImGui::SameLine(); ImGui::Checkbox("Material Colours", &g.use_material_colours);
            ImGui::SameLine(); ImGui::Checkbox("Front Sides Only", &g.cull_backfaces);
            ImGui::SameLine(); ImGui::Checkbox("Wireframe", &g.wireframe);

            static uint32_t text_col = 0xff12aef2;
            ImGui::PushStyleColor(ImGuiCol_Text, text_col);
            ImGui::SameLine(); ImGui::Text(" Note: Level may look brighter or darker in-game");
            ImGui::PopStyleColor();
            // static bool editing = false;
            // ImGui::Checkbox("edit", &editing);
            // if (editing) {
            //     auto c = ImGui::ColorConvertU32ToFloat4(text_col);
            //     ImGui::ColorEdit4("asdf", &c.x);
            //     text_col = ImGui::ColorConvertFloat4ToU32(c);
            //     ImGui::Text("%08x", text_col);
            // }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, {g.bg_col.X, g.bg_col.Y, g.bg_col.Z, 1});
            defer {
                ImGui::PopStyleColor();
            };
            if (ImGui::BeginChild("###Viewport Rendering Region")) {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddCallback(viewport_callback, &g);
            }
            ImGui::EndChild();
        }
        ImGui::End();
    } else {
        g.view_x = -1;
        g.view_y = -1;
        g.view_w = -1;
        g.view_h = -1;
    }

    g.click_result = {};
    if (!g.stale() && g.control_state != ControlState::Dragging) {
        Ray ray = g.clicked ? g.click_ray : g.mouse_ray; // @Yuck

        const HMM_Vec3 origin = -g.cld_origin();
        const HMM_Mat4 Tinv = HMM_Translate(-origin);
        const HMM_Mat4 Sinv = HMM_Scale( { 1 / SCALE, 1 / -SCALE, 1 / -SCALE });
        const HMM_Mat4 Minv = Tinv * Sinv;
        const HMM_Vec3 ray_pos = (Minv * HMM_Vec4{ ray.pos.X, ray.pos.Y, ray.pos.Z, 1 }).XYZ;
        const HMM_Vec3 ray_dir = (HMM_Norm(Minv * HMM_Vec4{ ray.dir.X, ray.dir.Y, ray.dir.Z, 0 })).XYZ;

        g.click_result = ray_vs_world(g, ray_pos, ray_dir);
    }
    if (!g.stale() && g.control_state != ControlState::Dragging && g.clicked) {
        g.clicked = false;
        auto result = g.click_result;

        g.control_state = ControlState::Normal;
        if (!result.hit || result.map.hit_vertex < 0) {
            g.drag_map_mesh = nullptr;
            g.drag_map_buffer = -1;
            g.drag_map_vertex = -1;
            for (MAP_Geometry_Buffer& buf : g.map_buffers) {
                buf.selected = false;
            }
        }

        if (!result.hit || result.cld.hit_vertex < 0) {
            g.drag_cld_group = -1;
            g.drag_cld_face = -1;
            g.drag_cld_vertex = -1;
        }
        if (!result.hit || result.cld.hit_face_index < 0) {
            g.select_cld_group = -1;
            g.select_cld_face = -1;
        }

        if (result.hit) {
            if (result.map.hit_mesh) {
                assert(result.map.hit_vertex_buffer >= 0);
                assert(result.map.hit_vertex_buffer < 4);

                if (result.map.hit_vertex >= 0) {
                    assert(result.map.hit_face < 0);

                    g.widget_original_pos = result.hit_widget_pos;
                    g.control_state = ControlState::Dragging;
                    g.drag_map_mesh = result.map.hit_mesh;
                    g.drag_map_buffer = result.map.hit_vertex_buffer;
                    g.drag_map_vertex = result.map.hit_vertex;

                    Log("Hit MAP vertex: Mesh %p, vertex buffer %d, vertex %d", result.map.hit_mesh, result.map.hit_vertex_buffer, result.map.hit_vertex);
                } else {
                    assert(result.map.hit_face >= 0);

                    for (MAP_Geometry_Buffer& buf : g.map_buffers) {
                        if (buf.mesh_ptr == result.map.hit_mesh) {
                            buf.selected = true;
                        }
                    }

                    Log("Hit MAP face: Mesh %p, vertex buffer %d, face %d", result.map.hit_mesh, result.map.hit_vertex_buffer, result.map.hit_face);
                }
            } else {
                for (MAP_Geometry_Buffer& buf : g.map_buffers) {
                    buf.selected = false;
                }

                assert(result.cld.hit_group >= 0);
                assert(result.cld.hit_group < 4);
                assert(result.cld.hit_face_index >= 0);
                g.select_cld_group = result.cld.hit_group;
                g.select_cld_face = result.cld.hit_face_index;

                if (result.cld.hit_vertex >= 0) {
                    g.widget_original_pos = result.hit_widget_pos;
                    assert(result.cld.hit_vertex < 4);

                    g.control_state = ControlState::Dragging;
                    g.drag_cld_group = result.cld.hit_group;
                    g.drag_cld_face = result.cld.hit_face_index;
                    g.drag_cld_vertex = result.cld.hit_vertex;

                    Log("Hit CLD vertex: Group %d, Face %d, vertex %d", result.cld.hit_group, result.cld.hit_face_index, result.cld.hit_vertex);
                }
            }
        }
    }

    auto make_history_entry = [&] (uint64_t map_hash) -> Map_History_Entry {
        ProfileFunction();

        Map_History_Entry entry = {};

        entry.hash = map_hash;

        entry.count = The_Arena_Allocator::arena_head;
        entry.bytes_used = The_Arena_Allocator::bytes_used;

        entry.data = (char *)malloc(entry.count);
        assert(entry.data);

        no_asan_memcpy(entry.data, The_Arena_Allocator::arena_data, entry.count);

        return entry;
    };

    auto apply_history_entry = [&] (Map_History_Entry entry) {
        assert(entry.count <= The_Arena_Allocator::ARENA_SIZE);
        The_Arena_Allocator::arena_head = entry.count;
        The_Arena_Allocator::bytes_used = entry.bytes_used;
        no_asan_memcpy(The_Arena_Allocator::arena_data, entry.data, The_Arena_Allocator::arena_head);
    };

    uint64_t map_hash = meow_hash(The_Arena_Allocator::arena_data, (int)The_Arena_Allocator::arena_head);
    if (g.undo_stack.count < 1 ||
        (g.control_state != ControlState::Dragging &&
         !ImGui::GetIO().WantCaptureKeyboard &&
         map_hash != g.undo_stack[g.undo_stack.count - 1].hash)) {
        Log("Undo/redo frame! Hash: %llu", map_hash);

        g.undo_stack.push(make_history_entry(map_hash));

        for (auto &entry : g.redo_stack) {
            entry.release();
        }
        g.redo_stack.clear();

        g.staleify_map();
    }

    if (g.control_z) {
        g.control_z = false;
        if (g.undo_stack.count > 1) {
            g.redo_stack.push(g.undo_stack.pop());
            apply_history_entry(g.undo_stack[g.undo_stack.count - 1]);
            g.staleify_map();
        }
    }
    if (g.control_y) {
        g.control_y = false;
        if (g.redo_stack.count > 0) {
            g.undo_stack.push(g.redo_stack.pop());
            apply_history_entry(g.undo_stack[g.undo_stack.count - 1]);
            g.staleify_map();
        }
    }

#ifndef NDEBUG
    if (The_Arena_Allocator::allocations_this_frame != 0) {
        Log("%d allocations this frame", The_Arena_Allocator::allocations_this_frame);
        Log("The_Arena_Allocator::arena_head is %llu", The_Arena_Allocator::arena_head);
        The_Arena_Allocator::allocations_this_frame = 0;
    }
#endif

    if (g.cld_must_update) {
        bool can_update = true;
        for (auto &buf : g.cld_face_buffers) {
            auto info = sg_query_buffer_info(buf.buf);
            uint64_t frame_count = sapp_frame_count();
            if (info.update_frame_index >= frame_count) {
                can_update = false;
                break;
            }
        }
        if (can_update) {
            cld_upload(g);
            g.cld_must_update = false;
        }
    }

    // @Note: We delay texture destruction until after imgui is done drawing images,
    //        so we can staleify and reupload maps on this frame while still letting
    //        imgui display any old images from earlier in the frame that may or may
    //        not have been destroyed. -p 2023-03-29
    Array<MAP_Texture_Buffer> textures_to_destroy = {};
    defer {
        for (auto &tex : textures_to_destroy) {
            tex.release();
        }
        textures_to_destroy.release();
    };

    if (g.map_must_update) {
        bool can_update = true;
        for (int i = 0; i < g.map_buffers_count; i++) {
            auto &buf = g.map_buffers[i];
            auto info = sg_query_buffer_info(buf.vertex_buffer);
            uint64_t frame_count = sapp_frame_count();
            if (info.update_frame_index >= frame_count) {
                can_update = false;
                break;
            }
        }
        if (can_update) {
            {
                textures_to_destroy = g.map_textures;
                g.map_textures = {};
            }
            map_upload(g);
            g.map_must_update = false;
        }
    }
    {
        sg_pass_action p = {};
        p.colors[0].action = SG_ACTION_CLEAR;
        p.colors[0].value = { 0.0f, 0.0f, 0.0f, 1.0f };
        p.depth.action = SG_ACTION_CLEAR;
        p.depth.value = 0;
        sg_begin_default_pass(p, sapp_width(), sapp_height());
        // ImDrawCmd cmd = {};
        // cmd.UserCallbackData = &g;
        // viewport_callback(nullptr, &cmd);
        simgui_render();
        sg_end_pass();
        sg_commit();
    }
}
static void viewport_callback(const ImDrawList* dl, const ImDrawCmd* cmd) {
    ProfileFunction();

    (void)dl;
    G &g = *(G *)cmd->UserCallbackData;
    // first set the viewport rectangle to render in, same as
    // the ImGui draw command's clip rect
    const int cx = (int) cmd->ClipRect.x;
    const int cy = (int) cmd->ClipRect.y;
    const int cw = (int) (cmd->ClipRect.z - cmd->ClipRect.x);
    const int ch = (int) (cmd->ClipRect.w - cmd->ClipRect.y);
    sg_apply_scissor_rect((int)(cx * sapp_dpi_scale()),
                          (int)(cy * sapp_dpi_scale()),
                          (int)(cw * sapp_dpi_scale()),
                          (int)(ch * sapp_dpi_scale()), true);
    sg_apply_viewport((int)(cx * sapp_dpi_scale()),
                      (int)(cy * sapp_dpi_scale()),
                      (int)(cw * sapp_dpi_scale()),
                      (int)(ch * sapp_dpi_scale()), true);
    g.view_x = (float)cx * sapp_dpi_scale();
    g.view_y = (float)cy * sapp_dpi_scale();
    g.view_w = (float)cw * sapp_dpi_scale();
    g.view_h = (float)ch * sapp_dpi_scale();
    {
        HMM_Mat4 perspective = {};
        HMM_Mat4 V = HMM_Transpose(camera_rot(g)) * HMM_Translate(-g.cam_pos);
        HMM_Mat4 R = camera_rot(g);
        auto draw_highlight_vertex_circle = [&g, &perspective, &V, &R] (float (&vertex_floats)[3], float scale_factor, float alpha) {
            HMM_Mat4 P = perspective;

            highlight_vertex_circle_vs_params_t params = {};
            params.in_color = HMM_Vec4 { 1, 1, 1, alpha };

            HMM_Vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
            HMM_Vec3 offset = -g.cld_origin() + vertex;
            offset.X *= SCALE;
            offset.Y *= -SCALE;
            offset.Z *= -SCALE;
            HMM_Mat4 T = HMM_Translate(offset);
            float scale = scale_factor * widget_radius(g, offset);
            HMM_Mat4 S = HMM_Scale( { scale, scale, scale });
            HMM_Mat4 M = T * R * S;
            params.MVP = P * V * M;
            sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_highlight_vertex_circle_vs_params, SG_RANGE(params));
            sg_draw(0, 6, 1);
        };
        {
            if (g.fov < FOV_MIN) {
                g.fov = FOV_MIN;
            }
            if (g.fov > FOV_MAX) {
                g.fov = FOV_MAX;
            }
            const float cot_half_fov = 1 / tanf(g.fov / 2);
            const float near_z = 0.01f;
            const float far_z = 1000.0f;
            float aspect_ratio = (float)cw / ch;
            // @Note: This is not the right projection matrix for default OpenGL, because its clip range is [-1,+1].
            //        You can call glClipControl to fix it, except in GLES2 where you need to fudge the matrix. Too bad!
            //perspective = HMM_Mat4 {
            //{
            //    { cot_half_fov / aspect_ratio,            0,                                       0,  0 },
            //    { 0,                           cot_half_fov,                                       0,  0 },
            //    { 0,                                      0,     (near_z + far_z) / (near_z - far_z), -1 },
            //    { 0,                                      0, (2 * near_z * far_z) / (near_z - far_z),  0 },
            //}
            //};
            //perspective = HMM_Mat4 {
            //{
            //    { cot_half_fov / aspect_ratio,            0,                                       0,  0 },
            //    { 0,                           cot_half_fov,                                       0,  0 },
            //    { 0,                                      0,     near_z / (far_z - near_z), -1 },
            //    { 0,                                      0, (far_z * near_z) / (far_z - near_z),  0 },
            //}
            //};
            perspective = HMM_Mat4 {
            {
                { cot_half_fov / aspect_ratio,            0,      0,  0 },
                { 0,                           cot_half_fov,      0,  0 },
                { 0,                                      0,      0, -1 },
                { 0,                                      0, near_z,  0 },
            }
            };
        }
        {
            vs_params_t params;
            params.cam_pos = g.cam_pos;
            params.P = perspective;

            // The view matrix is the inverse of the camera's model matrix (aka the camera's transform matrix).
            // We perform an inversion of the camera rotation by getting the transpose
            // (which equals the inverse in this case since it's just a rotation matrix).
            params.V = HMM_Transpose(camera_rot(g)) * HMM_Translate(-g.cam_pos);

            sg_apply_pipeline(g.cld_pipeline);
            for (int i = 0; i < g.cld_buffers_count; i++) {
                {
                    // I should also ask the community what the coordinate system is :)
                    params.M = HMM_Scale( { 1 * SCALE, -1 * SCALE, -1 * SCALE }) * HMM_Translate(-g.cld_origin());
                }
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE(params));
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = g.cld_face_buffers[i].buf;
                    sg_apply_bindings(b);
                    sg_draw(0, g.cld_face_buffers[i].num_vertices, 1);
                }
            }
        }
        {
            map_vs_params_t vs_params = {};
            map_fs_params_t fs_params = {};
            fs_params.do_a2c_sharpening = true;
            vs_params.cam_pos = g.cam_pos;
            vs_params.P = perspective;
            vs_params.V = HMM_Transpose(camera_rot(g)) * HMM_Translate(-g.cam_pos);

            for (int i = 0; i < g.map_buffers_count; i++) {
                MAP_Geometry_Buffer &buf = g.map_buffers[i];
                if (!buf.shown) continue;
                int num_times_to_render = buf.selected && !g.wireframe ? 2 : 1;
                sg_pipeline pipelines[2] = {};
                if (buf.source == MAP_Geometry_Buffer_Source::Opaque) {
                    fs_params.do_a2c_sharpening = true;
                    pipelines[0] = (g.cull_backfaces ? g.map_pipeline                   : g.map_pipeline_no_cull);
                    pipelines[1] = (g.cull_backfaces ? g.map_pipeline_no_cull_wireframe : g.map_pipeline_no_cull_wireframe);
                } else {
                    fs_params.do_a2c_sharpening = false;
                    pipelines[0] = (g.cull_backfaces ? g.decal_pipeline                   : g.decal_pipeline_no_cull);
                    pipelines[1] = (g.cull_backfaces ? g.decal_pipeline_no_cull_wireframe : g.decal_pipeline_no_cull_wireframe);
                }
                {
                    vs_params.scaling_factor = { 1, 1, 1 };
                    vs_params.displacement = { 0, 0, 0 };
                    vs_params.overall_center = { 0, 0, 0 };
                    if (buf.selected) {
                        vs_params.displacement = g.displacement;
                        vs_params.scaling_factor = g.scaling_factor;
                        vs_params.overall_center = g.overall_center;
                    }
                    // I should also ask the community what the coordinate system is :)
                    vs_params.M = HMM_Scale({ 1 * SCALE, -1 * SCALE, -1 * SCALE }) * HMM_Translate({});
                }
                for (int render_time = 0; render_time < num_times_to_render; ++render_time) {
                    int is_wireframe = buf.selected && !g.wireframe ? render_time : (int)g.wireframe;
                    sg_apply_pipeline(pipelines[is_wireframe]);
                    fs_params.textured = g.textured;
                    fs_params.use_colours = g.use_lighting_colours;
                    fs_params.shaded = !g.use_lighting_colours;
                    fs_params.highlight_amount = 0;
                    if (is_wireframe) {
                        fs_params.textured = false;
                        fs_params.use_colours = false;
                        fs_params.shaded = false;
                        if (buf.selected) {
                            fs_params.highlight_amount = (float)sin(g.t * TAU * 1.0f) * 0.5f + 0.5f;
                        }
                    }
                    MAP_Mesh &mesh = *buf.mesh_ptr;
                    int indices_start = 0;
                    int mesh_part_group_index = 0;
                    for (MAP_Mesh_Part_Group &mpg : mesh.mesh_part_groups) {
                        defer { mesh_part_group_index++; };
                        MAP_Texture_Buffer tex = g.missing_texture;
                        assert(mpg.material_index >= 0);
                        assert(mpg.material_index < 65536);
                        int num_indices = buf.vertices_per_mesh_part_group[mesh_part_group_index];
                        if (g.solo_material < 0 || g.solo_material == (int)mpg.material_index) {
                            fs_params.material_diffuse_color_bgra = HMM_V4(1, 1, 1, 1);
                            MAP_Material *material = g.materials.at_index(mpg.material_index);
                            if (material) {
                                assert(material->texture_id >= 0);
                                assert(material->texture_id < 65536);
                                auto map_tex = map_get_texture_buffer_by_id(g, material->texture_id);
                                if (map_tex) {
                                    assert(map_tex->tex.id);
                                    tex = *map_tex;
                                }
                                if (!is_wireframe && g.use_material_colours && material->mode != 0x4) { // @Todo: which modes use diffuse colours/etc.?????
                                    fs_params.material_diffuse_color_bgra = PH2MAP_u32_to_bgra(material->diffuse_color);
                                }
                            }
                            sg_bindings b = {};
                            b.vertex_buffers[0] = buf.vertex_buffer;
                            b.index_buffer = buf.index_buffer;
                            b.fs_images[0] = tex.tex;
                            sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_map_vs_params, SG_RANGE(vs_params));
                            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_map_fs_params, SG_RANGE(fs_params));
                            sg_apply_bindings(b);
                            sg_draw(indices_start, (int)num_indices, 1);
                        }
                        indices_start += num_indices;
                    }
                }
            }
        }
        if (g.cld.valid) {
            sg_apply_pipeline(g.highlight_vertex_circle_pipeline);
            sg_bindings b = {};
            b.vertex_buffers[0] = g.highlight_vertex_circle_buffer;
            {
                ProfileScope("sg_apply_bindings");
                sg_apply_bindings(b);
            }
            float screen_height = sapp_heightf();
            for (int group = 0; group < 4; group++) {
                if (group != g.select_cld_group) {
                    continue;
                }

                PH2CLD_Face *faces = g.cld.group_0_faces;
                size_t num_faces = g.cld.group_0_faces_count;
                if (group == 1) { faces = g.cld.group_1_faces; num_faces = g.cld.group_1_faces_count; }
                if (group == 2) { faces = g.cld.group_2_faces; num_faces = g.cld.group_2_faces_count; }
                if (group == 3) { faces = g.cld.group_3_faces; num_faces = g.cld.group_3_faces_count; }
                
                for (int face_index = 0; face_index < num_faces; face_index++) {
                    if (face_index != g.select_cld_face) {
                        continue;
                    }

                    PH2CLD_Face *face = &faces[face_index];
                    int vertices_to_render = 3;
                    if (face->quad) {
                        vertices_to_render = 4;
                    }
                    for (int i = 0; i < vertices_to_render; i++) {
                        float scale_factor = 1;
                        float alpha = 1;
                        if (g.control_state != ControlState::Normal) {
                            alpha = 0.7f;
                        }
                        if (g.control_state == ControlState::Dragging) {
                            if (group == g.drag_cld_group && face_index == g.drag_cld_face && i == g.drag_cld_vertex) {
                                alpha = 1;
                                scale_factor *= 0.5f;
                            } else {
                                alpha = 0.3f;
                            }
                        }
                        draw_highlight_vertex_circle(face->vertices[i], scale_factor, alpha);
                    }
                }
            }
#if (0)
            { // @Debug
                HMM_Vec3 offset = g.click_ray.pos + g.click_ray.dir;
                HMM_Mat4 T = HMM_Translate(offset);
                float scale = 1; //HMM_Len(g.cam_pos - offset) * widget_pixel_radius / screen_height;
                HMM_Mat4 S = HMM_Scale({scale, scale, scale});
                HMM_Mat4 M = T * R * S;
                params.MVP = P * V * M;
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_highlight_vertex_circle_vs_params, SG_RANGE(params));
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = g.highlight_vertex_circle_buffer;
                    sg_apply_bindings(b);
                    sg_draw(0, 6, 1);
                }
            }
#endif
        }
        if (g.control_state == ControlState::Normal) {
            sg_apply_pipeline(g.highlight_vertex_circle_pipeline);
            sg_bindings b = {};
            b.vertex_buffers[0] = g.highlight_vertex_circle_buffer;
            {
                ProfileScope("sg_apply_bindings");
                sg_apply_bindings(b);
            }

            const Ray mouse_ray = screen_to_ray(g, { g.mouse_x, g.mouse_y });
            const HMM_Vec3 origin = -g.cld_origin();
            const HMM_Mat4 Tinv = HMM_Translate(-origin);
            const HMM_Mat4 Sinv = HMM_Scale( { 1 / SCALE, 1 / -SCALE, 1 / -SCALE });
            const HMM_Mat4 Minv = Tinv * Sinv;
            const HMM_Vec3 ray_pos = (Minv * HMM_Vec4{ mouse_ray.pos.X, mouse_ray.pos.Y, mouse_ray.pos.Z, 1 }).XYZ;
            const HMM_Vec3 ray_dir = (HMM_Norm(Minv * HMM_Vec4{ mouse_ray.dir.X, mouse_ray.dir.Y, mouse_ray.dir.Z, 0 })).XYZ;

            Ray_Vs_World_Result result = g.click_result;

            if (!g.stale() && result.hit && result.map.hit_mesh) {
                if (result.map.hit_vertex >= 0) {
                    float vertex[3] = { result.hit_widget_pos.X, result.hit_widget_pos.Y, result.hit_widget_pos.Z };
                    draw_highlight_vertex_circle(vertex, 1, 0.7f);
                    // Log("Hit MAP vertex: Mesh %p, vertex buffer %d, vertex %d", result.hit_mesh, result.hit_vertex_buffer, result.hit_vertex);
                } else {
                    assert(result.map.hit_face >= 0);

                    for (auto& buf : g.map_buffers) { // @Lazy
                        if (buf.mesh_ptr != result.map.hit_mesh) continue;

                        float (&a)[3] = *(float (*)[3])&buf.vertices.data[buf.indices.data[result.map.hit_face * 3 + 0]];
                        float (&b)[3] = *(float (*)[3])&buf.vertices.data[buf.indices.data[result.map.hit_face * 3 + 1]];
                        float (&c)[3] = *(float (*)[3])&buf.vertices.data[buf.indices.data[result.map.hit_face * 3 + 2]];

                        draw_highlight_vertex_circle(a, 0.5f, 0.75f);
                        draw_highlight_vertex_circle(b, 0.5f, 0.75f);
                        draw_highlight_vertex_circle(c, 0.5f, 0.75f);

                        break;
                    }
                }
            }
        }
        
    }
}

static void cleanup(void *userdata) {
#ifndef NDEBUG
    {
        ProfileFunction();

        G &g = *(G *)userdata;
        g.release();
        simgui_shutdown();
        sg_shutdown();
        The_Arena_Allocator::quit();
        stb_leakcheck_dumpmem();
    }
#endif

    spall_buffer_flush(&spall_ctx, &spall_buffer);
    spall_buffer_quit(&spall_ctx, &spall_buffer);
    spall_flush(&spall_ctx);
    spall_quit(&spall_ctx);
}

G g_ = {};
sapp_desc sokol_main(int, char **) {
    sapp_desc d = {};
    d.user_data = &g_;
    d.init_userdata_cb = init;
    d.event_userdata_cb = event;
    d.frame_userdata_cb = frame;
    d.cleanup_userdata_cb = cleanup;
    d.sample_count = 8;
    d.swap_interval = 0;
    d.high_dpi = true;
#ifndef NDEBUG
    d.win32_console_create = true;
    d.win32_console_utf8 = true;
#endif
    d.html5_ask_leave_site = true;
    return d;
}
