// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

#define _CRT_SECURE_NO_WARNINGS
#define _NO_CRT_STDIO_INLINE

#ifndef defer
struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

#define countof(x) (sizeof(x) / sizeof(*(x)))

// With editor widgets, you should probably be able to:
//  - Box-select a group of vertices or edges
//      - Drag that selection as a group in any direction (with or without axis- and plane-alignment)
//      - Rotate that selection around its center of geometry (with or without plane-alignment)
//      - Scale that selection (with or without axis- and plane-alignment)
//      - Delete that selection and gracefully handle the results of that deletion (removing degenerate faces etc.)
//      - (Remember: these all need to include cylinders somehow!)
//      - Render the AABB of all selected things
//  - View any surface as a solid/shaded set of triangles with a wireframe overlaid
//  - View any surface as only solid, only shaded, only wireframe, only vertex colours etc.
//  - Lesson learned from Happenlance editor: Definitely need a base "Transform" struct so that you aren't
//    reimplementing the same logic for N different "widget kinds" (in happenlance editor this was
//    a huge pain because objects, sprites, particle emitters etc. all had scattered transform data.
//    Unity Engine and friends absolutely make the right decision to have Transforms be fundamental bases to all
//    editable things!)

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#include <Windows.h>

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
#define TAU 6.283185307179586476925
#define TAU32 6.283185307179586476925f

#define IM_ASSERT assert
#define SOKOL_ASSERT assert

// Dear Imgui
#include "imgui.h"

// Sokol libraries
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"

#include <stdarg.h>
#include <stdio.h>

struct LogMsg {
    unsigned int colour = IM_COL32(127,127,127,255);
    char buf[252] = {};
};
enum { LOG_MAX = 16384 };
LogMsg log_buf[LOG_MAX] = {};
int log_buf_index = 0;
void LogC(uint32_t c, const char *fmt, ...) {
    log_buf[log_buf_index % LOG_MAX].colour = c;
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_buf[log_buf_index % LOG_MAX].buf, sizeof(log_buf[0].buf), fmt, args);
    va_end(args);
    log_buf_index++;
}
#define LogC(c, fmt, ...) LogC(c, fmt, ##__VA_ARGS__)
#define Log(fmt, ...) LogC(IM_COL32(127,127,127,255), fmt, ##__VA_ARGS__)

#include "HandmadeMath.h"

#define PH2CLD_IMPLEMENTATION
#include "../cld/ph2_cld.h"
#include <io.h> // @Debug @Temporary @Remove

#include "shaders.glsl.h"

sg_context_desc sapp_sgcontext(void);

#ifdef _WIN32
#pragma warning(disable : 4255)
#pragma warning(disable : 4668)
static double get_time(void) {
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
static double get_time(void) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}
#endif

// How's this done for Linux/etc?
#define KEY(vk) ((unsigned short)GetAsyncKeyState(vk) >= 0x8000)

struct Ray {
    Ray() = default;
    Ray(hmm_vec3 pos, hmm_vec3 dir) : pos{pos}, dir{dir} {}
    hmm_vec3 pos = {};
    hmm_vec3 dir = {};
};

const float FOV_MIN = 10 * (TAU32 / 360);
const float FOV_MAX = 179 * (TAU32 / 360);
const float FOV_DEFAULT = 90 * (TAU32 / 360);
const float MOVE_SPEED_DEFAULT = -2;
const float MOVE_SPEED_MAX = 6;
struct CLD_Face_Buffer {
    sg_buffer buf = {};
    int num_vertices = 0;
};

struct MAP_Geometry_Vertex {
    float position[3] = {};
    float normal[3] = {};
    uint32_t color = 0xffffffff;
    float uv[2] = {};
};
struct MAP_Geometry_Buffer {
    sg_buffer buf = {};
    int num_vertices = 0;
    uint32_t id = 0;
    bool shown = true;
};
enum struct ControlState {
    Normal,
    Orbiting,
    Dragging,
};
struct G {
    double last_time = 0;
    double t = 0;

    ControlState control_state = {};
    float fov = FOV_DEFAULT;

    hmm_vec3 cam_pos = {0, 0, 0};
    float yaw = 0;
    float pitch = 0;
    float move_speed = MOVE_SPEED_DEFAULT;
    float scroll_speed = MOVE_SPEED_DEFAULT;
    float scroll_speed_timer = 0;

    Ray click_ray = {};
    hmm_vec3 widget_original_pos = {};
    int select_cld_group = -1;
    int select_cld_face = -1;
    int drag_cld_group = -1;
    int drag_cld_face = -1;
    int drag_cld_vertex = -1;
    
    PH2CLD_Collision_Data cld = {};
    hmm_vec3 cld_origin = {};
    sg_pipeline cld_pipeline = {};
    enum { cld_buffers_count = 4 };
    CLD_Face_Buffer cld_face_buffers[cld_buffers_count] = {};

    sg_pipeline map_pipeline = {};
    enum { map_buffers_max = 16 };
    MAP_Geometry_Buffer map_buffers[map_buffers_max] = {};
    int map_buffers_count = 0;

    sg_buffer highlight_vertex_circle_buffer = {};
    sg_pipeline highlight_vertex_circle_pipeline = {};

    sg_image textures[65536] = {};
    int texture_ui_selected = 0;

