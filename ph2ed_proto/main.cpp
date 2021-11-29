// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
// Dear Imgui is licensed under MIT License. https://github.com/ocornut/imgui/blob/master/LICENSE.txt
#ifndef defer
struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

#ifdef NDEBUG
#define RELEASE 1
#else
#undef RELEASE
#endif

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
#ifdef RELEASE
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
#define SOKOL_IMPL
//#define SOKOL_LOG(s) Log("%s", s)
#define SOKOL_D3D11
//#define SOKOL_GLCORE33
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"

struct LogMsg {
    unsigned int colour = IM_COL32(127,127,127,255);
    char buf[252] = {};
};
enum { LOG_MAX = 16384 };
LogMsg log_buf[LOG_MAX] = {};
int log_buf_index = 0;
#define LogC(c, fmt, ...) ((log_buf[log_buf_index % LOG_MAX].colour = (c)), (snprintf(log_buf[log_buf_index++ % LOG_MAX].buf, sizeof(LogMsg::buf), (fmt), ##__VA_ARGS__)))
#define Log(fmt, ...) LogC(IM_COL32(127,127,127,255), fmt, ##__VA_ARGS__)

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#define PH2CLD_IMPLEMENTATION
#include "../cld/ph2_cld.h"
#include <io.h> // @Debug @Temporary @Remove

// Shaders
#include "main.glsl.h"

// sokol_glue.h
SOKOL_API_IMPL sg_context_desc sapp_sgcontext(void) {
    sg_context_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.color_format = (sg_pixel_format) sapp_color_format();
    desc.depth_format = (sg_pixel_format) sapp_depth_format();
    desc.sample_count = sapp_sample_count();
    desc.gl.force_gles2 = sapp_gles2();
    desc.metal.device = sapp_metal_get_device();
    desc.metal.renderpass_descriptor_cb = sapp_metal_get_renderpass_descriptor;
    desc.metal.drawable_cb = sapp_metal_get_drawable;
    desc.d3d11.device = sapp_d3d11_get_device();
    desc.d3d11.device_context = sapp_d3d11_get_device_context();
    desc.d3d11.render_target_view_cb = sapp_d3d11_get_render_target_view;
    desc.d3d11.depth_stencil_view_cb = sapp_d3d11_get_depth_stencil_view;
    desc.wgpu.device = sapp_wgpu_get_device();
    desc.wgpu.render_view_cb = sapp_wgpu_get_render_view;
    desc.wgpu.resolve_view_cb = sapp_wgpu_get_resolve_view;
    desc.wgpu.depth_stencil_view_cb = sapp_wgpu_get_depth_stencil_view;
    return desc;
}

#ifdef _WIN32
#pragma warning(disable : 4255)
#pragma warning(disable : 4668)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
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

const float FOV_MIN = 10 * (TAU32 / 360);
const float FOV_MAX = 179 * (TAU32 / 360);
const float FOV_DEFAULT = 90 * (TAU32 / 360);
const float MOVE_SPEED_DEFAULT = -2;
const float MOVE_SPEED_MAX = 6;
struct CLD_Face_Buffer {
    sg_buffer buf = {};
    int num_vertices = 0;
};
struct G {
    double last_time = 0;
    double t = 0;

    bool orbiting = false;
    float fov = FOV_DEFAULT;

    hmm_vec3 cam_pos = {0, 0, 0};
    float yaw = 0;
    float pitch = 0;
    float move_speed = MOVE_SPEED_DEFAULT;
    float scroll_speed = MOVE_SPEED_DEFAULT;
    float scroll_speed_timer = 0;
    
    hmm_vec3 cld_origin = {};
    sg_pipeline cld_pipeline = {};
    CLD_Face_Buffer cld_face_buffers[4] = {};
};

