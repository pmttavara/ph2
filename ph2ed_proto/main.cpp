// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

#include "common.hpp"

int num_array_resizes = 0;

// Path to MVP:
// - OBJ export
// - Undo/redo
// - MAP mesh vertex snapping

// FINISHED on Path to MVP:
// - Multimesh movement/deleting/editing
// - Better move UX

// PUNTED to post-MVP:
// - If I save and reload a map with a custom texture, it's moshed - OMG!!!
//    -> Specific to when I edited and re-saved the DDS from Paint.NET. Punt!

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

#define TAU 6.283185307179586476925
#define TAU32 6.283185307179586476925f

// Dear Imgui
#include "imgui.h"

// Sokol libraries
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"

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

void MsgBox_(const char *title, int flag, const char *msg, ...) {
    va_list arg;
    va_start(arg, msg);
    defer {
        va_end(arg);
    };
    char buf[1024];
    if (vsnprintf(buf, sizeof buf, msg, arg) > 0) {
        MessageBoxA((HWND)sapp_win32_get_hwnd(), buf, title, MB_OK | flag | MB_SYSTEMMODAL);
    }
}
#define MsgErr(title, ...) MsgBox_(title, MB_ICONERROR, "" __VA_ARGS__);
#define MsgWarn(title, ...) MsgBox_(title, MB_ICONWARNING, "" __VA_ARGS__);
#define MsgInfo(title, ...) MsgBox_(title, MB_ICONINFORMATION, "" __VA_ARGS__);

#include "HandmadeMath.h"

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

#ifdef NDEBUG
#pragma comment(linker, "/subsystem:windows")
#include "libs.cpp"
#else
#pragma comment(linker, "/subsystem:console")
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

// Texture subfiles can be empty, so they can't be implicitly encoded by indices in MAP_Texture.
struct MAP_Texture_Subfile {
    bool came_from_non_numbered_dependency = false; // @Temporary! (maybe? Yuck!)
    int texture_count = 0;
};

// Geometries can be empty, so they can't be implicitly encoded by indices in MAP_Mesh.
// (MeshGroups can't be empty, so they can. Same with GeometryGroup subfiles, which we store here.)
// This encoding is preserved to achieve bit-for-bit roundtrippability.
struct MAP_Geometry {
    uint32_t id = 0;
    uint32_t subfile_index = 0;
    // You'll need to look into the MAP_Mesh arrays to figure out your start+end indices for these geometries.
    uint32_t opaque_mesh_count = 0; // this one member encodes the entire MeshGroup.
    uint32_t transparent_mesh_count = 0; // this one member encodes the entire MeshGroup.
    uint32_t decal_count = 0; // this one member encodes the entire DecalGroup.

    // Only here to preserve bit-for-bit roundtrippability.
    bool has_weird_2_byte_misalignment_before_transparents = false;
    bool has_weird_2_byte_misalignment_before_decals = false;
};

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
    sg_buffer buf = {};
    MAP_Geometry_Buffer_Source source = MAP_Geometry_Buffer_Source::Opaque;
    Array<MAP_Geometry_Vertex> vertices = {};
    uint32_t id = 0;
    int subfile_index = 0;
    int global_geometry_index = 0; // @Todo: index within subfile
    int global_mesh_index = 0; // @Todo: index within geometry
    int mesh_part_group_index = 0;
    bool shown = true;
    bool selected = false; // Used by Imgui
    uint16_t material_index = 0;
    void release() {
        sg_destroy_buffer(buf);
        vertices.release();
        *this = {};
    }
};
struct MAP_Mesh_Vertex_Buffer {
    int bytes_per_vertex;
    char *data;
    int num_vertices;
};
struct MAP_Mesh_Part {
    int strip_length;
    int strip_count;