    bool cld_must_update = false;
    bool subgroup_visible[16] = {true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};
};
static void cld_upload(G &g) {
    auto &cld = g.cld;
    if (!cld.valid) {
        g.cld_origin = {};
        for (auto &buf : g.cld_face_buffers) {
            buf.num_vertices = 0;
        }
        return;
    }
    //Log("CLD origin is (%f, 0, %f)", cld.origin[0], cld.origin[1]);
    g.cld_origin = hmm_vec3 { cld.origin[0], 0, cld.origin[1] };
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
    Log("CLD filename is \"%s\"", filename);
    PH2CLD_free_collision_data(g.cld);
    g.cld = PH2CLD_get_collision_data_from_file(filename);
    if (!g.cld.valid) {
        LogC(IM_COL32(255, 127, 127, 255), "Failed loading CLD file \"%s\"!", filename);
    }
    cld_upload(g);
    g.cld_must_update = true;
}
static void map_load(G &g, const char *filename) {
    //for (int i = 0; i < g.map_buffers_count; i++) {
        //sg_destroy_buffer(g.map_buffers[i].buf);
        //g.map_buffers[i] = {};
    //}
    g.map_buffers_count = 0;
    for (int i = 0; i < countof(g.textures); i++) {
        if (g.textures[i].id) {
            sg_destroy_image(g.textures[i]);
            g.textures[i].id = 0;
        }
    }
    g.texture_ui_selected = 0;
    {
        {
            FILE *f = PH2CLD__fopen(filename, "rb");
            if (!f) {
                LogC(IM_COL32(255, 127, 127, 255), "Failed loading MAP file \"%s\"!", filename);
                return;
            }
            assert(f);
            defer {
                fclose(f);
            };

            // @Temporary @Debug
            static char filedata[16 * 1024 * 1024];
            uint32_t file_len = (uint32_t)fread(filedata, 1, sizeof(filedata), f);
            struct PH2MAP__Header {
                uint32_t magic; // should be 0x20010510
                uint32_t file_length;
                uint32_t subfile_count;
                uint32_t padding0;
            };
            char *ptr = filedata;
            char *end = filedata + file_len;
#define Read(ptr, x) (assert(ptr + sizeof(x) <= end), memcpy(&(x), ptr, sizeof(x)) && (ptr += sizeof(x)))
            PH2MAP__Header header = {};
            Read(ptr, header);
            assert(header.magic == 0x20010510);
            assert(header.file_length == file_len);
            assert(header.padding0 == 0);
            for (uint32_t subfile_index = 0; subfile_index < header.subfile_count; subfile_index++) {
                struct PH2MAP__Subfile_Header {
                    uint32_t type; // 1 == Geometry; 2 == Texture
                    uint32_t length;
                    uint32_t padding0;
                    uint32_t padding1;
                };
                PH2MAP__Subfile_Header subfile_header = {};
                Read(ptr, subfile_header);
                assert(subfile_header.type == 1 || subfile_header.type == 2);
                assert(subfile_header.padding0 == 0);
                assert(subfile_header.padding1 == 0);
                if (subfile_header.type == 1) { // Geometry subfile
                    auto ptr2 = ptr;

                    assert(ptr + subfile_header.length <= end);
                    ptr += subfile_header.length;
                    
                    struct PH2MAP__Geometry_Subfile_Header {
                        uint32_t magic; // should be 0x20010730
                        uint32_t geometry_count;
                        uint32_t geometry_size;
                        uint32_t material_count;
                    };
                    PH2MAP__Geometry_Subfile_Header geometry_subfile_header = {};
                    Read(ptr2, geometry_subfile_header);
                    assert(geometry_subfile_header.magic == 0x20010730);
                    assert(geometry_subfile_header.geometry_count >= 1);
                    for (uint32_t geometry_index = 0; geometry_index < geometry_subfile_header.geometry_count; geometry_index++) {
                        struct PH2MAP__Geometry_Header {
                            uint32_t id;
                            int32_t group_size;
                            int32_t opaque_group_offset;
                            int32_t transparent_group_offset;
                            int32_t decal_group_offset;
                        };
                        PH2MAP__Geometry_Header geometry_header = {};
                        Read(ptr2, geometry_header);
                        assert(geometry_header.group_size > 0);
                        assert(geometry_header.group_size < 2 * 1024 * 1024);
                        assert(geometry_header.opaque_group_offset >= 0);
                        assert(geometry_header.transparent_group_offset >= 0);
                        assert(geometry_header.decal_group_offset >= 0);
                        assert(geometry_header.opaque_group_offset < geometry_header.group_size);
                        assert(geometry_header.transparent_group_offset < geometry_header.group_size);
                        assert(geometry_header.decal_group_offset < geometry_header.group_size);
                        Log("%d", geometry_header.id);

                        static MAP_Geometry_Vertex vertices_buffer[320000];
                        enum { vertices_max = sizeof(vertices_buffer) / sizeof(vertices_buffer[0]) };
                        int vertices_count = 0;
                        
                        ptr2 -= sizeof(geometry_header);

                        char *end2 = ptr2 + geometry_header.group_size;
                        if (geometry_header.opaque_group_offset) {
                            char *mesh_group_header = ptr2 + geometry_header.opaque_group_offset;
                            char *ptr3 = mesh_group_header;
                            uint32_t map_mesh_count = 0;
                            Read(ptr3, map_mesh_count);
                            for (uint32_t offset_index = 0; offset_index < map_mesh_count; offset_index++) {
                                int32_t map_mesh_offset = 0;
                                Read(ptr3, map_mesh_offset);
                                assert(map_mesh_offset > 0);

                                char *mapmesh_header_base = mesh_group_header + map_mesh_offset;
                                char *ptr4 = mapmesh_header_base;
                                struct PH2MAP__Mapmesh_Header {
                                    float bounding_box_a[4];
                                    float bounding_box_b[4];
                                    int32_t vertex_sections_header_offset;
                                    int32_t indices_offset;
                                    int32_t indices_length;
                                    int32_t unknown;
                                    int32_t mesh_part_group_count;
                                };
                                uint16_t indices[77 * 1024];
                                int indices_count = 0;
                                PH2MAP__Mapmesh_Header mapmesh_header = {};
                                Read(ptr4, mapmesh_header);
                                assert(PH2CLD__sanity_check_float4(mapmesh_header.bounding_box_a));
                                assert(PH2CLD__sanity_check_float4(mapmesh_header.bounding_box_b));
                                assert(mapmesh_header.bounding_box_a[0] <= mapmesh_header.bounding_box_b[0]);
                                assert(mapmesh_header.bounding_box_a[1] <= mapmesh_header.bounding_box_b[1]);
                                assert(mapmesh_header.bounding_box_a[2] <= mapmesh_header.bounding_box_b[2]);
                                assert(mapmesh_header.bounding_box_a[3] == 0);
                                assert(mapmesh_header.bounding_box_b[3] == 0);
                                assert(mapmesh_header.vertex_sections_header_offset >= 0);
                                assert(mapmesh_header.indices_offset >= 0);
                                assert(mapmesh_header.indices_length >= 0);
                                assert(mapmesh_header.indices_length % sizeof(uint16_t) == 0);
                                assert(mapmesh_header_base + mapmesh_header.indices_offset + mapmesh_header.indices_length <= end2);
                                
                                ptr4 = mapmesh_header_base + mapmesh_header.vertex_sections_header_offset;
                                struct PH2MAP__Vertex_Sections_Header {
                                    int32_t vertices_length;
                                    int32_t vertex_section_count;
                                };
                                PH2MAP__Vertex_Sections_Header vertex_sections_header = {};
                                Read(ptr4, vertex_sections_header);
                                assert(vertex_sections_header.vertices_length >= 0);
                                assert(vertex_sections_header.vertex_section_count >= 0);
                                assert(vertex_sections_header.vertex_section_count <= 4);
                                int vertex_sizes[4] = {};
                                char *vertex_buffers[4] = {};
                                int vertex_buffer_counts[4] = {};
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
                                char *ptr5 = ptr4 + vertex_sections_header.vertex_section_count * sizeof(PH2MAP__Vertex_Section_Header);
                                char *end_of_previous_section = ptr5;
                                for (int32_t vertex_section_index = 0; vertex_section_index < vertex_sections_header.vertex_section_count; vertex_section_index++) {
                                    PH2MAP__Vertex_Section_Header vertex_section_header = {};
                                    Read(ptr4, vertex_section_header);

                                    // @Note: How does section_starts work?
                                    //assert(vertex_section_header.section_starts == 0);

                                    char *vertex_section_offset_base = ptr4;

                                    assert(vertex_section_header.section_starts >= 0);
                                    assert(vertex_section_header.bytes_per_vertex == 0x14 ||
                                           vertex_section_header.bytes_per_vertex == 0x18 ||
                                           vertex_section_header.bytes_per_vertex == 0x20 ||
                                           vertex_section_header.bytes_per_vertex == 0x24);
                                    assert(vertex_section_header.section_length >= 0);
                                    assert(vertex_section_header.section_length % vertex_section_header.bytes_per_vertex == 0);

                                    vertex_sizes[vertex_section_index] = vertex_section_header.bytes_per_vertex;
                                    for (int j = 0; j < vertex_section_index; j++) {
                                        assert(vertex_sizes[j] != vertex_sizes[vertex_section_index]);
                                    }

                                    int num_vertices = vertex_section_header.section_length / vertex_section_header.bytes_per_vertex;

                                    assert(ptr5 == end_of_previous_section);
                                    char *end_of_this_section = ptr5 + vertex_section_header.section_length;
                                    vertex_buffers[vertex_section_index] = ptr5;
                                    vertex_buffer_counts[vertex_section_index] = num_vertices;
                                    for (int i = 0; i < num_vertices; i++) {
                                        switch (vertex_section_header.bytes_per_vertex) {
                                            case 0x14: {
                                                PH2MAP__Vertex14 vert = {};
                                                Read(ptr5, vert);
                                                assert(PH2CLD__sanity_check_float3(vert.position));
                                                assert(PH2CLD__sanity_check_float2(vert.uv));
                                                assert(vert.uv[0] > -1);
                                                assert(vert.uv[0] < +2);
                                                assert(vert.uv[1] > -1);
                                                assert(vert.uv[1] < +2);
                                            } break;
                                            case 0x18: {
                                                PH2MAP__Vertex18 vert = {};
                                                Read(ptr5, vert);
                                                assert(PH2CLD__sanity_check_float3(vert.position));
                                                assert(PH2CLD__sanity_check_float2(vert.uv));
                                                assert(vert.uv[0] > -1);
                                                assert(vert.uv[0] < +2);
                                                assert(vert.uv[1] > -1);
                                                assert(vert.uv[1] < +2);
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
                                                assert(vert.uv[0] < +2);
                                                assert(vert.uv[1] > -1);
                                                assert(vert.uv[1] < +2);
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
                                                assert(vert.uv[0] < +2);
                                                assert(vert.uv[1] > -1);
                                                assert(vert.uv[1] < +2);
                                            } break;
                                        };
                                    }
                                    assert(ptr5 == end_of_this_section);
                                    end_of_previous_section = end_of_this_section;
                                }
                                assert(end_of_previous_section == mapmesh_header_base + mapmesh_header.indices_offset);
                                ptr4 = end_of_previous_section;
                                for (int indices_index = 0; indices_index < mapmesh_header.indices_length / sizeof(uint16_t); indices_index++) {
                                    uint16_t index = 0;
                                    Read(ptr4, index);
                                    indices[indices_count++] = index;
                                }
                                ptr4 = mapmesh_header_base + sizeof(PH2MAP__Mapmesh_Header);
                                int indices_index = 0;
                                for (int mesh_part_group_index = 0; mesh_part_group_index < mapmesh_header.mesh_part_group_count; mesh_part_group_index++) {
                                    struct PH2MAP__Mesh_Part_Group_Header {
                                        uint32_t material_index;
                                        uint32_t section_index;
                                        uint32_t mesh_part_count;
                                    };
                                    PH2MAP__Mesh_Part_Group_Header mesh_part_group_header = {};
                                    Read(ptr4, mesh_part_group_header);
                                    // assert(mesh_part_group_header.material_index >= );
                                    // assert(mesh_part_group_header.material_index <= );
                                    assert(mesh_part_group_header.section_index < 4);

                                    for (uint32_t mesh_part_index = 0; mesh_part_index < mesh_part_group_header.mesh_part_count; mesh_part_index++) {
                                        struct PH2MAP__Mesh_Part {
                                            uint16_t strip_length;
                                            uint8_t invert_reading;
                                            uint8_t strip_count;
                                            uint16_t first_vertex;
                                            uint16_t last_vertex;
                                        };
                                        PH2MAP__Mesh_Part mesh_part = {};
                                        Read(ptr4, mesh_part);
                                        int vertex_size = vertex_sizes[mesh_part_group_header.section_index];
                                        char *vertex_buffer = vertex_buffers[mesh_part_group_header.section_index];
                                        int vertex_buffer_count = vertex_buffer_counts[mesh_part_group_header.section_index];
                                        // @Note: these assertions are probably not sanity checks, but rather real
                                        //        logical assert()s, because my code is broken if these are zero.
                                        assert(vertex_size);
                                        assert(vertex_buffer);
                                        int outer_max = mesh_part.strip_count;
                                        int inner_max = mesh_part.strip_length;
                                        if (mesh_part.invert_reading) { // Don't know why this is a thing, but whatever. Yuck!
                                            outer_max = mesh_part.strip_length;
                                            inner_max = mesh_part.strip_count;
                                        }
                                        auto get_index = [&] {
                                            assert(indices_index < indices_count);
                                            return indices[indices_index++];
                                        };
                                        for (int strip_index = 0; strip_index < outer_max; strip_index++) {
                                            int memory = get_index() << 0x10;
                                            int mask = 0xFFFF0000;
                                            uint16_t currentIndex = get_index();
                                            for (int i = 2; i < inner_max; i++) {
                                                auto get_vertex = [&] (int index) {
                                                    MAP_Geometry_Vertex result = {};
                                                    char *vertex_ptr = vertex_buffer + index * vertex_size;
                                                    switch (vertex_size) {
                                                        case 0x14: {
                                                            PH2MAP__Vertex14 vert = {};
                                                            Read(vertex_ptr, vert);
                                                            result.position[0] = vert.position[0];
                                                            result.position[1] = vert.position[1];
                                                            result.position[2] = vert.position[2];
                                                            result.uv[0] = vert.uv[0];
                                                            result.uv[1] = vert.uv[1];
                                                        } break;
                                                        case 0x18: {
                                                            PH2MAP__Vertex18 vert = {};
                                                            Read(vertex_ptr, vert);
                                                            result.position[0] = vert.position[0];
                                                            result.position[1] = vert.position[1];
                                                            result.position[2] = vert.position[2];
                                                            result.color = vert.color;
                                                            result.uv[0] = vert.uv[0];
                                                            result.uv[1] = vert.uv[1];
                                                        } break;
                                                        case 0x20: {
                                                            PH2MAP__Vertex20 vert = {};
                                                            Read(vertex_ptr, vert);
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
                                                            PH2MAP__Vertex24 vert = {};
                                                            Read(vertex_ptr, vert);
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
                                                    }
                                                    return result;
                                                };
                                                memory = (memory & mask) + (currentIndex << (0x10 & mask));
                                                mask ^= 0xFFFFFFFF;
                                                
                                                currentIndex = get_index();
                                                
                                                auto triangle_v0 = get_vertex(memory >> 0x10);
                                                auto triangle_v1 = get_vertex(memory & 0xffff);
                                                auto triangle_v2 = get_vertex(currentIndex);
                                                assert(vertices_count + 2 < vertices_max);
                                                vertices_buffer[vertices_count++] = triangle_v0;
                                                vertices_buffer[vertices_count++] = triangle_v1;
                                                vertices_buffer[vertices_count++] = triangle_v2;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        
                        if (vertices_count > 0) {
                            assert(g.map_buffers_count < g.map_buffers_max);
                            auto & map_buffer = g.map_buffers[g.map_buffers_count++];
                            sg_update_buffer(map_buffer.buf, sg_range { vertices_buffer, vertices_count * sizeof(vertices_buffer[0]) });
                            assert(vertices_count % 3 == 0);
                            map_buffer.num_vertices = vertices_count;
                            map_buffer.id = geometry_header.id;
                            //break;
                        }
                        
                        ptr2 += geometry_header.group_size;

                    }
                    for (uint32_t material_index = 0; material_index < geometry_subfile_header.material_count; material_index++) {
                        struct PH2MAP__Material {
                            int16_t mode;
                            int16_t texture_id;
                            uint32_t material_color;
                            uint32_t overlay_color;
                            float specularity;
                        };
                        PH2MAP__Material material = {};
                        Read(ptr2, material);
                        assert(PH2CLD__sanity_check_float(material.specularity));
                    }
                    assert(ptr2 == ptr);
                } else if (subfile_header.type == 2) { // Texture subfile
                    assert(ptr + subfile_header.length <= end);
                    auto end = ptr + subfile_header.length;
                    struct PH2MAP__Texture_Subfile_Header {
                        uint32_t magic;
                        uint32_t pad[2];
                        uint32_t always1;
                    };
                    PH2MAP__Texture_Subfile_Header texture_subfile_header = {};
                    Read(ptr, texture_subfile_header);
                    assert(texture_subfile_header.magic == 0x19990901);
                    assert(texture_subfile_header.pad[0] == 0);
                    assert(texture_subfile_header.pad[1] == 0);
                    assert(texture_subfile_header.always1 == 1);
                    for (;;) {
                        {
                            // "read until the first int of the line is 0, and then skip that line"
                            assert(ptr + 16 <= end);
                            auto ptr2 = ptr;
                            uint32_t line_check = 0;
                            Read(ptr2, line_check);
                            if (line_check == 0) {
                                ptr += 16;
                                break;
                            }
                        }
                        struct PH2MAP__BC_Texture_Header {
                            uint32_t id;
                            uint16_t width;
                            uint16_t height;
                            uint16_t width2;
                            uint16_t height2;
                            uint32_t sprite_count;
                            uint16_t unknown;
                            uint16_t id2;
                            uint32_t pad[3];
                        };
                        PH2MAP__BC_Texture_Header bc_texture_header = {};
                        Read(ptr, bc_texture_header);
                        assert(bc_texture_header.id <= 0xffff);
                        assert(bc_texture_header.width == bc_texture_header.width2);
                        assert(bc_texture_header.height == bc_texture_header.height2);
                        // @Note: the docs say id2 == id, but it looks like sometimes id2 == 1 etc. :(
                        // assert(bc_texture_header.id == bc_texture_header.id2);
                        assert((bc_texture_header.unknown >= 0x1 && bc_texture_header.unknown <= 0x10) ||
                            bc_texture_header.unknown == 0x28);
                        assert(bc_texture_header.pad[0] == 0);
                        assert(bc_texture_header.pad[1] == 0);
                        assert(bc_texture_header.pad[2] == 0);
                        for (size_t sprite_index = 0; sprite_index < bc_texture_header.sprite_count; sprite_index++) {
                            struct PH2MAP__Sprite_Header {
                                uint32_t id;
                                uint16_t x;
                                uint16_t y;
                                uint16_t width;
                                uint16_t height;
                                uint32_t format;
                            };
                            struct PH2MAP__Sprite_Pixel_Header {
                                uint32_t data_length;
                                uint32_t data_length_plus_header;
                                uint32_t pad;
                                uint32_t always0x99000000;
                            };
                            PH2MAP__Sprite_Header sprite_header = {};
                            PH2MAP__Sprite_Pixel_Header pixel_header = {};
                            Read(ptr, sprite_header);
                            Read(ptr, pixel_header);

                            assert(sprite_header.id <= 0xffff);
                            assert(sprite_header.format == 0x100 ||
                                sprite_header.format == 0x102 || 
                                sprite_header.format == 0x103 || 
                                sprite_header.format == 0x104);

                            assert(pixel_header.data_length + sizeof(PH2MAP__Sprite_Pixel_Header) == pixel_header.data_length_plus_header);
                            assert(pixel_header.pad == 0);
                            assert(pixel_header.always0x99000000 == 0x99000000);

                            auto pixels_data = ptr; 
                            size_t pixels_len = pixel_header.data_length;
                            auto pixels_end = pixels_data + pixels_len;
                            if (pixels_len) { // @Temporary
                                // @Temporary
                                assert(g.textures[bc_texture_header.id].id == 0);
                                sg_image_desc d = {};
                                d.width = sprite_header.width;
                                d.height = sprite_header.height;
                                if (sprite_header.format == 0x100 || sprite_header.format == 0x103) {
                                    d.pixel_format = SG_PIXELFORMAT_BC1_RGBA;
                                } else if (sprite_header.format == 0x102 || sprite_header.format == 0x104) {
                                    d.pixel_format = SG_PIXELFORMAT_BC2_RGBA;
                                } else {
                                    assert(false);
                                }
                                d.min_filter = SG_FILTER_LINEAR;
                                d.mag_filter = SG_FILTER_LINEAR;
                                d.data.subimage[0][0] = { pixels_data, pixels_len };
                                g.textures[bc_texture_header.id] = sg_make_image(d);
                                assert(g.textures[bc_texture_header.id].id);
                            }
                            assert(pixels_end <= end);
                            ptr = pixels_end;
                        }
                    }
                    ptr = end; // @Temporary
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
}
static void test_all_maps(G &g) {
    struct _finddata_t find_data;
    intptr_t directory = _findfirst("map/*.map", &find_data);
    assert(directory >= 0);
    while (1) {
        char b[260 + sizeof("map/")];
        snprintf(b, sizeof(b), "map/%s", find_data.name);
        map_load(g, b);
        if (_findnext(directory, &find_data) < 0) {
            if (errno == ENOENT) break;
            else assert(0);
        }
    }
    _findclose(directory);
    fflush(stdout);
}

static void imgui_do_console(G &g) {
    ImGui::SetNextWindowPos(ImVec2 { sapp_width() * 0.66f, sapp_height() * 0.66f }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2 { sapp_width() * 0.32f, sapp_height() * 0.32f }, ImGuiCond_FirstUseEver);
    ImGui::Begin("Console");
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
    if (ImGui::InputText("###console input", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
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
        } else {
            Log("Unkown command :)");
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

struct Ray_Vs_Aligned_Circle_Result {
    bool hit = false;
    float t = 0;
    hmm_vec3 closest_point = {};
    float distance_to_closest_point = {};
};
Ray_Vs_Aligned_Circle_Result ray_vs_aligned_circle(hmm_vec3 ro, hmm_vec3 rd, hmm_vec3 so, float r) {
    Ray_Vs_Aligned_Circle_Result result = {};
    result.t = HMM_Dot(so - ro, rd);
    //Log("Dot %f", result.t);
    result.closest_point = ro + rd * result.t;
    //Log("Closest Point %f, %f, %f", result.closest_point.X, result.closest_point.Y, result.closest_point.Z);
    result.distance_to_closest_point = HMM_Length(result.closest_point - so);
    //Log("Distance %f (out of %f)", result.distance_to_closest_point, r);
    if (result.distance_to_closest_point <= r) {
        result.hit = true;
    }
    return result;
}

static inline float abs(float x) { return x >= 0 ? x : -x; }

struct Ray_Vs_Plane_Result {
    bool hit = false;
    float t = 0;
    hmm_vec3 point = {};
};
Ray_Vs_Plane_Result ray_vs_plane(Ray ray, hmm_vec3 plane_point, hmm_vec3 plane_normal) {
    Ray_Vs_Plane_Result result = {};

    assert(HMM_Length(plane_normal) != 0);
    assert(HMM_Length(ray.dir) != 0);
    plane_normal = HMM_Normalize(plane_normal);
    
    hmm_vec3 displacement = plane_point - ray.pos;
    float distance_to_origin = HMM_Dot(displacement, plane_normal);
    float cos_theta = HMM_Dot(ray.dir, plane_normal);
    if (abs(cos_theta) < 0.0001) {
        result.hit = abs(distance_to_origin) < 0.0001;
        result.t = 0;
    } else {
        result.t = distance_to_origin / cos_theta;
        result.hit = result.t >= 0;
    }
    result.point = ray.pos + ray.dir * result.t;

    return result;
}
Ray_Vs_Plane_Result ray_vs_triangle(Ray ray, hmm_vec3 a, hmm_vec3 b, hmm_vec3 c) {
    hmm_vec3 normal = HMM_Normalize(HMM_Cross(b-a, c-a));
    auto raycast = ray_vs_plane(ray, a, normal);
    hmm_vec3 p = raycast.point;
    hmm_vec3 ba = a - b;
    hmm_vec3 ca = a - c;
    hmm_vec3 bp = p - b;
    hmm_vec3 cp = p - c;
    float beta = 0;
    {
        hmm_vec3 v = ba - HMM_Dot(ba, ca) / HMM_LengthSquared(ca) * ca;
        beta = 1 - HMM_Dot(v, bp) / HMM_Dot(v, ba);
    }
    float gamma = 0;
    {
        hmm_vec3 v = ca - HMM_Dot(ca, ba) / HMM_LengthSquared(ba) * ba;
        gamma = 1 - HMM_Dot(v, cp) / HMM_Dot(v, ca);
    }
    float u = beta;
    float v = gamma;
    raycast.hit = raycast.hit && u >= 0 && v >= 0 && u + v <= 1;
    return raycast;
}

static void init(void *userdata) {
    G &g = *(G *)userdata;
    { // @Temporary @Remove
        auto hwnd = sapp_win32_get_hwnd();
        MoveWindow((HWND)hwnd, -1910, 0, 1900, 1000, false);
    }
    g.last_time = get_time();
    sg_desc desc = {};
    desc.context = sapp_sgcontext();
    sg_setup(&desc);
    simgui_desc_t simgui_desc = {};
    simgui_desc.no_default_font = true;
    simgui_desc.dpi_scale = sapp_dpi_scale();
    simgui_desc.sample_count = sapp_sample_count();
#ifdef NDEBUG
    simgui_desc.ini_filename = "imgui.ini";
#endif
    simgui_setup(&simgui_desc);
    {
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        ImFontConfig fontCfg;
        fontCfg.FontDataOwnedByAtlas = false;
        fontCfg.OversampleH = 2;
        fontCfg.OversampleV = 2;
        fontCfg.RasterizerMultiply = 1.5f;
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", 16, &fontCfg);
        
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
            buffer.buf = sg_make_buffer(d);
        }
    }
    {
        cld_load(g, "../cld/cld/ob01.cld");
        map_load(g, "map/ob01.map");
    }
    if (0) test_all_maps(g);

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
        //d.primitive_type = SG_PRIMITIVETYPE_LINES;
        d.depth.write_enabled = true;
        d.depth.compare = SG_COMPAREFUNC_GREATER;
        d.cull_mode = SG_CULLMODE_BACK;
        d.face_winding = SG_FACEWINDING_CCW;
        g.map_pipeline = sg_make_pipeline(d);
    }
}

// I should ask the community what they know about the units used in the game
const float SCALE = 0.001f;
const float widget_pixel_radius = 30;
static float widget_radius(G &g, hmm_vec3 offset) {
    return HMM_Length(g.cam_pos - offset) * widget_pixel_radius / sapp_heightf() * tanf(g.fov / 2);
}

static hmm_mat4 camera_rot(G &g) {
    // We pitch the camera by applying a rotation around X,
    // then yaw the camera by applying a rotation around Y.
    auto pitch_matrix = HMM_Rotate(g.pitch * (360 / TAU32), HMM_Vec3(1, 0, 0));
    auto yaw_matrix = HMM_Rotate(g.yaw * (360 / TAU32), HMM_Vec3(0, 1, 0));
    return yaw_matrix * pitch_matrix;
}
static Ray screen_to_ray(G &g, hmm_vec2 mouse_xy) {
    Ray ray = {};
    ray.pos = { g.cam_pos.X, g.cam_pos.Y, g.cam_pos.Z };
    hmm_vec2 ndc = { mouse_xy.X, mouse_xy.Y };
    ndc.X = ndc.X / sapp_widthf() * 2 - 1;
    ndc.Y = ndc.Y / sapp_heightf() * -2 + 1;
    //Log("NDC %f, %f", ndc.X, ndc.Y);
    hmm_vec4 ray_dir4 = { ndc.X, ndc.Y, -1, 0 };
    ray_dir4.XY *= tanf(g.fov / 2);
    ray_dir4.X *= sapp_widthf() / sapp_heightf();
    ray_dir4 = camera_rot(g) * ray_dir4;
    ray.dir = HMM_Normalize(ray_dir4.XYZ);
    return ray;
}

static void event(const sapp_event *e_, void *userdata) {
    G &g = *(G *)userdata;
    simgui_handle_event(e_);
    const sapp_event &e = *e_;
    if (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (e.mouse_button == SAPP_MOUSEBUTTON_LEFT) {
            g.click_ray = screen_to_ray(g, { e.mouse_x, e.mouse_y });

            //Log("Pos = %f, %f, %f, %f", ray_pos.X, ray_pos.Y, ray_pos.Z, ray_pos.W);
            //Log("Dir = %f, %f, %f, %f", ray_dir.X, ray_dir.Y, ray_dir.Z, ray_dir.W);

            if (g.cld.valid) {
                const hmm_vec3 origin = -g.cld_origin;
                const hmm_mat4 Tinv = HMM_Translate(-origin);
                const hmm_mat4 Sinv = HMM_Scale( { 1 / SCALE, 1 / -SCALE, 1 / -SCALE });
                const hmm_mat4 Minv = Tinv * Sinv;
                const hmm_vec4 ray_pos = Minv * hmm_vec4{ g.click_ray.pos.X, g.click_ray.pos.Y, g.click_ray.pos.Z, 1 };
                const hmm_vec4 ray_dir = HMM_Normalize(Minv * hmm_vec4{ g.click_ray.dir.X, g.click_ray.dir.Y, g.click_ray.dir.Z, 0 });

                float closest_t = INFINITY;
                int hit_group = -1;
                int hit_face_index = -1;
                int hit_vertex = -1;
                hmm_vec3 hit_widget_pos = {};
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
                        int vertices_to_raycast = 3;
                        if (face->quad) {
                            vertices_to_raycast = 4;
                        }

                        for (int vertex_index = 0; vertex_index < vertices_to_raycast; vertex_index++) {
                            float (&vertex_floats)[3] = face->vertices[vertex_index];
                            hmm_vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
                            //Log("Minv*Pos = %f, %f, %f, %f", ray_pos.X, ray_pos.Y, ray_pos.Z, ray_pos.W);
                            //Log("Minv*Dir = %f, %f, %f, %f", ray_dir.X, ray_dir.Y, ray_dir.Z, ray_dir.W);

                            //Log("Vertex Pos = %f, %f, %f", vertex.X, vertex.Y, vertex.Z);

                            hmm_vec3 offset = -g.cld_origin + vertex;
                            offset.X *= SCALE;
                            offset.Y *= -SCALE;
                            offset.Z *= -SCALE;

                            hmm_vec3 widget_pos = vertex;
                            float radius = widget_radius(g, offset) / SCALE;

                            auto raycast = ray_vs_aligned_circle(ray_pos.XYZ, ray_dir.XYZ, widget_pos, radius);
                            if (raycast.hit) {
                                if (raycast.t < closest_t) {
                                    closest_t = raycast.t;
                                    hit_group = group;
                                    hit_face_index = face_index;
                                    hit_vertex = vertex_index;
                                    hit_widget_pos = widget_pos;
                                }
                            }
                        }
                    }
                }
                if (closest_t < INFINITY) {
                    g.widget_original_pos = hit_widget_pos;
                    assert(hit_group >= 0);
                    assert(hit_group < 4);
                    assert(hit_face_index >= 0);
                    assert(hit_vertex >= 0);
                    assert(hit_vertex < 4);
                    assert(g.select_cld_group == hit_group);
                    assert(g.select_cld_face == hit_face_index);
                    g.control_state = ControlState::Dragging;
                    g.drag_cld_group = hit_group;
                    g.drag_cld_face = hit_face_index;
                    g.drag_cld_vertex = hit_vertex;
                } else {
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
                                hmm_vec3 a = { face->vertices[0][0], face->vertices[0][1], face->vertices[0][2] };
                                hmm_vec3 b = { face->vertices[1][0], face->vertices[1][1], face->vertices[1][2] };
                                hmm_vec3 c = { face->vertices[2][0], face->vertices[2][1], face->vertices[2][2] };
                                Ray ray = { ray_pos.XYZ, ray_dir.XYZ };
                                auto raycast = ray_vs_triangle(ray, a, b, c);
                                if (!raycast.hit) {
                                    if (face->quad) {
                                        hmm_vec3 d = { face->vertices[3][0], face->vertices[3][1], face->vertices[3][2] };
                                        raycast = ray_vs_triangle(ray, a, c, d);
                                    }
                                }
                                if (raycast.hit) {
                                    if (raycast.t < closest_t) {
                                        closest_t = raycast.t;
                                        hit_group = group;
                                        hit_face_index = face_index;
                                        hit_vertex = -1;
                                        Log("hit %d, %d", hit_group, hit_face_index);
                                    }
                                }
                            }
                        }
                    }
                    assert(hit_vertex == -1);
                    if (closest_t < INFINITY) {
                        g.select_cld_group = hit_group;
                        g.select_cld_face = hit_face_index;
                    } else {
                        g.select_cld_group = -1;
                        g.select_cld_face = -1;
                    }
                    g.control_state = ControlState::Normal;
                    g.drag_cld_group = -1;
                    g.drag_cld_face = -1;
                    g.drag_cld_vertex = -1;
                }
            }
        }
        if (e.mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            g.control_state = ControlState::Orbiting;
        }
    }
    if (e.type == SAPP_EVENTTYPE_UNFOCUSED) {
        g.control_state = ControlState::Normal;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_UP) {
        g.control_state = ControlState::Normal;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        if (g.control_state == ControlState::Orbiting) {
            g.yaw += -e.mouse_dx * 3 * (0.022f * (TAU32 / 360));
            g.pitch += -e.mouse_dy * 3 * (0.022f * (TAU32 / 360));
        }
        if (g.control_state == ControlState::Dragging) {
            if (g.cld.valid) {
                hmm_vec2 prev_mouse_pos = { e.mouse_x - e.mouse_dx, e.mouse_y - e.mouse_dy };
                hmm_vec2 this_mouse_pos = { e.mouse_x, e.mouse_y };
                
                PH2CLD_Face *faces = g.cld.group_0_faces;
                size_t num_faces = g.cld.group_0_faces_count;
                if (g.drag_cld_group == 1) { faces = g.cld.group_1_faces; num_faces = g.cld.group_1_faces_count; }
                if (g.drag_cld_group == 2) { faces = g.cld.group_2_faces; num_faces = g.cld.group_2_faces_count; }
                if (g.drag_cld_group == 3) { faces = g.cld.group_3_faces; num_faces = g.cld.group_3_faces_count; }

                PH2CLD_Face *face = &faces[g.drag_cld_face];
                
                // @Temporary: @Deduplicate.
                float (&vertex_floats)[3] = face->vertices[g.drag_cld_vertex];
                hmm_vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
                hmm_vec3 origin = -g.cld_origin;

                hmm_mat4 Tinv = HMM_Translate(-origin);
                hmm_mat4 Sinv = HMM_Scale( { 1 / SCALE, 1 / -SCALE, 1 / -SCALE });
                hmm_mat4 Minv = Tinv * Sinv;

                hmm_vec3 offset = -g.cld_origin + vertex;
                offset.X *= SCALE;
                offset.Y *= -SCALE;
                offset.Z *= -SCALE;

                hmm_vec3 widget_pos = vertex;

                {
                    Ray click_ray = g.click_ray;
                    Ray this_ray = screen_to_ray(g, this_mouse_pos);
                    hmm_vec4 click_ray_pos = { 0, 0, 0, 1 };
                    click_ray_pos.XYZ = click_ray.pos;
                    hmm_vec4 click_ray_dir = {};
                    click_ray_dir.XYZ = click_ray.dir;
                    hmm_vec4 this_ray_pos = { 0, 0, 0, 1 };
                    this_ray_pos.XYZ = this_ray.pos;
                    hmm_vec4 this_ray_dir = {};
                    this_ray_dir.XYZ = this_ray.dir;

                    click_ray_pos = Minv * click_ray_pos;
                    click_ray_dir = HMM_Normalize(Minv * click_ray_dir);

                    this_ray_pos = Minv * this_ray_pos;
                    this_ray_dir = HMM_Normalize(Minv * this_ray_dir);

                    bool aligned_with_camera = true;
                    if (aligned_with_camera) {
                        // Remember you gotta put the plane in the coordinate space of the cld file!
                        hmm_vec3 plane_normal = ((Sinv * camera_rot(g)) * hmm_vec4 { 0, 0, -1, 0 }).XYZ;
                        auto click_raycast = ray_vs_plane(Ray { click_ray_pos.XYZ, click_ray_dir.XYZ }, widget_pos, plane_normal);
                        auto raycast = ray_vs_plane(Ray { this_ray_pos.XYZ, this_ray_dir.XYZ }, widget_pos, plane_normal);

                        assert(click_raycast.hit); // How could it not hit if it's aligned to the camera?
                        assert(raycast.hit);
                        hmm_vec3 drag_offset = click_raycast.point - g.widget_original_pos;
                        hmm_vec3 target = raycast.point - drag_offset;
                        if (e.modifiers & SAPP_MODIFIER_ALT) {
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
                                        hmm_vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };

                                        hmm_vec3 disp = vertex - target;
                                        float dist = HMM_Length(disp);
                                        if (dist < 150) {
                                            target = vertex;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        vertex_floats[0] = target.X;
                        vertex_floats[1] = target.Y;
                        vertex_floats[2] = target.Z;
                        g.cld_must_update = true;
                    } else { // @Temporary: only along XZ plane
                        auto click_raycast = ray_vs_plane(Ray { click_ray_pos.XYZ, click_ray_dir.XYZ }, widget_pos, { 0, 1, 0 });
                        auto raycast = ray_vs_plane(Ray { this_ray_pos.XYZ, this_ray_dir.XYZ }, widget_pos, { 0, 1, 0 });
                        if (click_raycast.hit && raycast.hit) {
                            // @Note: Drag-offsetting should use a screenspace offset computed when first clicked,
                            //        rather than a new raycast, so that distant vertices don't have problems and
                            //        so that it always looks like it is going directly to your cursor but modulo
                            //        the little screespace offset between your cursor and the centre of the circle.
                            //        For now, let's just have no offset so that it never has problems.
                            hmm_vec3 drag_offset = {};
                            // hmm_vec3 drag_offset = click_raycast.point - g.widget_original_pos;
                            hmm_vec3 target = raycast.point - drag_offset;
                            target.Y = vertex_floats[1]; // Needs to be assigned for precision reasons
                            if (e.modifiers & SAPP_MODIFIER_ALT) {
                                for (auto &f : target.Elements) {
                                    f = roundf(f / 100) * 100;
                                }
                            }
                            vertex_floats[0] = target.X;
                            vertex_floats[2] = target.Z;
                            g.cld_must_update = true;
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
    if (e.type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        hmm_vec4 translate = { 0, 0, -e.scroll_y * 0.1f, 0 };
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

static void frame(void *userdata) {
    G &g = *(G *)userdata;
    float dt = 0;
    defer {
        g.t += dt;
    };
    {
        auto next = get_time();
        dt = (float)(next - g.last_time);
        g.last_time = next;
    }
    simgui_new_frame(sapp_width(), sapp_height(), dt);
    sapp_lock_mouse(g.control_state == ControlState::Orbiting);
    // sapp_show_mouse(g.control_state != ControlState::Dragging);
    g.scroll_speed_timer -= dt;
    if (g.scroll_speed_timer < 0) {
        g.scroll_speed_timer = 0;
        g.scroll_speed = 0;
    }
    if (g.control_state == ControlState::Orbiting || g.control_state == ControlState::Dragging) {
        float rightwards = ((float)KEY('D') - (float)KEY('A')) * dt;
        float forwards   = ((float)KEY('S') - (float)KEY('W')) * dt;
        hmm_vec4 translate = {rightwards, 0, forwards, 0};
        if (HMM_Length(translate) == 0) {
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
    if (g.control_state != ControlState::Normal) {
        ImGui::GetStyle().Alpha = 0.333f;
    } else {
        ImGui::GetStyle().Alpha = 1;
    }
    {
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_MenuBar);
        defer {
            ImGui::End();
        };
        ImGui::SliderAngle("Camera FOV", &g.fov, FOV_MIN * (360 / TAU32), FOV_MAX * (360 / TAU32));
        if (ImGui::Button("Reset Camera")) {
            g.cam_pos = {};
            g.pitch = 0;
            g.yaw = 0;
        }
        ImGui::Text("MAP Geometries:");
        for (int i = 0; i < g.map_buffers_count; i++) {
            ImGui::SameLine();
            char b[32]; snprintf(b, sizeof b, "%d", g.map_buffers[i].id);
            ImGui::Checkbox(b, &g.map_buffers[i].shown);
        }
        ImGui::Text("CLD Subgroups:");
        {
            ImGui::PushID("CLD Subgroup Visibility Buttons");
            defer {
                ImGui::PopID();
            };
            bool all_subgroups_on = true;
            for (int i = 0; i < 16; i++) {
                ImGui::SameLine();
                ImGui::PushID(i);
                if (ImGui::Checkbox("###CLD Subgroup Visible", &g.subgroup_visible[i])) {
                    g.cld_must_update = true;
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
                    g.cld_must_update = true;
                }
            } else {
                if (ImGui::Button("None")) {
                    for (int i = 0; i < 16; i++) {
                        g.subgroup_visible[i] = false;
                    }
                    g.cld_must_update = true;
                }
            }
        }
        {
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
            if (!face) {
                ImGui::BeginDisabled();
            }
            defer {
                if (!face) {
                    ImGui::EndDisabled();
                }
            };
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
                g.cld_must_update = true;
            }
        }
    }
    {
        ImGui::SetNextWindowPos(ImVec2 { 60, sapp_height() * 0.98f - 280 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(256, 256), ImGuiCond_FirstUseEver);
        ImGui::Begin("Textures");
        defer {
            ImGui::End();
        };
        auto size = ImGui::GetWindowSize();
        {
            ImGui::BeginChild("texture_list", ImVec2(80, size.y - 50));
            defer {
                ImGui::EndChild();
            };
            for (int i = 0; i < countof(g.textures); i++) {
                if (g.textures[i].id) {
                    char b[16]; snprintf(b, sizeof b, "ID %d", i);
                    if (ImGui::Selectable(b, g.texture_ui_selected == i)) {
                        if (g.texture_ui_selected == i) {
                            g.texture_ui_selected = 0;
                        } else {
                            g.texture_ui_selected = i;
                        }
                    }
                }
            }
        }
        ImGui::SameLine(0,-1);
        if (g.texture_ui_selected != 0) {
            ImGui::BeginChild("texture_panel");
            defer {
                ImGui::EndChild();
            };
            sg_image_info info = sg_query_image_info(g.textures[g.texture_ui_selected]);
            float w = (float)info.width; 
            float h = (float)info.height;
            float aspect = w / h;
            if (w > size.x - 100) {
                w = size.x - 100;
                h = w / aspect;
            }
            if (h > size.y - 50) {
                h = size.y - 50;
                w = h * aspect;
            }
            if (w > 0 && h > 0) {
                ImGui::Image((ImTextureID)(uintptr_t)g.textures[g.texture_ui_selected].id, ImVec2(w, h));
            }
        }
    }
    imgui_do_console(g);
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
    {
        sg_pass_action p = {};
        p.colors[0].action = SG_ACTION_CLEAR;
        p.colors[0].value = { 0.0f, 0.0f, 0.0f, 1.0f };
        p.depth.action = SG_ACTION_CLEAR;
        p.depth.value = 0;
        sg_begin_default_pass(p, sapp_width(), sapp_height());
        defer {
            sg_end_pass();
        };
        hmm_mat4 perspective = {};
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
            float aspect_ratio = sapp_widthf() / sapp_heightf();
            // @Note: This is not the right projection matrix for default OpenGL, because its clip range is [-1,+1].
            //        You can call glClipControl to fix it, except in GLES2 where you need to fudge the matrix. Too bad!
            //perspective = hmm_mat4 {
            //{
            //    { cot_half_fov / aspect_ratio,            0,                                       0,  0 },
            //    { 0,                           cot_half_fov,                                       0,  0 },
            //    { 0,                                      0,     (near_z + far_z) / (near_z - far_z), -1 },
            //    { 0,                                      0, (2 * near_z * far_z) / (near_z - far_z),  0 },
            //}
            //};
            //perspective = hmm_mat4 {
            //{
            //    { cot_half_fov / aspect_ratio,            0,                                       0,  0 },
            //    { 0,                           cot_half_fov,                                       0,  0 },
            //    { 0,                                      0,     near_z / (far_z - near_z), -1 },
            //    { 0,                                      0, (far_z * near_z) / (far_z - near_z),  0 },
            //}
            //};
            perspective = hmm_mat4 {
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
                    params.M = HMM_Scale({ 1 * SCALE, -1 * SCALE, -1 * SCALE }) * HMM_Translate(-g.cld_origin);
                }
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE(params));
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = g.cld_face_buffers[i].buf;
                    sg_apply_bindings(b);
                    sg_draw(0, g.cld_face_buffers[i].num_vertices, 1);
                }
            }

            sg_apply_pipeline(g.map_pipeline);
            for (int i = 0; i < g.map_buffers_count; i++) {
                if (!g.map_buffers[i].shown) continue;
                {
                    // I should also ask the community what the coordinate system is :)
                    params.M = HMM_Scale({ 1 * SCALE, -1 * SCALE, -1 * SCALE }) * HMM_Translate({});
                }
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE(params));
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = g.map_buffers[i].buf;
                    sg_apply_bindings(b);
                    sg_draw(0, g.map_buffers[i].num_vertices, 1);
                }
            }
        }
        if (g.cld.valid) {
            highlight_vertex_circle_vs_params_t params = {};
            hmm_mat4 P = perspective;
            hmm_mat4 V = HMM_Transpose(camera_rot(g)) * HMM_Translate(-g.cam_pos);
            sg_apply_pipeline(g.highlight_vertex_circle_pipeline);
            float screen_height = sapp_heightf();
            hmm_mat4 R = camera_rot(g);
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
                        params.in_color = hmm_vec4 { 1, 1, 1, alpha };

                        float (&vertex_floats)[3] = face->vertices[i];
                        hmm_vec3 vertex = { vertex_floats[0], vertex_floats[1], vertex_floats[2] };
                        hmm_vec3 offset = -g.cld_origin + vertex;
                        offset.X *= SCALE;
                        offset.Y *= -SCALE;
                        offset.Z *= -SCALE;
                        hmm_mat4 T = HMM_Translate(offset);
                        float scale = scale_factor * widget_radius(g, offset);
                        hmm_mat4 S = HMM_Scale( { scale, scale, scale });
                        hmm_mat4 M = T * R * S;
                        params.MVP = P * V * M;
                        sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_highlight_vertex_circle_vs_params, SG_RANGE(params));
                        {
                            sg_bindings b = {};
                            b.vertex_buffers[0] = g.highlight_vertex_circle_buffer;
                            sg_apply_bindings(b);
                            sg_draw(0, 6, 1);
                        }
                    }
                }
            }
            if (0) { // @Debug
                hmm_vec3 offset = g.click_ray.pos + g.click_ray.dir;
                hmm_mat4 T = HMM_Translate(offset);
                float scale = 1; //HMM_Length(g.cam_pos - offset) * widget_pixel_radius / screen_height;
                hmm_mat4 S = HMM_Scale({scale, scale, scale});
                hmm_mat4 M = T * R * S;
                params.MVP = P * V * M;
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_highlight_vertex_circle_vs_params, SG_RANGE(params));
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = g.highlight_vertex_circle_buffer;
                    sg_apply_bindings(b);
                    sg_draw(0, 6, 1);
                }
            }
        }
        simgui_render();
    }
    sg_commit();
}

static void cleanup(void *userdata) {
    G &g = *(G *)userdata;
    (void)g;
    simgui_shutdown();
    sg_shutdown();
}

static void fail(const char *str) {
    char b[512];
    snprintf(b, sizeof(b), "There was an error during startup:\n    \"%s\"", str);
    Log("%s", b);
    MessageBoxA(0, b, "Fatal Error", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
}

G g;
sapp_desc sokol_main(int, char **) {
    sapp_desc d = {};
    d.width = 1900; // @Temporary
    d.height = 1000; // @Temporary
    d.user_data = &g;
    d.init_userdata_cb = init;
    d.event_userdata_cb = event;
    d.frame_userdata_cb = frame;
    d.cleanup_userdata_cb = cleanup;
    d.fail_cb = fail;
    d.sample_count = 4;
    d.swap_interval = 0;
    d.high_dpi = true;
#ifndef NDEBUG
    d.win32_console_create = true;
#endif
    d.html5_ask_leave_site = true;
    return d;
}