static void cld_load(G &g, const char *filename) {
    Log("CLD filename is \"%s\"", filename);
    PH2CLD_Collision_Data cld = PH2CLD_get_collision_data_from_file(filename);
    if (!cld.valid) {
        LogC(IM_COL32(255, 127, 127, 255), "Failed loading CLD file \"%s\"!", filename);
        return;
    }
    assert(cld.valid);
    defer {
        PH2CLD_free_collision_data(cld);
    };
    Log("CLD origin is (%f, 0, %f)", cld.origin[0], cld.origin[1]);
    g.cld_origin = hmm_vec3 { cld.origin[0], 0, cld.origin[1] };
    for (int group = 0; group < 4; group++) {
        PH2CLD_Face *faces = cld.group_0_faces;
        size_t num_faces = cld.group_0_faces_count;
        if (group == 1) { faces = cld.group_1_faces; num_faces = cld.group_1_faces_count; }
        if (group == 2) { faces = cld.group_2_faces; num_faces = cld.group_2_faces_count; }
        if (group == 3) { faces = cld.group_3_faces; num_faces = cld.group_3_faces_count; }
        Log("group %d has %zu faces", group, num_faces);
        auto max_triangles = num_faces * 2;
        auto max_vertices = max_triangles * 3;
        auto max_floats = max_vertices * 3;
        if (!max_floats) {
            g.cld_face_buffers[group].num_vertices = 0;
            continue;
        }
        auto floats = (float *)malloc(max_floats * sizeof(float));
        assert(floats);
        defer {
            free(floats);
        };
        auto write_pointer = floats;
        auto end = floats + max_floats;
        for (size_t i = 0; i < num_faces; i++) {
            PH2CLD_Face face = faces[i];
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
        
        sg_update_buffer(g.cld_face_buffers[group].buf, sg_range { floats, num_floats * sizeof(float) });
        assert(num_floats % 3 == 0);
        g.cld_face_buffers[group].num_vertices = (int)num_floats / 3;
        assert(g.cld_face_buffers[group].num_vertices % 3 == 0);
    }
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
        if (memcmp("load ", buf, 5) == 0) {
            char *p = buf + 5;
            while (isspace(*p)) p++;
            cld_load(g, p);
        } else {
            Log("Unkown command :)");
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

static void init(void *userdata) {
    G &g = *(G *)userdata;
    (void)g;
    g.last_time = get_time();
    sg_desc desc = {};
    desc.context = sapp_sgcontext();
    sg_setup(&desc);
    simgui_desc_t simgui_desc = {};
    simgui_desc.no_default_font = true;
    simgui_desc.dpi_scale = sapp_dpi_scale();
    simgui_desc.sample_count = sapp_sample_count();
#if RELEASE
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
        const char *filename = "../cld/cld/ob01.cld";
        // cld_load(g, filename);
    }
    { // @Temporary @Debug @Remove
        struct _finddata_t find_data;
        intptr_t directory = _findfirst("map/*.map", &find_data);
        assert(directory >= 0);
        // while (1)
        {
            // char b[260 + sizeof("map/")];
            // snprintf(b, sizeof(b), "map/%s", find_data.name);
            // FILE *f = PH2CLD__fopen(b, "rb");
            FILE *f = PH2CLD__fopen("map/ma01.map", "rb");
            assert(f);
            defer {
                fclose(f);
            };

            // @Temporary @Debug
            static float floats_buffer[58254 * 2 * 3 * 3];
            static const int floats_max = sizeof(floats_buffer) / sizeof(floats_buffer[0]);
            int floats_count = 0;

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
#define Read(ptr, x) (assert(ptr + sizeof(x) <= end) && memcpy(&(x), ptr, sizeof(x)) && (ptr += sizeof(x)))
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
#define GetIndex() (assert(indices_index < indices_count), indices[indices_index++])
                                        for (int strip_index = 0; strip_index < outer_max; strip_index++) {
                                            int memory = GetIndex() << 0x10;
                                            int mask = 0xFFFF0000;
                                            uint16_t currentIndex = GetIndex();
                                            for (int i = 2; i < inner_max; i++) {
                                                auto get_vertex = [&] (int index) {
                                                    struct {
                                                        float position[3] = {};
                                                    } result = {};
                                                    char *vertex_ptr = vertex_buffer + index * vertex_size;
                                                    switch (vertex_size) {
                                                        case 0x14: {
                                                            PH2MAP__Vertex14 vert = {};
                                                            Read(vertex_ptr, vert);
                                                            result.position[0] = vert.position[0];
                                                            result.position[1] = vert.position[1];
                                                            result.position[2] = vert.position[2];
                                                        } break;
                                                        case 0x18: {
                                                            PH2MAP__Vertex18 vert = {};
                                                            Read(vertex_ptr, vert);
                                                            result.position[0] = vert.position[0];
                                                            result.position[1] = vert.position[1];
                                                            result.position[2] = vert.position[2];
                                                        } break;
                                                        case 0x20: {
                                                            PH2MAP__Vertex20 vert = {};
                                                            Read(vertex_ptr, vert);
                                                            result.position[0] = vert.position[0];
                                                            result.position[1] = vert.position[1];
                                                            result.position[2] = vert.position[2];
                                                        } break;
                                                        case 0x24: {
                                                            PH2MAP__Vertex24 vert = {};
                                                            Read(vertex_ptr, vert);
                                                            result.position[0] = vert.position[0];
                                                            result.position[1] = vert.position[1];
                                                            result.position[2] = vert.position[2];
                                                        } break;
                                                    }
                                                    return result;
                                                };
                                                memory = (memory & mask) + (currentIndex << (0x10 & mask));
                                                mask ^= 0xFFFFFFFF;
                                                
                                                currentIndex = GetIndex();
                                                
                                                auto triangle_v0 = get_vertex(memory >> 0x10);
                                                auto triangle_v1 = get_vertex(memory & 0xffff);
                                                auto triangle_v2 = get_vertex(currentIndex);
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v0.position[0];
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v0.position[1];
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v0.position[2];
                                                
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v1.position[0];
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v1.position[1];
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v1.position[2];
                                                
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v2.position[0];
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v2.position[1];
                                                assert(floats_count < floats_max);
                                                floats_buffer[floats_count++] = triangle_v2.position[2];
                                            }
                                        }
                                    }
                                }
                            }
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
                    ptr += subfile_header.length;
                } else {
                    assert(0);
                }
            }
            assert(end - ptr < 16);
            for (; ptr < end; ptr++) {
                assert(*ptr == 0);
            }
            if (floats_count > 0) {
                sg_update_buffer(g.cld_face_buffers[0].buf, sg_range { floats_buffer, floats_count * sizeof(float) });
                assert(floats_count % 3 == 0);
                g.cld_face_buffers[0].num_vertices = (int)floats_count / 3;
                assert(g.cld_face_buffers[0].num_vertices % 3 == 0);
                //break;
            }
            //if (_findnext(directory, &find_data) < 0) {
            //    if (errno == ENOENT) break;
            //    else assert(0);
            //}
        }
        _findclose(directory);
        fflush(stdout);
    }

    {
        sg_pipeline_desc d = {};
        d.shader = sg_make_shader(main_shader_desc(sg_query_backend()));
        d.layout.attrs[ATTR_vs_position].format = SG_VERTEXFORMAT_FLOAT3;
        d.depth.write_enabled = true;
        d.depth.compare = SG_COMPAREFUNC_GREATER;
        //d.primitive_type = SG_PRIMITIVETYPE_POINTS;
        //d.colors[0].blend.enabled = true;
        //d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        //d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        //d.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_SRC_ALPHA;
        //d.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        g.cld_pipeline = sg_make_pipeline(d);
    }
}