    bool was_inverted; // Only for roundtrippability.
};
struct MAP_Mesh_Part_Group {
    uint32_t material_index;
    uint32_t section_index;
    Array<MAP_Mesh_Part> mesh_parts = {};
};
// @Note: Geometries can be empty -- contain 0 mesh groups (no opaque, no transparent, no decal).
//        This means you can't just store tree nesting structure implicitly on the map meshes, you need
//        explicit metadata if you want to preserve bit-for-bit roundtrippability.
//        Geometry subfiles CANNOT be empty (I'm asserting geometry_count >= 1 as of writing). -p 2022-06-25
struct MAP_Mesh {
    bool bbox_override = false;
    float bounding_box_a[3] = {};
    float bounding_box_b[3] = {};
    Array<MAP_Mesh_Part_Group> mesh_part_groups = {};
    Array<MAP_Mesh_Vertex_Buffer> vertex_buffers = {};
    Array<uint16_t> indices = {};
    void release() {
        for (MAP_Mesh_Part_Group &mesh_part_group : mesh_part_groups) {
            mesh_part_group.mesh_parts.release();
        }
        mesh_part_groups.release();
        for (MAP_Mesh_Vertex_Buffer & vertex_buffer : vertex_buffers) {
            if (vertex_buffer.data) {
                free(vertex_buffer.data);
            }
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

struct MAP_Material {
    uint32_t subfile_index = 0;
    int16_t mode;
    uint16_t texture_id;
    uint32_t material_color;
    uint32_t overlay_color;
    float specularity;
};

struct MAP_Sprite_Metadata {
    uint16_t id;
    uint16_t format;
};
enum MAP_Texture_Format {
    MAP_Texture_Format_BC1,
    MAP_Texture_Format_BC2,
    MAP_Texture_Format_BC3,
};
// @Note: Texture subfiles can be empty -- contain 0 textures.
//        This means you can't just store tree nesting structure implicitly on the textures, you need
//        explicit metadata if you want to preserve bit-for-bit roundtrippability. -p 2022-06-25
struct MAP_Texture {
    sg_image image = {};
    uint16_t id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t material = 0;
    // Sprite metadata literally only exists to facilitate bit-for-bit roundtrippability.
    // The data isn't used otherwise, so there's no point in adding more than SH2 ever had.
    // The max SH2 has is 41, so round that up to 64 in a fixed-size array to avoid dynamic allocations.
    uint8_t sprite_count = 0;
    MAP_Sprite_Metadata sprite_metadata[64] = {};
    uint8_t format = MAP_Texture_Format_BC1;
    void *pixel_data = nullptr;
    uint32_t pixel_bytes = 0;
    void release() {
        if (image.id) {
            sg_destroy_image(image);
        }
        if (pixel_data) {
            free(pixel_data);
            pixel_data = nullptr;
        }
        *this = {};
    }
};

enum struct ControlState {
    Normal,
    Orbiting,
    Dragging,
};
struct G {
    double last_time = 0;
    double t = 0;
    float dt_history[1024] = {};

    ControlState control_state = {};
    float fov = FOV_DEFAULT;

    hmm_vec3 displacement = {};
    hmm_vec3 scaling_factor = {1, 1, 1};
    hmm_vec3 overall_center = {0, 0, 0};
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

    Array<MAP_Texture_Subfile> texture_subfiles = {};
    Array<MAP_Geometry> geometries = {};

    Array<MAP_Mesh> opaque_meshes = {};
    Array<MAP_Mesh> transparent_meshes = {};
    Array<MAP_Mesh> decal_meshes = {};

    sg_pipeline map_pipeline = {};
    enum { map_buffers_max = 64 };
    MAP_Geometry_Buffer map_buffers[map_buffers_max];// = {};
    int map_buffers_count = 0;

    sg_pipeline decal_pipeline = {};
    enum { decal_buffers_max = 64 };
    MAP_Geometry_Buffer decal_buffers[decal_buffers_max];// = {};
    int decal_buffers_count = 0;

    sg_buffer highlight_vertex_circle_buffer = {};
    sg_pipeline highlight_vertex_circle_pipeline = {};

    sg_image missing_texture = {};
    Array<MAP_Texture> textures = {};
    int texture_ui_selected = -1;

    Array<MAP_Material> materials = {};

    bool cld_must_update = false;
    bool map_must_update = false;
    bool subgroup_visible[16] = {true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};

    bool textured = true;
    bool lit = true;

    char *opened_map_filename = nullptr;

    void release() {
        PH2CLD_free_collision_data(cld);
        sg_destroy_pipeline(cld_pipeline);
        for (auto &buf : cld_face_buffers) {
            sg_destroy_buffer(buf.buf);
        }
        for (auto &mesh : opaque_meshes) {
            mesh.release();
        }
        for (auto &mesh : transparent_meshes) {
            mesh.release();
        }
        for (auto &mesh : decal_meshes) {
            mesh.release();
        }
        geometries.release();
        texture_subfiles.release();
        sg_destroy_pipeline(map_pipeline);
        for (auto &buf : map_buffers) {
            buf.release();
        }
        sg_destroy_pipeline(decal_pipeline);
        for (auto &buf : decal_buffers) {
            buf.release();
        }
        sg_destroy_buffer(highlight_vertex_circle_buffer);
        sg_destroy_pipeline(highlight_vertex_circle_pipeline);
        sg_destroy_image(missing_texture);
        for (auto &tex : textures) {
            tex.release();
        }
        textures.release();
        materials.release();
        free(opened_map_filename); opened_map_filename = nullptr;
        *this = {};
    }
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
    int16_t mode;
    uint16_t texture_id;
    uint32_t material_color;
    uint32_t overlay_color;
    float specularity;
};

void map_destrip_mesh_part_group(Array<MAP_Geometry_Vertex> &vertices, int &indices_index, const MAP_Mesh &mesh, MAP_Mesh_Part_Group mesh_part_group) {
    assert(mesh_part_group.mesh_parts.count > 0);
    vertices.clear();
    int vertices_reserve = 0;
    for (MAP_Mesh_Part &mesh_part : mesh_part_group.mesh_parts) {
        vertices_reserve += 3 * mesh_part.strip_count * mesh_part.strip_length;
    }
    vertices.reserve(vertices_reserve);
    for (MAP_Mesh_Part &mesh_part : mesh_part_group.mesh_parts) {
        int vertex_size = mesh.vertex_buffers[mesh_part_group.section_index].bytes_per_vertex;
        char *vertex_buffer = mesh.vertex_buffers[mesh_part_group.section_index].data;
        int vertex_buffer_count = mesh.vertex_buffers[mesh_part_group.section_index].num_vertices;
        char *end = mesh.vertex_buffers[mesh_part_group.section_index].data + vertex_buffer_count * vertex_size;
        // @Note: these assertions are probably not sanity checks, but rather real
        //        logical assert()s, because my code is broken if these are zero.
        assert(vertex_size);
        assert(vertex_buffer);
        int outer_max = mesh_part.strip_count;
        int inner_max = mesh_part.strip_length;
        auto get_index = [&] {
            return mesh.indices[indices_index++];
        };
        for (int strip_index = 0; strip_index < outer_max; strip_index++) {
            unsigned int memory = get_index() << 0x10;
            unsigned int mask = 0xFFFF0000;
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
                vertices.push(triangle_v0);
                vertices.push(triangle_v1);
                vertices.push(triangle_v2);
            }
        }
    }
}
// We need 'misalignment' because some mesh groups are weirdly misaligned, and mesh alignment happens *with respect to that misalignment* (!!!!!!!!!!)
struct Mesh_Group_Load_Result {
    int num_added;
    int total_bytes;
};
static Mesh_Group_Load_Result map_load_mesh_group_or_decal_group(Array<MAP_Mesh> *meshes, uint32_t misalignment, const char *group_header, const char *end, bool is_decal) {
    const char *ptr3 = group_header;
    uint32_t count = 0;
    Read(ptr3, count);
    assert(count > 0);
    meshes->reserve(count);
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

            MAP_Mesh_Vertex_Buffer vertex_buffer = {};
            defer {
                mesh.vertex_buffers.push(vertex_buffer);
            };
            
            vertex_buffer.bytes_per_vertex = vertex_section_header.bytes_per_vertex;

            vertex_buffer.num_vertices = vertex_section_header.section_length / vertex_section_header.bytes_per_vertex;

            assert(ptr5 == end_of_previous_section);
            assert(ptr5 == vertex_section_offset_base + vertex_section_header.section_starts);
            const char *end_of_this_section = ptr5 + vertex_section_header.section_length;
            vertex_buffer.data = (char *)malloc(vertex_buffer.num_vertices * vertex_buffer.bytes_per_vertex);
            memcpy(vertex_buffer.data, ptr5, vertex_buffer.num_vertices * vertex_buffer.bytes_per_vertex);
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
        mesh.mesh_part_groups.reserve(header.count);
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
    Mesh_Group_Load_Result load_result = {};
    load_result.num_added = (int)count;
    load_result.total_bytes = (int)(mesh_end - group_header);
    return load_result;
}
static void map_write_struct(Array<uint8_t> *result, const void *px, size_t sizeof_x) {
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

static void map_write_mesh_group_or_decal_group(Array<uint8_t> *result, bool decals, Array<MAP_Mesh> group, int start, int count, int misalignment) {
    int64_t aligner = result->count;
    WriteLit(uint32_t, count);
    int64_t offsets_start = result->count;

    for (int64_t i = 0; i < count; i++) {
        WriteLit(uint32_t, 0); // offset will be backpatched
    }

    for (int mesh_index = 0; mesh_index < count; mesh_index++) {
        // Mapmesh Header
        int64_t mesh_start = result->count;
        WriteBackpatch(uint32_t, offsets_start + mesh_index * sizeof(uint32_t), mesh_start - offsets_start + sizeof(uint32_t));
        MAP_Mesh &mesh = group[start + mesh_index];
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
                            auto verts = (PH2MAP__Vertex14 *)vertex_buffer.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                        case 0x18: {
                            auto verts = (PH2MAP__Vertex18 *)vertex_buffer.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                        case 0x20: {
                            auto verts = (PH2MAP__Vertex20 *)vertex_buffer.data;
                            memcpy(pos, verts[i].position, 3 * sizeof(float));
                        } break;
                        case 0x24: {
                            auto verts = (PH2MAP__Vertex24 *)vertex_buffer.data;
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
        WriteLit(int32_t, mesh.mesh_part_groups.count);

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
            map_write_struct(result, section.data, section.num_vertices * section.bytes_per_vertex);
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
    }
    assert(result->count % 16 == misalignment);
}
static int32_t map_compute_file_length(G &g) {
    int32_t file_length = 0;
    file_length += sizeof(PH2MAP__Header);
    int subfile_count = 0;
    int rolling_texture_index = 0;
    for (auto &sub : g.texture_subfiles) {
        if (sub.came_from_non_numbered_dependency) { // @Temporary i hope!
            rolling_texture_index += sub.texture_count;
            continue;
        }
        ++subfile_count;
        file_length += sizeof(PH2MAP__Subfile_Header) + sizeof(PH2MAP__Texture_Subfile_Header);
        for (int texture_index = rolling_texture_index; texture_index < rolling_texture_index + sub.texture_count; texture_index++) {
            auto &tex = g.textures[texture_index];
            file_length += sizeof(PH2MAP__BC_Texture_Header) + tex.sprite_count * sizeof(PH2MAP__Sprite_Header) + tex.pixel_bytes;
        }
        file_length += 16;
        rolling_texture_index += sub.texture_count;
    }
    int rolling_opaque_mesh_index = 0;
    int rolling_transparent_mesh_index = 0;
    int rolling_decal_index = 0;
    for (;;) {
        int geometry_start = 0;
        int geometry_count = 0;
        for (auto &geo : g.geometries) {
            if ((int)geo.subfile_index == subfile_count) {
                if (!geometry_count) {
                    geometry_start = (int)(&geo - g.geometries.data);
                }
                ++geometry_count;
            }
        }
        if (geometry_count <= 0) {
            break;
        }
        file_length += sizeof(PH2MAP__Subfile_Header) + sizeof(PH2MAP__Geometry_Subfile_Header);
        int misalignment = 0;
        for (int geometry_index = geometry_start; geometry_index < geometry_start + geometry_count; geometry_index++) {
            MAP_Geometry &geo = g.geometries[geometry_index];
            file_length += sizeof(PH2MAP__Geometry_Header);
            if (geo.opaque_mesh_count > 0) {
                file_length += (1 + geo.opaque_mesh_count) * sizeof(uint32_t); // count + offsets
                for (int mesh_index = rolling_opaque_mesh_index; mesh_index < rolling_opaque_mesh_index + (int)geo.opaque_mesh_count; mesh_index++) {
                    file_length += sizeof(PH2MAP__Mapmesh_Header);
                    MAP_Mesh &mesh = g.opaque_meshes[mesh_index];
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
            if (geo.transparent_mesh_count > 0) {
                if (geo.has_weird_2_byte_misalignment_before_transparents) {
                    file_length += 2;
                }
                file_length += (1 + geo.transparent_mesh_count) * sizeof(uint32_t); // count + offsets
                for (int mesh_index = rolling_transparent_mesh_index; mesh_index < rolling_transparent_mesh_index + (int)geo.transparent_mesh_count; mesh_index++) {
                    file_length += sizeof(PH2MAP__Mapmesh_Header);
                    MAP_Mesh &mesh = g.transparent_meshes[mesh_index];
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
            if (geo.decal_count > 0) {
                if (geo.has_weird_2_byte_misalignment_before_decals) {
                    file_length += 2;
                }
                file_length += (1 + geo.decal_count) * sizeof(uint32_t); // count + offsets
                for (int decal_index = rolling_decal_index; decal_index < rolling_decal_index + (int)geo.decal_count; decal_index++) {
                    file_length += sizeof(PH2MAP__Decal_Header);
                    MAP_Mesh &decal = g.decal_meshes[decal_index];
                    file_length += (int)(decal.mesh_part_groups.count * sizeof(PH2MAP__Sub_Decal));
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
            rolling_opaque_mesh_index += (int)geo.opaque_mesh_count;
            rolling_transparent_mesh_index += (int)geo.transparent_mesh_count;
            rolling_decal_index += (int)geo.decal_count;
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
    int32_t file_length = map_compute_file_length(g);
    result->reserve(file_length);

    // MAP Header
    WriteLit(uint32_t, 0x20010510);
    WriteLit(uint32_t, file_length);
    auto backpatch_subfile_count = CreateBackpatch(uint32_t);
    WriteLit(uint32_t, 0);

    int subfile_count = 0;
    int rolling_texture_index = 0;
    for (auto &sub : g.texture_subfiles) {
        defer {
            rolling_texture_index += sub.texture_count;
        };
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

        for (int texture_index = rolling_texture_index; texture_index < rolling_texture_index + sub.texture_count; texture_index++) {
            MAP_Texture &tex = g.textures[texture_index];
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
                    sprite_header.data_length = tex.pixel_bytes;
                } else {
                    sprite_header.data_length = 0;
                }
                sprite_header.data_length_plus_header = sprite_header.data_length + 16;
                sprite_header.always0x99000000 = 0x99000000;
                Write(sprite_header);
            }
            map_write_struct(result, tex.pixel_data, tex.pixel_bytes);
        }
        // textures are read until the first int of the line is 0, and then that line is skipped - hence, we add this terminator sentinel line here
        WriteLit(PH2MAP__BC_End_Sentinel, PH2MAP__BC_End_Sentinel{});

        WriteBackpatch(uint32_t, backpatch_subfile_length, result->count - subfile_start);
        ++subfile_count;
    }

    // GeometryGroup Subfiles
    assert(subfile_count >= 0);
    int rolling_opaque_mesh_index = 0;
    int rolling_transparent_mesh_index = 0;
    int rolling_decal_index = 0;
    for (;;) { // Write geometry subfiles and geometries
        int geometry_start = 0;
        int geometry_count = 0;
        for (auto &geo : g.geometries) {
            if ((int)geo.subfile_index == subfile_count) {
                if (!geometry_count) {
                    geometry_start = (int)(&geo - g.geometries.data);
                }
                ++geometry_count;
            }
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
        for (int geometry_index = geometry_start; geometry_index < geometry_start + geometry_count; geometry_index++) {
            auto &geo = g.geometries[geometry_index];

            // Geometry Header
            int64_t geometry_header_start = result->count;
            WriteLit(uint32_t, geo.id);
            auto backpatch_group_size = CreateBackpatch(int32_t);
            auto backpatch_opaque_group_offset = CreateBackpatch(int32_t);
            auto backpatch_transparent_group_offset = CreateBackpatch(int32_t);
            auto backpatch_decal_group_offset = CreateBackpatch(int32_t);

            if (geo.opaque_mesh_count) {
                assert(result->count % 16 == 4);
                misalignment = 0;
                WriteBackpatch(int32_t, backpatch_opaque_group_offset, result->count - geometry_header_start);
                map_write_mesh_group_or_decal_group(result, false, g.opaque_meshes, rolling_opaque_mesh_index, (int)geo.opaque_mesh_count, misalignment);
                assert(result->count % 16 == misalignment);
            }
            if (geo.transparent_mesh_count) {
                if (geo.has_weird_2_byte_misalignment_before_transparents) {
                    result->push(0);
                    result->push(0);
                    misalignment = 2;
                }
                if (!geo.opaque_mesh_count) {
                    assert(result->count % 16 == 4);
                } else {
                    assert(result->count % 16 == misalignment);
                }
                WriteBackpatch(int32_t, backpatch_transparent_group_offset, result->count - geometry_header_start);
                map_write_mesh_group_or_decal_group(result, false, g.transparent_meshes, rolling_transparent_mesh_index, (int)geo.transparent_mesh_count, misalignment);
                assert(result->count % 16 == misalignment);
            }
            if (geo.decal_count) {
                if (geo.has_weird_2_byte_misalignment_before_transparents || geo.has_weird_2_byte_misalignment_before_decals) {
                    while (result->count % 16 != 2) result->push(0);
                    misalignment = 2;
                }
                if (!geo.opaque_mesh_count && !geo.transparent_mesh_count) {
                    assert(result->count % 16 == 4);
                } else {
                    assert(result->count % 16 == misalignment);
                }
                WriteBackpatch(int32_t, backpatch_decal_group_offset, result->count - geometry_header_start);
                map_write_mesh_group_or_decal_group(result, true, g.decal_meshes, rolling_decal_index, (int)geo.decal_count, misalignment);
                assert(result->count % 16 == misalignment);
            }
            WriteBackpatch(int32_t, backpatch_group_size, result->count - geometry_header_start);
            rolling_opaque_mesh_index += (int)geo.opaque_mesh_count;
            rolling_transparent_mesh_index += (int)geo.transparent_mesh_count;
            rolling_decal_index += (int)geo.decal_count;
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
            material.material_color = mat.material_color;
            material.overlay_color = mat.overlay_color;
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
static MAP_Texture *map_get_texture_by_id(Array<MAP_Texture> textures, uint32_t id) {
    for (auto &tex : textures) {
        if (tex.id == id) {
            return &tex;
        }
    }
    return nullptr;
}
static void map_load(G &g, const char *filename, bool is_non_numbered_dependency = false) {
    g.geometries.clear();
    for (MAP_Mesh &mesh : g.opaque_meshes) mesh.release();
    for (MAP_Mesh &mesh : g.transparent_meshes) mesh.release();
    for (MAP_Mesh &mesh : g.decal_meshes) mesh.release();
    g.opaque_meshes.clear();
    g.transparent_meshes.clear();
    g.decal_meshes.clear();
    if (!is_non_numbered_dependency) {
        g.materials.clear();
        for (auto &tex : g.textures) {
            tex.release();
        }
        g.texture_subfiles.clear();
        g.textures.clear();
        g.texture_ui_selected = -1;
    }
    { // Semi-garbage code.
        int len = (int)strlen(filename);
        char *non_numbered = (char *) malloc(len + 1);
        assert(non_numbered);
        if (non_numbered) {
            defer {
                free(non_numbered);
            };
            
            strncpy(non_numbered, filename, len);
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
                    non_numbered[i] = 0;
                    int n = snprintf(nullptr, 0, "%s.map", non_numbered) + 1;
                    char *mem = (char *)malloc(n);
                    assert(mem);
                    if (mem) {
                        defer {
                            free(mem);
                        };
                        snprintf(mem, n, "%s.map", non_numbered);
                        if (strcmp(filename, mem) != 0) {
                            // Log("Loading \"%s\" for base textures", mem);
                            map_load(g, mem, true);
                        }
                    }
                }
            }
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
                LogC(IM_COL32(255, 127, 127, 255), "Failed loading MAP file \"%s\"!", filename);
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
                    assert(geometry_subfile_header.geometry_size == subfile_header.length - geometry_subfile_header.material_count * sizeof(PH2MAP__Material));
                    g.geometries.reserve(g.geometries.count + geometry_subfile_header.geometry_count);
                    for (uint32_t geometry_index = 0; geometry_index < geometry_subfile_header.geometry_count; geometry_index++) {
                        // Log("  In geometry %u", geometry_index);
                        const char *geometry_start = ptr2;
                        assert((uintptr_t)geometry_start % 16 == 0);
                        PH2MAP__Geometry_Header geometry_header = {};
                        Read(ptr2, geometry_header);
                        assert(geometry_header.group_size > 0);
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
                            int mesh_start = (int)g.opaque_meshes.count;
                            Mesh_Group_Load_Result result = map_load_mesh_group_or_decal_group(&g.opaque_meshes, 0, mesh_group_header, ptr2 + geometry_header.group_size, false);
                            opaque_group_length += result.total_bytes;
                            geometry.opaque_mesh_count += result.num_added;
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
                            int mesh_start = (int)g.transparent_meshes.count;
                            Mesh_Group_Load_Result result = map_load_mesh_group_or_decal_group(&g.transparent_meshes, geometry.has_weird_2_byte_misalignment_before_transparents ? 2 : 0, mesh_group_header, ptr2 + geometry_header.group_size, false);
                            transparent_group_length += result.total_bytes;
                            geometry.transparent_mesh_count += result.num_added;
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
                            int mesh_start = (int)g.decal_meshes.count;
                            Mesh_Group_Load_Result result = map_load_mesh_group_or_decal_group(&g.decal_meshes, geometry.has_weird_2_byte_misalignment_before_transparents || geometry.has_weird_2_byte_misalignment_before_decals ? 2 : 0, decal_group_header, ptr2 + geometry_header.group_size, true);
                            decal_group_length += result.total_bytes;
                            geometry.decal_count += result.num_added;
                        }
                        length_sum += decal_group_length;
                        assert(length_sum == geometry_header.group_size);
                        
                        ptr2 += geometry_header.group_size;

                    }
                    g.materials.reserve(g.materials.count + geometry_subfile_header.material_count);
                    for (uint32_t material_index = 0; material_index < geometry_subfile_header.material_count; material_index++) {
                        PH2MAP__Material material = {};
                        Read(ptr2, material);
                        assert(PH2CLD__sanity_check_float(material.specularity));

                        MAP_Material mat = {};
                        defer {
                            g.materials.push(mat);
                        };
                        mat.subfile_index = subfile_index;
                        mat.mode = material.mode;
                        mat.texture_id = material.texture_id;
                        mat.material_color = material.material_color;
                        mat.overlay_color = material.overlay_color;
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

                    MAP_Texture_Subfile texture_subfile = {};
                    defer {
                        g.texture_subfiles.push(texture_subfile);
                    };
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
                        ++texture_subfile.texture_count;
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

                        MAP_Texture &tex = *g.textures.push(); // @Cleanup: replace ref with ptr here
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

                                tex.pixel_bytes = (uint32_t)pixels_len;
                                tex.pixel_data = malloc(tex.pixel_bytes);
                                assert(tex.pixel_data);
                                memcpy(tex.pixel_data, pixels_data, tex.pixel_bytes);

                                assert(tex.image.id == 0);
                                if (tex.sprite_metadata[sprite_index].format == 0x100) {
                                    tex.format = MAP_Texture_Format_BC1;
                                } else if (tex.sprite_metadata[sprite_index].format == 0x102) {
                                    tex.format = MAP_Texture_Format_BC2;
                                } else if (tex.sprite_metadata[sprite_index].format == 0x103) {
                                    tex.format = MAP_Texture_Format_BC3;
                                } else if (tex.sprite_metadata[sprite_index].format == 0x104) {
                                    tex.format = MAP_Texture_Format_BC3;
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
        { // Round-trip test
            Array<uint8_t> round_trip = {};
            defer {
                round_trip.release();
            };
            map_write_to_memory(g, &round_trip);
            assert(round_trip.count == file_len_do_not_modify_me_please);
            assert(memcmp(round_trip.data, filedata_do_not_modify_me_please, round_trip.count) == 0);
        }
    }

    g.map_must_update = true;
    if (g.opened_map_filename) {
        free(g.opened_map_filename);
        g.opened_map_filename = nullptr;
    }
    g.opened_map_filename = strdup(filename);
    assert(g.opened_map_filename);

    for (auto &buf : g.map_buffers) {
        buf.shown = true;
        buf.selected = false;
    }
    for (auto &buf : g.decal_buffers) {
        buf.shown = true;
        buf.selected = false;
    }
}
static void map_upload(G &g) {
    g.map_buffers_count = 0;
    g.decal_buffers_count = 0;
    for (auto &tex : g.textures) {
        if (tex.image.id) {
            sg_destroy_image(tex.image);
        }
    }
    {
        // Log("about to upload %d map meshes", (int)g.opaque_meshes.count);
        int rolling_opaque_geo_index = 0;
        uint32_t rolling_opaque_geo_start = 0;
        uint32_t opaque_mesh_index = 0;
        for (MAP_Mesh &mesh : g.opaque_meshes) {
            int indices_index = 0;
            assert(rolling_opaque_geo_index < g.geometries.count);
            while (opaque_mesh_index >= rolling_opaque_geo_start + g.geometries[rolling_opaque_geo_index].opaque_mesh_count) {
                rolling_opaque_geo_start += g.geometries[rolling_opaque_geo_index].opaque_mesh_count;
                rolling_opaque_geo_index++;
                assert(rolling_opaque_geo_index < g.geometries.count);
            }
            // Log("about to upload %d mesh part groups", (int)mesh.mesh_part_groups.count);
            for (MAP_Mesh_Part_Group &mesh_part_group : mesh.mesh_part_groups) {
                assert(g.map_buffers_count < g.map_buffers_max);
                auto &map_buffer = g.map_buffers[g.map_buffers_count++];
                map_buffer.source = MAP_Geometry_Buffer_Source::Opaque;
                map_destrip_mesh_part_group(map_buffer.vertices, indices_index, mesh, mesh_part_group);
                assert(map_buffer.vertices.count % 3 == 0);
                map_buffer.id = g.geometries[rolling_opaque_geo_index].id;
                assert(mesh_part_group.material_index >= 0);
                assert(mesh_part_group.material_index < 65536);
                map_buffer.material_index = (uint16_t)mesh_part_group.material_index;

                map_buffer.subfile_index = g.geometries[rolling_opaque_geo_index].subfile_index;
                map_buffer.global_geometry_index = rolling_opaque_geo_index;
                map_buffer.global_mesh_index = (int)(&mesh - g.opaque_meshes.data); // @Lazy
                map_buffer.mesh_part_group_index = (int)(&mesh_part_group - mesh.mesh_part_groups.data); // @Lazy
            }
            ++opaque_mesh_index;
        }
        int rolling_transparent_geo_index = 0;
        uint32_t rolling_transparent_geo_start = 0;
        uint32_t transparent_mesh_index = 0;
        for (MAP_Mesh &mesh : g.transparent_meshes) {
            int indices_index = 0;
            assert(rolling_transparent_geo_index < g.geometries.count);
            while (transparent_mesh_index >= rolling_transparent_geo_start + g.geometries[rolling_transparent_geo_index].transparent_mesh_count) {
                rolling_transparent_geo_start += g.geometries[rolling_transparent_geo_index].transparent_mesh_count;
                rolling_transparent_geo_index++;
                assert(rolling_transparent_geo_index < g.geometries.count);
            }
            for (MAP_Mesh_Part_Group &mesh_part_group : mesh.mesh_part_groups) {
                assert(g.decal_buffers_count < g.decal_buffers_max);
                auto &decal_buffer = g.decal_buffers[g.decal_buffers_count++];
                decal_buffer.source = MAP_Geometry_Buffer_Source::Transparent;
                map_destrip_mesh_part_group(decal_buffer.vertices, indices_index, mesh, mesh_part_group);
                assert(decal_buffer.vertices.count % 3 == 0);
                decal_buffer.id = g.geometries[rolling_transparent_geo_index].id;
                assert(mesh_part_group.material_index >= 0);
                assert(mesh_part_group.material_index < 65536);
                decal_buffer.material_index = (uint16_t)mesh_part_group.material_index;

                decal_buffer.subfile_index = g.geometries[rolling_transparent_geo_index].subfile_index;
                decal_buffer.global_geometry_index = rolling_transparent_geo_index;
                decal_buffer.global_mesh_index = (int)(&mesh - g.transparent_meshes.data); // @Lazy
                decal_buffer.mesh_part_group_index = (int)(&mesh_part_group - mesh.mesh_part_groups.data); // @Lazy
            }
        }
        int rolling_decal_geo_index = 0;
        uint32_t rolling_decal_geo_start = 0;
        uint32_t decal_mesh_index = 0;
        for (MAP_Mesh &mesh : g.decal_meshes) {
            int indices_index = 0;
            assert(rolling_decal_geo_index < g.geometries.count);
            while (decal_mesh_index >= rolling_decal_geo_start + g.geometries[rolling_decal_geo_index].decal_count) {
                rolling_decal_geo_start += g.geometries[rolling_decal_geo_index].decal_count;
                rolling_decal_geo_index++;
                assert(rolling_decal_geo_index < g.geometries.count);
            }
            for (MAP_Mesh_Part_Group &mesh_part_group : mesh.mesh_part_groups) {
                assert(g.decal_buffers_count < g.decal_buffers_max);
                auto &decal_buffer = g.decal_buffers[g.decal_buffers_count++];
                decal_buffer.source = MAP_Geometry_Buffer_Source::Decal;
                map_destrip_mesh_part_group(decal_buffer.vertices, indices_index, mesh, mesh_part_group);
                assert(decal_buffer.vertices.count % 3 == 0);
                decal_buffer.id = g.geometries[rolling_decal_geo_index].id;
                assert(mesh_part_group.material_index >= 0);
                assert(mesh_part_group.material_index < 65536);
                decal_buffer.material_index = (uint16_t)mesh_part_group.material_index;

                decal_buffer.subfile_index = g.geometries[rolling_decal_geo_index].subfile_index;
                decal_buffer.global_geometry_index = rolling_decal_geo_index;
                decal_buffer.global_mesh_index = (int)(&mesh - g.decal_meshes.data); // @Lazy
                decal_buffer.mesh_part_group_index = (int)(&mesh_part_group - mesh.mesh_part_groups.data); // @Lazy
            }
        }
        for (auto &tex : g.textures) {
            sg_image_desc d = {};
            d.width = tex.width;
            d.height = tex.height;
            switch (tex.format) {
                case MAP_Texture_Format_BC1: d.pixel_format = SG_PIXELFORMAT_BC1_RGBA; break;
                case MAP_Texture_Format_BC2: d.pixel_format = SG_PIXELFORMAT_BC2_RGBA; break;
                case MAP_Texture_Format_BC3: d.pixel_format = SG_PIXELFORMAT_BC3_RGBA; break;
                default: assert(false); break;
            };
            d.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            d.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            d.min_filter = SG_FILTER_NEAREST;
            d.mag_filter = SG_FILTER_NEAREST;
            d.max_anisotropy = 16;
            d.data.subimage[0][0] = { tex.pixel_data, tex.pixel_bytes };
            tex.image = sg_make_image(d);
            assert(tex.image.id);
        }

        // Log("%lld geometries", g.geometries.count);
        // for (auto &geo : g.geometries) {
        //     Log("    %u meshes", geo.opaque_mesh_count + geo.transparent_mesh_count + geo.decal_count);
        // }
        // Log("%lld texture subfiles", g.texture_subfiles.count);
        // for (auto &sub : g.texture_subfiles) {
        //     Log("    %u textures%s", sub.texture_count, sub.came_from_non_numbered_dependency ? " (from non-numbered dependency)" : "");
        // }
    }
    for (int i = 0; i < g.map_buffers_count; i++) {
        auto &buf = g.map_buffers[i];
        sg_update_buffer(buf.buf, sg_range { buf.vertices.data, buf.vertices.count * sizeof(buf.vertices[0]) });
    }
    for (int i = 0; i < g.decal_buffers_count; i++) {
        auto &buf = g.decal_buffers[i];
        sg_update_buffer(buf.buf, sg_range { buf.vertices.data, buf.vertices.count * sizeof(buf.vertices[0]) });
    }
}
static void test_all_maps(G &g) {
    struct _finddata_t find_data;
    intptr_t directory = _findfirst("map/*.map", &find_data);
    assert(directory >= 0);
    int spinner = 0;
    int num_tested = 0;
    while (1) {
        char b[260 + sizeof("map/")];
        snprintf(b, sizeof(b), "map/%s", find_data.name);
        // Log("Loading map \"%s\"", b);
        // printf("%c\r", "|/-\\"[spinner++ % 4]);
        map_load(g, b);
        ++num_tested;
        if (_findnext(directory, &find_data) < 0) {
            if (errno == ENOENT) break;
            else assert(0);
        }
    }
    _findclose(directory);
    Log("Tested %d maps.", num_tested);
}

static void imgui_do_console(G &g) {
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
        } else if (memcmp("test_all_maps", buf, sizeof("test_all_maps") - 1) == 0) {
            test_all_maps(g);
        } else {
            Log("Unknown command :)");
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

static bool file_exists(const uint16_t *filename16) {
    DWORD attr = GetFileAttributesW((LPCWSTR)filename16);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
static bool file_exists(LPCWSTR filename16) { return file_exists((const uint16_t *)filename16); }

void *operator new(size_t, void *ptr) { return ptr; }
static void init(void *userdata) {
    double init_time = -get_time();
    defer {
        init_time += get_time();
        // Log("Init() took %f seconds.", init_time);
    };
#ifndef NDEBUG
    MoveWindow(GetConsoleWindow(), -1925, 0, 1500, 800, true);
    MoveWindow((HWND)sapp_win32_get_hwnd(), -1870, 50, 1500, 800, true);
    ShowWindow((HWND)sapp_win32_get_hwnd(), SW_MAXIMIZE);
#endif
    G &g = *(G *)userdata;
    new (userdata) G{};
    g.last_time = get_time();
    sg_desc desc = {};
    desc.context = sapp_sgcontext();
    desc.buffer_pool_size = 256;
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
Size=1536,789
Collapsed=0

[Window][Debug##Default]
Pos=60,60
Size=400,400
Collapsed=0

[Window][Console]
Pos=953,633
Size=452,229
Collapsed=0

[Window][Editor]
Pos=0,20
Size=392,427
Collapsed=0
DockId=0x00000001,0

[Window][Textures]
Pos=0,449
Size=392,360
Collapsed=0
DockId=0x00000005,0

[Window][Materials]
Pos=394,449
Size=1142,360
Collapsed=0
DockId=0x00000006,0

[Window][Viewport]
Pos=394,20
Size=1142,427
Collapsed=0
DockId=0x00000002,0

[Docking][Data]
DockSpace     ID=0x8B93E3BD Window=0xA787BDB4 Pos=0,20 Size=1536,789 Split=Y Selected=0x13926F0B
  DockNode    ID=0x00000003 Parent=0x8B93E3BD SizeRef=1536,427 Split=X
    DockNode  ID=0x00000001 Parent=0x00000003 SizeRef=392,789 Selected=0x9F27EDF6
    DockNode  ID=0x00000002 Parent=0x00000003 SizeRef=1142,789 CentralNode=1 Selected=0x13926F0B
  DockNode    ID=0x00000004 Parent=0x8B93E3BD SizeRef=1536,360 Split=X Selected=0xFC744897
    DockNode  ID=0x00000005 Parent=0x00000004 SizeRef=392,256 Selected=0xFC744897
    DockNode  ID=0x00000006 Parent=0x00000004 SizeRef=1142,256 Selected=0x6AE1E39D
)");
    }
    {
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigWindowsMoveFromTitleBarOnly = false; // only really needed because the resize widget is in the bottom right corner and we can't resize from edges for some reason...
        io.ConfigWindowsResizeFromEdges = true; // Why doesn't this work???

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
            buffer.buf = sg_make_buffer(d);
        }
        for (auto &buffer : g.decal_buffers) {
            buffer.buf = sg_make_buffer(d);
        }
    }
    if (0) {
        test_all_maps(g);
        sapp_request_quit();
    }

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
        d.cull_mode = SG_CULLMODE_BACK;
        d.face_winding = SG_FACEWINDING_CCW;
        // d.primitive_type = SG_PRIMITIVETYPE_POINTS;
        g.map_pipeline = sg_make_pipeline(d);
        d.depth.write_enabled = false;
        d.alpha_to_coverage_enabled = false;
        d.colors[0].blend.enabled = true;
        d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        g.decal_pipeline = sg_make_pipeline(d);
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
        g.missing_texture = sg_make_image(d);
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
    ndc.X = ((ndc.X - g.view_x) / g.view_w) * 2 - 1;
    ndc.Y = ((ndc.Y - g.view_y) / g.view_h) * -2 + 1;
    //Log("NDC %f, %f", ndc.X, ndc.Y);
    hmm_vec4 ray_dir4 = { ndc.X, ndc.Y, -1, 0 };
    ray_dir4.XY *= tanf(g.fov / 2);
    ray_dir4.X *= g.view_w / g.view_h;
    ray_dir4 = camera_rot(g) * ray_dir4;
    ray.dir = HMM_Normalize(ray_dir4.XYZ);
    return ray;
}

static void event(const sapp_event *e_, void *userdata) {
    G &g = *(G *)userdata;
    const sapp_event &e = *e_;
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
            (e.key_code == SAPP_KEYCODE_F11) ||
            (e.key_code == SAPP_KEYCODE_ENTER && (e.modifiers & SAPP_MODIFIER_ALT))) {
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
    }
    simgui_handle_event(&e);
    if (g.control_state == ControlState::Normal) {
        bool mouse_in_viewport = (e.mouse_x >= g.view_x && e.mouse_x <= g.view_x + g.view_w &&
                                  e.mouse_y >= g.view_y && e.mouse_y <= g.view_y + g.view_h);
        if (!mouse_in_viewport || ImGui::GetIO().WantCaptureKeyboard) {
            return;
        }
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
        case FOURCC_DXT1: { tex.format = (uint8_t)MAP_Texture_Format_BC1; } break;
        case FOURCC_DXT2: { tex.format = (uint8_t)MAP_Texture_Format_BC2; } break;
        case FOURCC_DXT3: { tex.format = (uint8_t)MAP_Texture_Format_BC2; } break;
        case FOURCC_DXT4: { tex.format = (uint8_t)MAP_Texture_Format_BC3; } break;
        case FOURCC_DXT5: { tex.format = (uint8_t)MAP_Texture_Format_BC3; } break;
        default: { assert(false); } break;
    }
    // @Note: *PRETTY* sure this is fine??
    switch (tex.format) {
        case MAP_Texture_Format_BC1: { tex.sprite_metadata[0].format = 0x100; } break;
        case MAP_Texture_Format_BC2: { tex.sprite_metadata[0].format = 0x102; } break;
        case MAP_Texture_Format_BC3: { tex.sprite_metadata[0].format = 0x103; } break;
        default: { assert(false); } break;
    }

    tex.pixel_bytes = header.pitch_or_linear_size;
    tex.pixel_data = malloc(header.pitch_or_linear_size);
    FailIfFalse(tex.pixel_data, "Texture data couldn't be allocated in memory.");
    FailIfFalse(fread(tex.pixel_data, tex.pixel_bytes, 1, f) == 1, "File read error reading the texture data. File too small?");

    result = tex;
    tex = {};

    MsgInfo("DDS Import", "Imported!");

    return true;
}

static void viewport_callback(const ImDrawList* dl, const ImDrawCmd* cmd);
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
        g.dt_history[sapp_frame_count() % countof(g.dt_history)] = dt;
    }
    simgui_new_frame({ sapp_width(), sapp_height(), dt, sapp_dpi_scale() });
    char *obj_file_buf = nullptr;
    defer {
        free(obj_file_buf);
    };
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
                g.control_shift_s = false;
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
            for (auto &buf : g.map_buffers) if (buf.selected) any_selected = true;
            for (auto &buf : g.decal_buffers) if (buf.selected) any_selected = true;
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
        if (ImGui::BeginMenu("View")) {
            defer { ImGui::EndMenu(); };
            ImGui::MenuItem("Editor", nullptr, &g.show_editor);
            ImGui::MenuItem("Viewport", nullptr, &g.show_viewport);
            ImGui::MenuItem("Textures", nullptr, &g.show_textures);
            ImGui::MenuItem("Materials", nullptr, &g.show_materials);
            ImGui::MenuItem("Console", nullptr, &g.show_console);
        }
        if (ImGui::BeginMenu("About")) {
            defer { ImGui::EndMenu(); };
            ImGui::MenuItem("Psilent pHill 2 Editor v0.001", nullptr, false, false);
            ImGui::MenuItem("Built On: " __DATE__ " " __TIME__, nullptr, false, false);
            ImGui::Separator();
#define URL "https://github.com/pmttavara/ph2"
            if (ImGui::MenuItem(URL)) {
                // Okay, we'll be nice people and ask for confirmation.
                if (MessageBoxA((HWND)sapp_win32_get_hwnd(),
                    "This will open your browser to " URL ". Go?",
                    "Open Site",
                    MB_YESNO | MB_ICONINFORMATION | MB_SYSTEMMODAL) == IDYES) {
                    ShellExecuteW(0, L"open", L"" URL, 0, 0, SW_SHOW);
                }
            }
        }
        ImGui::SameLine(sapp_widthf() / sapp_dpi_scale() - 70.0f);
        double frametime = 0.0f;
        uint64_t last_frame = sapp_frame_count();
        uint64_t first_frame = max((int64_t)(sapp_frame_count() - countof(g.dt_history) + 1), 0);
        for (auto i = first_frame; i <= last_frame; i++) {
            frametime += g.dt_history[sapp_frame_count() % countof(g.dt_history)];
        }
        ImGui::Text("%.0f FPS", (last_frame - first_frame) / frametime);
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
    ImGui::GetStyle().Alpha = 1;
    // if (g.control_state != ControlState::Normal) {
    //     ImGui::GetStyle().Alpha = 0.333f;
    // }
    auto get_meshes = [&] (MAP_Geometry_Buffer &buf) -> Array<MAP_Mesh> & {
        switch (buf.source) {
            default: assert(false);
            case MAP_Geometry_Buffer_Source::Opaque: return g.opaque_meshes;
            case MAP_Geometry_Buffer_Source::Transparent: return g.transparent_meshes;
            case MAP_Geometry_Buffer_Source::Decal: return g.decal_meshes;
        }
    };
        if (obj_file_buf) {
            FILE *f = PH2CLD__fopen(obj_file_buf, "r");
            if (f) {
                defer {
                    fclose(f);
                };

                Array<hmm_vec3> positions = {}; defer { positions.release(); };
                Array<hmm_vec2> uvs = {}; defer { uvs.release(); };
                Array<hmm_vec3> normals = {}; defer { normals.release(); };
                // Array<uint32_t> colours = {}; defer { colours.release(); };
                Array<PH2MAP__Vertex20> verts = {}; defer { verts.release(); }; // @Errdefer

                Array<int> unstripped_indices = {}; defer { unstripped_indices.release(); };

                char b[1024];
                hmm_vec3 center = {};
                while (fgets(b, sizeof b, f)) {
                    if (char *lf = strrchr(b, '\n')) *lf = 0;
                    char directive[3] = {};
                    char args[4][64] = {};
                    int matches = sscanf(b, " %s %s %s %s %s ", directive, args[0], args[1], args[2], args[3]);
                    if (strcmp("v", directive) == 0) {
                        // Position
                        assert(matches == 4);
                        // Log("Position: (%s, %s, %s)", args[0], args[1], args[2]);
                        auto &pos = *positions.push();
                        pos.X = (float)atof(args[0]);
                        pos.Y = (float)atof(args[1]);
                        pos.Z = (float)atof(args[2]);
                        center += pos;
                    } else if (strcmp("vt", directive) == 0) {
                        // UV
                        assert(matches == 3);
                        // Log("UV: (%s, %s)", args[0], args[1]);
                        auto &uv = *uvs.push();
                        uv.X = (float)atof(args[0]);
                        uv.Y = (float)atof(args[1]);
                    } else if (strcmp("vn", directive) == 0) {
                        // Normal
                        assert(matches == 4);
                        // Log("Normal: (%s, %s, %s)", args[0], args[1], args[2]);
                        auto &normal = *normals.push();
                        normal.X = (float)atof(args[0]);
                        normal.Y = (float)atof(args[1]);
                        normal.Z = (float)atof(args[2]);
                    } else if (strcmp("f", directive) == 0) {
                        // Triangle/Quad
                        assert(matches == 4 || matches == 5);
                        // Log("Triangle/Quad: (%s, %s, %s%s%s)", args[0], args[1], args[2], matches == 5 ? ", " : "", matches == 5 ? args[3] : "");
                        int indices_to_push[4] = {};
                        for (int i = 0; i < matches - 1; i++) {
                            PH2MAP__Vertex20 vert = {};
                            int index_pos = 0;
                            int index_uv = 0;
                            int index_normal = 0;
                            int sub_matches = sscanf(args[i], "%d/%d/%d", &index_pos, &index_uv, &index_normal);
                            if (sub_matches >= 1) { // %d[...]
                                assert(index_pos > 0);
                                vert.position[0] = positions[index_pos - 1].X;
                                vert.position[1] = positions[index_pos - 1].Y;
                                vert.position[2] = positions[index_pos - 1].Z;
                            } else {
                                assert(false); // @Lazy
                            }
                            if (sub_matches == 3) { // %d/%d/%d
                                assert(index_uv > 0);
                                assert(index_normal > 0);
                                vert.uv[0] = uvs[index_uv - 1].X;
                                vert.uv[1] = uvs[index_uv - 1].Y;
                                vert.normal[0] = normals[index_normal - 1].X;
                                vert.normal[1] = normals[index_normal - 1].Y;
                                vert.normal[2] = normals[index_normal - 1].Z;
                            } else if (sub_matches == 2) { // %d/%d
                                assert(index_uv > 0);
                                vert.uv[0] = uvs[index_uv - 1].X;
                                vert.uv[1] = uvs[index_uv - 1].Y;
                            } else if (sub_matches == 1) { // %d[...]
                                int dummy = 0;
                                sub_matches = sscanf(args[i], "%d//%d", &dummy, &index_normal);
                                assert(dummy == index_pos);
                                if (sub_matches == 2) { // %d//%d
                                    assert(index_normal > 0);
                                    vert.normal[0] = normals[index_normal - 1].X;
                                    vert.normal[1] = normals[index_normal - 1].Y;
                                    vert.normal[2] = normals[index_normal - 1].Z;
                                } else {
                                    assert(false);
                                }
                            }
                            verts.push(vert);
                            indices_to_push[i] = (int)verts.count - 1;
                        }
                        auto push_wound = [&] (int a, int b, int c) {
                            int idx_a = indices_to_push[a];
                            int idx_b = indices_to_push[b];
                            int idx_c = indices_to_push[c];
                            PH2MAP__Vertex20 vert_a = verts[idx_a];
                            PH2MAP__Vertex20 vert_b = verts[idx_b];
                            PH2MAP__Vertex20 vert_c = verts[idx_c];
                            // Infer face winding by comparing cross product to normal
                            hmm_vec3 v0 = { vert_a.position[0], vert_a.position[1], vert_a.position[2] };
                            hmm_vec3 v1 = { vert_b.position[0], vert_b.position[1], vert_b.position[2] };
                            hmm_vec3 v2 = { vert_c.position[0], vert_c.position[1], vert_c.position[2] };

                            hmm_vec3 n0 = { vert_a.normal[0], vert_a.normal[1], vert_a.normal[2] };
                            hmm_vec3 n1 = { vert_b.normal[0], vert_b.normal[1], vert_b.normal[2] };
                            hmm_vec3 n2 = { vert_c.normal[0], vert_c.normal[1], vert_c.normal[2] };

                            hmm_vec3 wound_normal = HMM_Cross((v1 - v0), (v2 - v0));
                            hmm_vec3 given_normal = HMM_Normalize(n0 + n1 + n2);

                            float dot = HMM_Dot(wound_normal, given_normal);

                            bool is_wound_right = (dot >= 0);

                            if (is_wound_right) {
                                unstripped_indices.push(idx_b);
                                unstripped_indices.push(idx_a);
                                unstripped_indices.push(idx_c);
                            } else {
                                unstripped_indices.push(idx_b);
                                unstripped_indices.push(idx_a);
                                unstripped_indices.push(idx_c);
                            }
                        };
                        push_wound(0, 1, 2);
                        if (matches == 5) { // Quad - upload another triangle
                            push_wound(0, 2, 3);
                        }
                    }
                    memset(b, 0, sizeof b);
                }
                // Log("We got %lld positions, %lld uvs, %lld normals.", positions.count, uvs.count, normals.count);
                // Log("We built %lld vertices.", verts.count);
                // Log("We built %lld stripped indices.", stripped_indices.count);
                MAP_Mesh mesh = {}; defer { mesh.release(); }; // @Errdefer

                static PH2MAP__Vertex20 mesh_verts[65536] = {};
                int mesh_verts_count = 0;

                auto flush_mesh = [&] { // @Todo: return bool
                    MAP_Mesh_Vertex_Buffer &buf = *mesh.vertex_buffers.push();
                    buf.data = (char *)malloc(mesh_verts_count * sizeof(mesh_verts[0]));
                    assert(buf.data);
                    buf.num_vertices = mesh_verts_count;
                    buf.bytes_per_vertex = sizeof(mesh_verts[0]);
                    memcpy(buf.data, mesh_verts, mesh_verts_count * sizeof(mesh_verts[0]));

                    MAP_Mesh_Part_Group &group = *mesh.mesh_part_groups.push();
                    group.material_index = 0; // @Todo
                    group.section_index = 0;
                    MAP_Mesh_Part &part = *group.mesh_parts.push();
                    part.strip_count = 1;
                    part.strip_length = (uint16_t)mesh.indices.count;
                    assert(mesh.mesh_part_groups[0].mesh_parts[0].strip_count > 0);
                    assert(mesh.mesh_part_groups[0].mesh_parts[0].strip_length > 0);

                    g.geometries[g.geometries.count - 1].opaque_mesh_count += 1;
                    g.opaque_meshes.push(mesh);
                    mesh = {};
                    mesh_verts_count = 0;
                };

                for (int i = 0; i < unstripped_indices.count; i += 3) {
                    mesh_verts[mesh_verts_count + 0] = verts[unstripped_indices[i + 0]];
                    mesh_verts[mesh_verts_count + 1] = verts[unstripped_indices[i + 1]];
                    mesh_verts[mesh_verts_count + 2] = verts[unstripped_indices[i + 2]];
                    assert(mesh_verts_count + 0 >= 0);
                    assert(mesh_verts_count + 2 <= 65535);
                    mesh.indices.push((uint16_t)(mesh_verts_count + 0));
                    mesh.indices.push((uint16_t)(mesh_verts_count + 0));
                    mesh.indices.push((uint16_t)(mesh_verts_count + 1));
                    mesh.indices.push((uint16_t)(mesh_verts_count + 2));
                    mesh.indices.push((uint16_t)(mesh_verts_count + 2));
                    mesh.indices.push((uint16_t)(mesh_verts_count + 2));
                    mesh_verts_count += 3;
                    if (mesh_verts_count >= 65536 - 2 ||
                        mesh.indices.count >= 65536 - 5) {
                        flush_mesh();
                        assert(mesh_verts_count == 0);
                        assert(mesh.indices.count == 0);
                    }
                }
                if (mesh_verts_count > 0) {
                    assert(mesh.indices.count > 0);
                    flush_mesh();
                }
                assert(mesh_verts_count == 0);
                assert(mesh.indices.count == 0);

                g.map_must_update = true;

                assert(positions.count);
                if (positions.count) {
                    center /= (float)positions.count;
                    g.cam_pos = center;
                    g.cam_pos.X *= 1 * SCALE;
                    g.cam_pos.Y *= -1 * SCALE;
                    g.cam_pos.Z *= -1 * SCALE;
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
                g.textures.push(tex);
                g.texture_subfiles[g.texture_subfiles.count - 1].texture_count++;
                g.map_must_update = true;
            }
        }
    auto export_to_obj = [&] {
        char *mtl_export_name = strdup(obj_export_name);
        if (!mtl_export_name) {
            MsgErr("OBJ Export Error", "Couldn't build MTL filename for \"%s\".", obj_export_name);
            return;
        }
        {
            size_t n = strlen(mtl_export_name);
            char *slash = max(strrchr(mtl_export_name, '/'), strrchr(mtl_export_name, '\\'));
            char *dot = strrchr(mtl_export_name, '.');
            if (dot <= slash || strcmp(dot, ".obj") != 0) {
                // File doesn't end with ".obj" extension -- just append .mtl instead.
                mtl_export_name = (char *)realloc(mtl_export_name, n + 4);
                if (!mtl_export_name) {
                    MsgErr("OBJ Export Error", "Couldn't build MTL filename for \"%s\".", obj_export_name);
                    return;
                }
                dot = mtl_export_name + n;
            }
            dot[0] = '.';
            dot[1] = 'm';
            dot[2] = 't';
            dot[3] = 'l';
        }
        FILE *obj = PH2CLD__fopen(obj_export_name, "w");
        if (!obj) {
            MsgErr("OBJ Export Error", "Couldn't open file \"%s\"!!", obj_export_name);
            return;
        }
        defer {
            fclose(obj);
        };
        Array<hmm_vec3> vertices = {};
        defer {
            vertices.release();
        };
        Array<hmm_vec2> uvs = {};
        defer {
            uvs.release();
        };
        Array<hmm_vec3> normals = {};
        defer {
            normals.release();
        };
        for (auto &buf : g.map_buffers) {
            if (buf.selected) {
                for (auto &vert : buf.vertices) {
                    vertices.push( { vert.position[0], vert.position[1], vert.position[2] });
                    uvs.push( { vert.uv[0], vert.uv[1] });
                    normals.push( { vert.normal[0], vert.normal[1] });
                }
            }
        }
        for (auto &buf : g.decal_buffers) {
            if (buf.selected) {
                for (auto &vert : buf.vertices) {
                    vertices.push( { vert.position[0], vert.position[1], vert.position[2] });
                    uvs.push( { vert.uv[0], vert.uv[1] });
                    normals.push( { vert.normal[0], vert.normal[1] });
                }
            }
        }
        assert(vertices.count % 3 == 0);
        fprintf(obj, "# .MAP mesh export from Psilent pHill 2 Editor (" URL ")\n");
        fprintf(obj, "\n");
        fprintf(obj, "usemtl %s\n", mtl_export_name);
        fprintf(obj, "\n");
        for (auto &v : vertices) {
            fprintf(obj, "v %f %f %f\n", v.X, v.Y, v.Z);
        }
        fprintf(obj, "\n");
        for (auto &vt : uvs) {
            fprintf(obj, "vt %f %f\n", vt.X, vt.Y);
        }
        fprintf(obj, "\n");
        for (auto &vn : normals) {
            fprintf(obj, "vn %f %f %f\n", vn.X, vn.Y, vn.Z);
        }
        fprintf(obj, "\n");
        fprintf(obj, "o hello_object\n");
        fprintf(obj, "g hello_group\n");
        fprintf(obj, "\n");
        for (int64_t i = 0; i < vertices.count; i += 3) {
            fprintf(obj, "f %lld/%lld/%lld %lld/%lld/%lld %lld/%lld/%lld\n", i + 1, i + 1, i + 1, i + 2, i + 2, i + 2, i + 3, i + 3, i + 3);
        }
    };
    if (obj_export_name) {
        export_to_obj();
    }
    if (g.show_editor) {
        ImGui::Begin("Editor", &g.show_editor, ImGuiWindowFlags_NoCollapse);
        defer {
            ImGui::End();
        };
        {
            static bool (vertices_touched[4])[UINT16_MAX] = {};
            static int (vertex_remap[4])[UINT16_MAX] = {};
            auto iterate_mesh = [&] (const char *str, int i, MAP_Mesh &mesh) {
                int num_untouched = 0;
                int num_untouched_per_buf[4] = {};
                for (auto &buf : mesh.vertex_buffers) {
                    int buf_index = (int)(&buf - mesh.vertex_buffers.data);
                    assert(buf_index >= 0);
                    assert(buf_index < 4);
                    memset(vertices_touched[buf_index], 0, buf.num_vertices);
                    int indices_index = 0;
                    for (auto &group : mesh.mesh_part_groups) {
                        for (auto &part : group.mesh_parts) {
                            for (int j = 0; j < part.strip_length * part.strip_count; j++) {
                                int vert_index = mesh.indices[indices_index];
                                if ((int)group.section_index == buf_index) {
                                    vertices_touched[buf_index][vert_index] = true;
                                }
                                ++indices_index;
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
                    ImGui::PushStyleColor(ImGuiCol_Text, 0xe00d3bff);
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
                            memmove(buf.data +  vert_index_to_remove      * buf.bytes_per_vertex,
                                    buf.data + (vert_index_to_remove + 1) * buf.bytes_per_vertex,
                                    (buf.num_vertices - vert_index_to_remove - 1) * buf.bytes_per_vertex);
                            buf.data = (char *)realloc(buf.data, buf.num_vertices * buf.bytes_per_vertex);
                            assert(buf.data);
                            // Ordered removal from the vertices_touched array
                            memmove(&vertices_touched[buf_index][vert_index_to_remove],
                                    &vertices_touched[buf_index][vert_index_to_remove + 1],
                                    (buf.num_vertices - vert_index_to_remove - 1) * sizeof(vertices_touched[buf_index][0]));
                            --buf.num_vertices;
                        } else {
                            ++vert_index_to_remove;
                        }
                    }
                }
            };
            for (int i = 0; i < g.opaque_meshes.count; i++) {
                iterate_mesh("Opaque Mesh", i, g.opaque_meshes[i]);
            }
            for (int i = 0; i < g.transparent_meshes.count; i++) {
                iterate_mesh("Transparent Mesh", i, g.transparent_meshes[i]);
            }
            for (int i = 0; i < g.decal_meshes.count; i++) {
                iterate_mesh("Decal Mesh", i, g.decal_meshes[i]);
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
        ImGui::Separator();
        ImGui::Checkbox("MAP Textured", &g.textured); ImGui::SameLine(); ImGui::Checkbox("MAP Lit", &g.lit);
        if (ImGui::CollapsingHeader("MAP Geometries", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (g.map_buffers_count + g.decal_buffers_count <= 0) {
                ImGui::BeginDisabled();
            }
            ImGui::Indent();
            defer {
                if (g.map_buffers_count + g.decal_buffers_count <= 0) {
                    ImGui::EndDisabled();
                }
                ImGui::Unindent();
            };
            bool all_buffers_shown = g.map_buffers_count + g.decal_buffers_count > 0;
            bool all_buffers_selected = g.map_buffers_count + g.decal_buffers_count > 0;
            for (int i = 0; i < g.map_buffers_count; i++) {
                if (!g.map_buffers[i].shown) {
                    all_buffers_shown = false;
                }
                if (!g.map_buffers[i].selected) {
                    all_buffers_selected = false;
                }
            }
            for (int i = 0; i < g.decal_buffers_count; i++) {
                if (!g.decal_buffers[i].shown) {
                    all_buffers_shown = false;
                }
                if (!g.decal_buffers[i].selected) {
                    all_buffers_selected = false;
                }
            }
            if (all_buffers_shown ? ImGui::Button("Hide All") : ImGui::Button("Show All")) {
                for (int i = 0; i < g.map_buffers_count; i++) {
                    g.map_buffers[i].shown = !all_buffers_shown;
                }
                for (int i = g.map_buffers_count; i < g.map_buffers_max; i++) {
                    g.map_buffers[i].shown = true;
                }
                for (int i = 0; i < g.decal_buffers_count; i++) {
                    g.decal_buffers[i].shown = !all_buffers_shown;
                }
                for (int i = g.decal_buffers_count; i < g.decal_buffers_max; i++) {
                    g.decal_buffers[i].shown = true;
                }
            }
            ImGui::SameLine(); if (all_buffers_selected ? ImGui::Button("Select None") : ImGui::Button("Select All")) {
                for (int i = 0; i < g.map_buffers_count; i++) {
                    g.map_buffers[i].selected = !all_buffers_selected;
                }
                for (int i = g.map_buffers_count; i < g.map_buffers_max; i++) {
                    g.map_buffers[i].selected = false;
                }
                for (int i = 0; i < g.decal_buffers_count; i++) {
                    g.decal_buffers[i].selected = !all_buffers_selected;
                }
                for (int i = g.decal_buffers_count; i < g.decal_buffers_max; i++) {
                    g.decal_buffers[i].selected = false;
                }
                // @Note: Bleh!!!
                g.overall_center_needs_recalc = true;
            }
            MAP_Geometry_Buffer_Source prev_source = MAP_Geometry_Buffer_Source::Opaque;
            auto map_buffer_ui = [&] (MAP_Geometry_Buffer &buf) {
                auto &meshes = get_meshes(buf);
                const char *source = "I made it up";
                switch (buf.source) {
                    case MAP_Geometry_Buffer_Source::Opaque: { source = "Opaque"; } break;
                    case MAP_Geometry_Buffer_Source::Transparent: { source = "Transparent"; } break;
                    case MAP_Geometry_Buffer_Source::Decal: { source = "Decal"; } break;
                    default: { assert(false); } break;
                }
                if (buf.source != prev_source) {
                    ImGui::Separator();
                }
                prev_source = buf.source;

                char b[512]; snprintf(b, sizeof b, "Geo #%d (ID %d), %s Mesh #%d, group #%d", buf.global_geometry_index, buf.id, source, buf.global_mesh_index, buf.mesh_part_group_index);
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;
                if (buf.selected) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                ImGui::Checkbox("", &buf.shown);
                ImGui::SameLine(); bool ret = ImGui::TreeNodeEx(b, flags);
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    bool orig = buf.selected;
                    if (!ImGui::GetIO().KeyShift) {
                        for (auto &buf2 : g.map_buffers) {
                            buf2.selected = false;
                        }
                        for (auto &buf2 : g.decal_buffers) {
                            buf2.selected = false;
                        }
                    }
                    buf.selected = !orig;
                    // @Note: Bleh.
                    g.overall_center_needs_recalc = true;
                }
                if (!ret) {
                    return;
                }
                defer {
                    ImGui::TreePop();
                };
                
                ImGui::Indent();
                defer {
                    ImGui::Unindent();
                    ImGui::NewLine();
                };

                if (!g.map_must_update) { // @HACK !!!!!!!!!!!!!!!! @@@@@@ !!!!!!!!
                    auto &mesh = meshes[buf.global_mesh_index];
                    auto &mesh_part_group = mesh.mesh_part_groups[buf.mesh_part_group_index];
                    int mat_index = mesh_part_group.material_index;
                    if (ImGui::InputInt("Material Index", &mat_index)) {
                        while (mat_index < 0) {
                            mat_index += (int)g.materials.count + !g.materials.count;
                        }
                        if (g.materials.count > 0) {
                            mat_index %= g.materials.count;
                        }
                        mesh_part_group.material_index = mat_index;
                        g.map_must_update = true;
                    }
                }
            };
            for (int i = 0; i < g.map_buffers_count; i++) {
                ImGui::PushID("MAP Opaque Mesh Selection Nodes");
                ImGui::PushID(i);
                defer {
                    ImGui::PopID();
                    ImGui::PopID();
                };
                auto &buf = g.map_buffers[i];
                map_buffer_ui(buf);
            }
            for (int i = 0; i < g.decal_buffers_count; i++) {
                ImGui::PushID("MAP Transparent/Decal Mesh Selection Nodes");
                ImGui::PushID(i);
                defer {
                    ImGui::PopID();
                    ImGui::PopID();
                };
                auto &buf = g.decal_buffers[i];
                map_buffer_ui(buf);
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
                    g.cld_must_update = true;
                }
            }
            ImGui::Separator();
            int num_map_bufs_selected = 0;
            for (auto &buf : g.map_buffers) {
                if (buf.selected) {
                    num_map_bufs_selected++;
                }
            }
            for (auto &buf : g.decal_buffers) {
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
                    int mpg_index = buf.mesh_part_group_index;
                    auto &mesh = meshes[buf.global_mesh_index];
                    mesh.bbox_override = false;
                    auto &mesh_part_group = mesh.mesh_part_groups[mpg_index];
                    auto &buf = mesh.vertex_buffers[mesh_part_group.section_index];
                    int indices_index = 0;
                    for (int i = 0; i < mpg_index; i++) {
                        for (auto &part : mesh.mesh_part_groups[i].mesh_parts) {
                            indices_index += part.strip_length * part.strip_count;
                        }
                    }
                    // Log("Starting at index %d...", indices_index);
                    assert(indices_index < mesh.indices.count);
                    int indices_to_process = 0;
                    for (auto &part : mesh_part_group.mesh_parts) {
                        indices_to_process += part.strip_length * part.strip_count;
                    }
                    // Log("We're going to process %d indices...", indices_to_process);
                    assert(indices_index + indices_to_process <= mesh.indices.count);
                    // Log("Scaling by %f, %f, %f", g.scaling_factor.X, g.scaling_factor.Y, g.scaling_factor.Z);
                    static bool vert_referenced[65536];
                    int unique_indices_count = 0;
                    memset(vert_referenced, 0, sizeof(vert_referenced));
                    hmm_vec3 center = {};
                    for (int i = 0; i < indices_to_process; i++) {
                        int index = mesh.indices.data[indices_index + i];
                        if (!vert_referenced[index]) {
                            vert_referenced[index] = true;
                            unique_indices_count++;
                            float (*position)[3] = (float(*)[3])(buf.data + buf.bytes_per_vertex * index);
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
                    g.map_must_update = true;
                    // Log("Moved/Scaled!");
                    if (get_center) {
                        center /= (float)unique_indices_count + !unique_indices_count;
                        g.overall_center += center;
                        overall_center_sum += 1;
                    }
                }
                if (duplicate) {
                    int mpg_index = buf.mesh_part_group_index;
                    auto &mesh = meshes[buf.global_mesh_index];
                    auto &buf = mesh.vertex_buffers[mesh.mesh_part_groups[mpg_index].section_index];
                    int indices_index = 0;
                    for (int i = 0; i < mpg_index; i++) {
                        for (auto &part : mesh.mesh_part_groups[i].mesh_parts) {
                            indices_index += part.strip_length * part.strip_count;
                        }
                    }
                    // Log("Starting at index %d...", indices_index);
                    assert(indices_index < mesh.indices.count);
                    int indices_to_process = 0;
                    for (auto &part : mesh.mesh_part_groups[mpg_index ].mesh_parts) {
                        indices_to_process += part.strip_length * part.strip_count;
                    }
                    // Log("We're going to process %d indices...", indices_to_process);
                    assert(indices_index + indices_to_process <= mesh.indices.count);

                    hmm_vec3 center = {};

                    // @Note: I think this is a GGGAAAARRRRRBBBBBAAAGGGEE way to dupe a mesh.
                    //        Yikes! It would be nice to have a better way to do so later.
                    MAP_Mesh new_mesh = {};
                    new_mesh.indices.reserve(indices_to_process);
                    auto &new_vertex_buffer = *new_mesh.vertex_buffers.push();
                    new_vertex_buffer.bytes_per_vertex = buf.bytes_per_vertex;
                    new_vertex_buffer.data = (char *)malloc(indices_to_process * buf.bytes_per_vertex);
                    assert(new_vertex_buffer.data);

                    for (int i = 0; i < indices_to_process; i++) {
                        int vert_index = mesh.indices[indices_index + i];
                        switch (buf.bytes_per_vertex) {
                            case 0x14: {
                                auto verts_in = (PH2MAP__Vertex14 *)buf.data;
                                auto verts_out = (PH2MAP__Vertex14 *)new_vertex_buffer.data;
                                verts_out[new_vertex_buffer.num_vertices] = verts_in[vert_index];
                            } break;
                            case 0x18: {
                                auto verts_in = (PH2MAP__Vertex18 *)buf.data;
                                auto verts_out = (PH2MAP__Vertex18 *)new_vertex_buffer.data;
                                verts_out[new_vertex_buffer.num_vertices] = verts_in[vert_index];
                            } break;
                            case 0x20: {
                                auto verts_in = (PH2MAP__Vertex20 *)buf.data;
                                auto verts_out = (PH2MAP__Vertex20 *)new_vertex_buffer.data;
                                verts_out[new_vertex_buffer.num_vertices] = verts_in[vert_index];
                            } break;
                            case 0x24: {
                                auto verts_in = (PH2MAP__Vertex24 *)buf.data;
                                auto verts_out = (PH2MAP__Vertex24 *)new_vertex_buffer.data;
                                verts_out[new_vertex_buffer.num_vertices] = verts_in[vert_index];
                            } break;
                        }
                        new_mesh.indices.push((uint16_t)(new_vertex_buffer.num_vertices));
                        new_vertex_buffer.num_vertices++;
                    }
                    assert(new_vertex_buffer.num_vertices == indices_to_process);
                    auto &new_group = *new_mesh.mesh_part_groups.push();
                    new_group.material_index = mesh.mesh_part_groups[mpg_index].material_index;
                    for (auto &part : mesh.mesh_part_groups[mpg_index].mesh_parts) {
                        new_group.mesh_parts.push(part);
                    }
                    meshes.push(new_mesh);
                    g.geometries[g.geometries.count - 1].opaque_mesh_count += 1;
                    g.map_must_update = true;
                    // Log("Duped!");
                }
            };
            for (auto &buf : g.map_buffers) {
                if (buf.selected) {
                    process_mesh(buf, false, false, false, g.overall_center_needs_recalc);
                }
            }
            for (auto &buf : g.decal_buffers) {
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
            for (auto &buf : g.decal_buffers) {
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

            auto clear_out_mesh_part_group_for_deletion = [&] (MAP_Geometry_Buffer &buf) {
                auto &meshes = get_meshes(buf);
                int mpg_index = buf.mesh_part_group_index;
                MAP_Mesh &mesh = meshes[buf.global_mesh_index];
                MAP_Mesh_Part_Group &mesh_part_group = mesh.mesh_part_groups[mpg_index];
                int indices_index = 0;
                for (int i = 0; i < mpg_index; i++) {
                    for (auto &part : mesh.mesh_part_groups[i].mesh_parts) {
                        indices_index += part.strip_length * part.strip_count;
                    }
                }
                // Log("Starting at index %d...", indices_index);
                assert(indices_index < mesh.indices.count);
                int indices_to_process = 0;
                for (auto &part : mesh_part_group.mesh_parts) {
                    indices_to_process += part.strip_length * part.strip_count;
                }
                // Log("We're going to process %d indices...", indices_to_process);
                assert(indices_index + indices_to_process <= mesh.indices.count);
                mesh.indices.remove_ordered(indices_index, indices_to_process);
                mesh_part_group.mesh_parts.release();
                mesh_part_group = {};
            };
            if (del) {
                for (auto &buf : g.map_buffers) {
                    if (buf.selected) {
                        clear_out_mesh_part_group_for_deletion(buf);
                        buf.selected = false;
                    }
                }
                for (auto &buf : g.decal_buffers) {
                    if (buf.selected) {
                        clear_out_mesh_part_group_for_deletion(buf);
                        buf.selected = false;
                    }
                }
                g.map_must_update = true;
                g.overall_center_needs_recalc = true;
            }
            auto prune_empty_mesh_part_groups = [&] (MAP_Mesh &mesh) {
                for (int64_t mesh_part_group_index = 0; mesh_part_group_index < mesh.mesh_part_groups.count;) {
                    if (mesh.mesh_part_groups[mesh_part_group_index].mesh_parts.count <= 0) {
                        mesh.mesh_part_groups.remove_ordered(mesh_part_group_index);
                    } else {
                        ++mesh_part_group_index;
                    }
                }
            };
            for (auto &mesh : g.opaque_meshes) {
                prune_empty_mesh_part_groups(mesh);
            }
            for (auto &mesh : g.transparent_meshes) {
                prune_empty_mesh_part_groups(mesh);
            }
            for (auto &mesh : g.decal_meshes) {
                prune_empty_mesh_part_groups(mesh);
            }
            auto prune_empty_meshes = [&] (Array<MAP_Mesh> &meshes, MAP_Geometry_Buffer_Source source) {
                // @Note: BRO this SUUUUCKS!!!!!!!!! please let there eventually be a way that this isn't bogged
                int64_t rolling_geo_index = 0;
                int64_t rolling_geo_start = 0;
                for (int64_t mesh_index = 0; mesh_index < meshes.count;) {
                    MAP_Mesh &mesh = meshes[mesh_index];
                    assert(rolling_geo_index < g.geometries.count);
                    uint32_t *count = nullptr;
                    do {
                        auto &geo = g.geometries[rolling_geo_index];
                        if (source == MAP_Geometry_Buffer_Source::Opaque) {
                            count = &geo.opaque_mesh_count;
                        } else if (source == MAP_Geometry_Buffer_Source::Transparent) {
                            count = &geo.transparent_mesh_count;
                        } else if (source == MAP_Geometry_Buffer_Source::Decal) {
                            count = &geo.decal_count;
                        }
                        if (mesh_index >= rolling_geo_start + *count) {
                            rolling_geo_start += *count;
                            rolling_geo_index++;
                            assert(rolling_geo_index < g.geometries.count);
                        }
                    } while (mesh_index >= rolling_geo_start + *count);
                    auto &geo = g.geometries[rolling_geo_index];
                    if (mesh.mesh_part_groups.count <= 0) {
                        meshes.remove_ordered(mesh_index);
                        --*count;
                    } else {
                        ++mesh_index;
                    }
                }
            };
            prune_empty_meshes(g.opaque_meshes, MAP_Geometry_Buffer_Source::Opaque);
            prune_empty_meshes(g.transparent_meshes, MAP_Geometry_Buffer_Source::Transparent);
            prune_empty_meshes(g.decal_meshes, MAP_Geometry_Buffer_Source::Decal);
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
                if (strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", localtime(&backup_time))) {
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
                            Log("Relying on existing backup \"%s\".", newest_valid_backup_name8);
                            return true;
                        };
                        if (check_latest_backup()) {
                            backup = false;
                        }
                        uint16_t *bak_filename16 = utf8_to_utf16(bak_filename);
                        if (bak_filename16) {
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
                                if (MessageBoxA((HWND)sapp_win32_get_hwnd(),
                                    "A backup of this file couldn't be written to disk.\n\nDo you want to save, and overwrite the file, without any backup?",
                                    "Save Backup Failed",
                                    MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL) == IDYES) {
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
                                    if (MessageBoxA((HWND)sapp_win32_get_hwnd(),
                                        "The file couldn't be written to disk.\n\nDo you want to attempt to restore from a backup?",
                                        "Save Failed",
                                        MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL) == IDYES) {
                                        // Attempt to recover file contents by overwriting with the backup.
                                        if (restore_from_backup()) {
                                            // If we had written a backup just now, prompt to delete it.
                                            if (backup_succeeded) {
                                                if (MessageBoxA((HWND)sapp_win32_get_hwnd(),
                                                    "The file was restored from a freshly made backup.\nDo you want to delete this redundant backup?",
                                                    "Restore Succeded",
                                                    MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL) == IDYES) {
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
    // @Hack: the map updates after Imgui gets an Image() call, but before Imgui renders,
    //        so we just make it never call Image() on a frame that reuploads textures
    if (g.map_must_update) {
        g.texture_ui_selected = -1;
    }
    if (g.show_textures) {
        ImGui::SetNextWindowPos(ImVec2 { 60, sapp_height() * 0.98f - 280 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(256, 256), ImGuiCond_FirstUseEver);
        ImGui::Begin("Textures", &g.show_textures, ImGuiWindowFlags_NoCollapse);
        defer {
            ImGui::End();
        };
        auto size = ImGui::GetWindowSize();
        {
            ImGui::BeginChild("texture_list", ImVec2(80, size.y - 50));
            defer {
                ImGui::EndChild();
            };
            for (int i = 0; i < g.textures.count; i++) {
                char b[16]; snprintf(b, sizeof b, "ID %d", g.textures[i].id);
                if (ImGui::Selectable(b, g.texture_ui_selected == i)) {
                    if (g.texture_ui_selected == i) {
                        g.texture_ui_selected = -1;
                    } else {
                        g.texture_ui_selected = i;
                    }
                }
            }
        }
        ImGui::SameLine(0, -1);
        if (g.texture_ui_selected >= 0) {
            ImGui::BeginChild("texture_panel");
            ImGui::PushID(g.texture_ui_selected);
            defer {
                ImGui::PopID();
                ImGui::EndChild();
            };
            
            if (ImGui::Button("Delete")) {
                // Perform index patching on subfiles.
                int rolling_texture_index = 0;
                for (auto &sub : g.texture_subfiles) {
                    int start = rolling_texture_index;
                    int end = rolling_texture_index + sub.texture_count;
                    if (g.texture_ui_selected >= start && g.texture_ui_selected < end) {
                        sub.texture_count--;
                        break;
                    }
                    rolling_texture_index = end;
                }
                g.textures[g.texture_ui_selected].release();
                g.textures.remove_ordered(g.texture_ui_selected);
                if (g.texture_ui_selected >= g.textures.count) {
                    g.texture_ui_selected = -1;
                }
            } else {
                auto &tex = g.textures[g.texture_ui_selected];
                char *dds_export_path = nullptr;
                ImGui::SameLine(); if (ImGui::Button("Export DDS...")) {
                    dds_export_path = win_import_or_export_dialog(L"DDS Texture File\0" "*.dds\0"
                                                                  "All Files\0" "*.*\0",
                                                                  L"Save DDS", false);
                }
                auto write = [&] {
                    FILE *f = PH2CLD__fopen(dds_export_path, "wb");
                    if (f) {
                        defer {
                            fclose(f);
                        };
                        assert(fwrite("DDS\x20", 4, 1, f) == 1);
                        my_dds_header header = {};
                        header.size = 0x7c;
                        header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
                        header.height = tex.height;
                        header.width = tex.width;
                        header.pitch_or_linear_size = tex.pixel_bytes;
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
                        assert(fwrite(tex.pixel_data, tex.pixel_bytes, 1, f) == 1);
                        MsgInfo("DDS Export", "Exported!");
                    } else {
                        MsgErr("DDS Export Error", "Couldn't open file \"%s\"!!", dds_export_path);
                    }
                };
                if (dds_export_path) {
                    write();
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
                        new_tex.id = tex.id;
                        new_tex.material = tex.material;
                        new_tex.sprite_count = tex.sprite_count;
                        memcpy(new_tex.sprite_metadata, tex.sprite_metadata, sizeof(tex.sprite_metadata));
                        tex.release();
                        tex = new_tex;
                        g.map_must_update = true;
                    }
                }
                // Semi-@Hack: If we import and fudge up all the textures on reupload,
                //             we don't wanna send Imgui a stale texture id.
                if (!import_success) {
                    {
                        int x = tex.id;
                        ImGui::InputInt("ID", &x);
                        x = clamp(x, 0, 65535);
                        tex.id = (uint16_t)x;
                    }
                    {
                        int x = tex.material;
                        ImGui::InputInt("\"Material\" (?)", &x);
                        x = clamp(x, 0, 255);
                        tex.material = (uint8_t)x;
                    }
                    
                    float w = (float)tex.width;
                    float h = (float)tex.height;
                    float aspect = w / h;
                    w = size.x - 99; // yucky hack
                    h = size.y - 49; // yucky hack
                    if (w > size.x - 100) {
                        w = size.x - 100;
                        h = w / aspect;
                    }
                    if (h > size.y - 50) {
                        h = size.y - 50;
                        w = h * aspect;
                    }
                    if (w > 0 && h > 0) {
                        ImGui::Image((ImTextureID)(uintptr_t)tex.image.id, ImVec2(w, h));
                    }
                }
            }
        }
    }
    if (g.show_materials) {
        ImGui::SetNextWindowPos(ImVec2 { 60 + 256 + 20, sapp_height() * 0.98f - 280 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(512, 256), ImGuiCond_FirstUseEver);
        ImGui::Begin("Materials", &g.show_materials, ImGuiWindowFlags_NoCollapse);
        defer {
            ImGui::End();
        };
        int delete_mat = -1;
        for (int i = 0; i < g.materials.count; i++) {
            ImGui::PushID("Material iteration");
            ImGui::PushID(i);
            ImGui::Text("Material #%d", i); {
                if (g.materials.count > 1) {
                    ImGui::SameLine();
                    if (ImGui::Button("Delete###Delete material")) {
                        delete_mat = i;
                    }
                }

                ImGui::Indent();
                defer {
                    ImGui::Unindent();
                };
                auto &mat = g.materials[i];
                ImGui::Columns(2);
                {
                    int x = mat.mode;
                    ImGui::InputInt("Mode", &x);
                    x = clamp(x, -32768, 32767);
                    mat.mode = (int16_t)x;
                }
                ImGui::NextColumn();
                {
                    int x = mat.texture_id;
                    ImGui::InputInt("TexID", &x);
                    x = clamp(x, 0, 65535);
                    mat.texture_id = (uint16_t)x;
                }
                ImGui::Columns(1);
                {
                    auto c = ImGui::ColorConvertU32ToFloat4(mat.material_color);
                    ImGui::ColorEdit4("Color", &c.x);
                    mat.material_color = ImGui::ColorConvertFloat4ToU32(c);
                }
                {
                    auto c = ImGui::ColorConvertU32ToFloat4(mat.overlay_color);
                    ImGui::ColorEdit4("Overlay Color", &c.x);
                    mat.overlay_color = ImGui::ColorConvertFloat4ToU32(c);
                }
                ImGui::DragFloat("Specularity", &mat.specularity);
            }
            ImGui::PopID();
            ImGui::PopID();
        }
        if (delete_mat >= 0) {
            g.materials.remove_ordered(delete_mat);
            // @Todo @@@: Index patching on EVERY SINGLE mesh group!!!!!!!!!!!!!!!!! @@@
        }
        if (ImGui::Button("New Material")) {
            MAP_Material mat = {};
            assert(g.materials.count > 0);
            mat.subfile_index = g.materials[g.materials.count - 1].subfile_index;
            mat.mode = 1;
            mat.texture_id = 0;
            mat.material_color = 0xffffffff;
            mat.overlay_color = 0;
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
            ImGui::Columns(2);
            ImGui::SliderAngle("Camera FOV", &g.fov, FOV_MIN * (360 / TAU32), FOV_MAX * (360 / TAU32));
            ImGui::NextColumn();
            if (ImGui::Button("Reset Camera")) {
                g.cam_pos = {};
                g.pitch = 0;
                g.yaw = 0;
            }
            ImGui::SameLine(); ImGui::Text("(%.0f, %.0f, %.0f)", g.cam_pos.X / SCALE, g.cam_pos.Y / -SCALE, g.cam_pos.Z / -SCALE);
            ImGui::Columns(1);
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
    if (g.map_must_update) {
        bool can_update = true;
        for (int i = 0; i < g.map_buffers_count; i++) {
            auto &buf = g.map_buffers[i];
            auto info = sg_query_buffer_info(buf.buf);
            uint64_t frame_count = sapp_frame_count();
            if (info.update_frame_index >= frame_count) {
                can_update = false;
                break;
            }
        }
        if (can_update) {
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
            float aspect_ratio = (float)cw / ch;
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
                    params.M = HMM_Scale( { 1 * SCALE, -1 * SCALE, -1 * SCALE }) * HMM_Translate(-g.cld_origin);
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
            fs_params.textured = g.textured;
            fs_params.lit = g.lit;
            fs_params.do_a2c_sharpening = true;
            vs_params.cam_pos = g.cam_pos;
            vs_params.P = perspective;
            vs_params.V = HMM_Transpose(camera_rot(g)) * HMM_Translate(-g.cam_pos);
            sg_apply_pipeline(g.map_pipeline);
            for (int i = 0; i < g.map_buffers_count; i++) {
                auto &buf = g.map_buffers[i];
                if (!buf.shown) continue;
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
                {
                    fs_params.highlight_amount = 0;
                    if (buf.selected) {
                        fs_params.highlight_amount = (float)sin(g.t * TAU * 1.5f) * 0.5f + 0.5f;
                    }
                }
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_map_vs_params, SG_RANGE(vs_params));
                sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_map_fs_params, SG_RANGE(fs_params));
                sg_image tex = g.missing_texture;
                assert(buf.material_index >= 0);
                if (buf.material_index < g.materials.count) {
                    assert(g.materials[buf.material_index].texture_id >= 0);
                    assert(g.materials[buf.material_index].texture_id < 65536);
                    auto map_tex = map_get_texture_by_id(g.textures, g.materials[buf.material_index].texture_id);
                    if (map_tex) {
                        assert(map_tex->image.id);
                        tex = map_tex->image;
                    }
                }
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = buf.buf;
                    b.fs_images[0] = tex;
                    sg_apply_bindings(b);
                    sg_draw(0, (int)buf.vertices.count, 1);
                }
            }
            fs_params.do_a2c_sharpening = false;
            sg_apply_pipeline(g.decal_pipeline);
            for (int i = 0; i < g.decal_buffers_count; i++) {
                auto &buf = g.decal_buffers[i];
                if (!g.decal_buffers[i].shown) continue;
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
                {
                    fs_params.highlight_amount = 0;
                    if (buf.selected) {
                        fs_params.highlight_amount = (float)sin(g.t * TAU * 2) * 0.5f + 0.5f;
                    }
                }
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_map_vs_params, SG_RANGE(vs_params));
                sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_map_fs_params, SG_RANGE(fs_params));
                sg_image tex = g.missing_texture;
                assert(buf.material_index >= 0);
                if (buf.material_index < g.materials.count) {
                    assert(g.materials[buf.material_index].texture_id >= 0);
                    assert(g.materials[buf.material_index].texture_id < 65536);
                    auto map_tex = map_get_texture_by_id(g.textures, g.materials[buf.material_index].texture_id);
                    if (map_tex) {
                        assert(map_tex->image.id);
                        tex = map_tex->image;
                    }
                }
                {
                    sg_bindings b = {};
                    b.vertex_buffers[0] = buf.buf;
                    b.fs_images[0] = tex;
                    sg_apply_bindings(b);
                    sg_draw(0, (int)buf.vertices.count, 1);
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
    }
}

static void cleanup(void *userdata) {
    G &g = *(G *)userdata;
    g.release();
    simgui_shutdown();
    sg_shutdown();
#ifndef NDEBUG
    stb_leakcheck_dumpmem();
#endif
}

static void fail(const char *str) {
    MsgErr("Fatal Error", "There was an error during startup:\n    \"%s\"", str);
}

char g_[sizeof(G)];
sapp_desc sokol_main(int, char **) {
    sapp_desc d = {};
    d.user_data = &g_;
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
    d.win32_console_utf8 = true;
#endif
    d.html5_ask_leave_site = true;
    return d;
}