static hmm_mat4 camera_rot(G &g) {
    // We pitch the camera by applying a rotation around X,
    // then yaw the camera by applying a rotation around Y.
    auto pitch_matrix = HMM_Rotate(g.pitch * (360 / TAU32), HMM_Vec3(1, 0, 0));
    auto yaw_matrix = HMM_Rotate(g.yaw * (360 / TAU32), HMM_Vec3(0, 1, 0));
    return yaw_matrix * pitch_matrix;
}
static void event(const sapp_event *e_, void *userdata) {
    G &g = *(G *)userdata;
    simgui_handle_event(e_);
    const sapp_event &e = *e_;
    if (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (e.mouse_button & SAPP_MOUSEBUTTON_RIGHT) {
            g.orbiting = true;
        }
    }
    if (e.type == SAPP_EVENTTYPE_UNFOCUSED) {
        g.orbiting = false;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_UP) {
        g.orbiting = false;
    }
    if (e.type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        if (g.orbiting) {
            g.yaw += -e.mouse_dx * 3 * (0.022f * (TAU32 / 360));
            g.pitch += -e.mouse_dy * 3 * (0.022f * (TAU32 / 360));
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
    sapp_lock_mouse(g.orbiting);
    g.scroll_speed_timer -= dt;
    if (g.scroll_speed_timer < 0) {
        g.scroll_speed_timer = 0;
        g.scroll_speed = 0;
    }
    if (g.orbiting) {
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
    if (g.orbiting) {
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
    }
    imgui_do_console(g);
    //if (g.changed) {
    //    vertices;
    //    sg_update_buffer(g.cld_buffer, );
    //}
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
            sg_apply_pipeline(g.cld_pipeline);
            vs_params_t params;
            params.cam_pos = g.cam_pos;
            params.P = perspective;
            
            // The view matrix is the inverse of the camera's model matrix (aka the camera's transform matrix).
            // We perform an inversion of the camera rotation by getting the transpose
            // (which equals the inverse in this case since it's just a rotation matrix).
            params.V = HMM_Transpose(camera_rot(g)) * HMM_Translate(-g.cam_pos);
            for (int i = 0; i < 4; i++) {
                {
                    // I should ask the community what they know about the units used in the game
                    const float SCALE = 0.001f * 1.75f;
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
#if !RELEASE
    d.win32_console_create = true;
#endif
    d.html5_ask_leave_site = true;
    return d;
}
