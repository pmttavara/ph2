// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD
// Parts of this code come from the Sokol libraries by Andre Weissflog
// which are licensed under zlib/libpng license.
#include <assert.h>
#include <math.h>

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#define SOKOL_IMPL
#define SOKOL_D3D11
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "triangle-sapp.glsl.h"

#define PH2CLD_IMPLEMENTATION
#include "../cld/ph2_cld.h"

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

// application state
static struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_bindings bind2;
    sg_pass_action pass_action;
    float rx;
    float ry;
    float camx;
    float camy;
    float camz;
    float zoom = 32000;
    bool entered = false;
    size_t num_verts = 0;
    PH2CLD_Collision_Data data = {};
} state;

static void init(void) {
    auto &data = state.data;
    data = PH2CLD_get_collision_data_from_file("../cld/cld/ap58.cld");
    assert(data.valid);

    state.camx = -data.origin[0];
    state.camz = -data.origin[1];

    {
        sg_desc d = {};
        d.context = sapp_sgcontext();
        sg_setup(d);
    }
    // __dbgui_setup(sapp_sample_count());
    float triangle[] = {
        // positions
        0.0f,  0.5f, 0,
        0.5f, -0.5f, 0,
        -0.5f, -0.5f, 0,
    };
    {
        sg_buffer_desc d = {};
        d.data = SG_RANGE(triangle);
        d.label = "triangle-vertices";
        state.bind2.vertex_buffers[0] = sg_make_buffer(d);
    }
    // a vertex buffer with 3 vertices
    //float vertices[] = {
        // positions
    //    0.0f,  0.5f, 0.5f,
    //    0.5f, -0.5f, 0.5f,
    //    -0.5f, -0.5f, 0.5f,
//
    //    1.0f,  0.5f, 0.5f,
    //    1.5f, -0.5f, 0.5f,
    //    0.5f, -0.5f, 0.5f,
//
    //    0.0f,  0.5f, 0.5f+1,
    //    0.5f, -0.5f, 0.5f+1,
    //    -0.5f, -0.5f, 0.5f+1,
    //    
    //    0.0f,  0.5f, 0.5f+1,
    //    0.5f, -0.5f, 0.5f+1,
    //    -0.5f, -0.5f, 0.5f+1,
    //    
    //    0.0f,  0.5f, 0.5f + 100,
    //    0.5f, -0.5f, 0.5f + 100,
    //    -0.5f, -0.5f, 0.5f + 100,
    //    
    //    0.0f,  -1, 0.5f,
    //    -100, -1, 0.5f + 100,
    //    100, -1, 0.5f + 100,
    //};
    //data.group_0_faces_count = 1;
    //data.group_1_faces_count = 1;
    //data.group_2_faces_count = 1;
    //data.group_3_faces_count = 1;
    size_t vertices_bytes =
        (data.group_0_faces_count
            + data.group_1_faces_count
            + data.group_2_faces_count
            + data.group_3_faces_count) * sizeof(float[6][3]);
    float *vertices = (float*)calloc(vertices_bytes, 1);
    float *vertex_pointer = vertices;
    auto push_face = [&] (PH2CLD_Face face) {
        assert(vertex_pointer < vertices + vertices_bytes / sizeof(float));
        auto push_vert = [&] (float (&vert)[3]) {
            vertex_pointer++[0] = vert[0];
            vertex_pointer++[0] = -vert[1];
            vertex_pointer++[0] = vert[2];
        };
        push_vert(face.vertices[0]);
        push_vert(face.vertices[1]);
        push_vert(face.vertices[2]);
        if (face.quad) {
            push_vert(face.vertices[0]);
            push_vert(face.vertices[2]);
            push_vert(face.vertices[3]);
        }
    };
    printf("%zu floors\n", data.group_0_faces_count);
    printf("%zu walls\n", data.group_1_faces_count);
    printf("%zu somethings\n", data.group_2_faces_count);
    printf("%zu furniture collider faces\n", data.group_3_faces_count);
    printf("%zu pillars\n", data.group_4_cylinders_count);
    for (size_t i = 0; i < data.group_0_faces_count; i++) {
        push_face(data.group_0_faces[i]);
    }
    for (size_t i = 0; i < data.group_1_faces_count; i++) {
        push_face(data.group_1_faces[i]);
    }
    for (size_t i = 0; i < data.group_2_faces_count; i++) {
        push_face(data.group_2_faces[i]);
    }
    for (size_t i = 0; i < data.group_3_faces_count; i++) {
        push_face(data.group_3_faces[i]);
    }
    assert(!((vertex_pointer - vertices) % 3));
    state.num_verts = (vertex_pointer - vertices) / 3;
    {
        sg_buffer_desc d = {};
        d.data = sg_range{vertices, state.num_verts * sizeof(float)};
        d.label = "cld-vertices";
        state.bind.vertex_buffers[0] = sg_make_buffer(d);
    }

    // create shader from code-generated sg_shader_desc
    sg_shader shd = sg_make_shader(triangle_shader_desc(sg_query_backend()));

    // create a pipeline object (default render states are fine for triangle)
    {
        sg_pipeline_desc d = {};
        d.shader = shd;
        d.depth.compare = SG_COMPAREFUNC_LESS;
        d.depth.write_enabled = true;
        // if the vertex layout doesn't have gaps, don't need to provide strides and offsets
        d.layout.attrs[ATTR_vs_position].format = SG_VERTEXFORMAT_FLOAT3;
        d.label = "triangle-pipeline";
        state.pip = sg_make_pipeline(d);
    }

    {
        sg_pass_action a = {};
        a.colors[0].action = SG_ACTION_CLEAR;
        a.colors[0].value = { 0.0f, 0.0f, 0.0f, 1.0f };
        // a pass action to framebuffer to black
        state.pass_action = a;
    }
}

hmm_vec4 get_rotated_direction(hmm_vec4 v) {
    hmm_mat4 rxm = HMM_Rotate(state.rx, HMM_Vec3(-1.0f, 0.0f, 0.0f));
    hmm_mat4 rym = HMM_Rotate(state.ry, HMM_Vec3(0.0f, -1.0f, 0.0f));
    v = rxm * v;
    v = rym * v;
    return v;
}
void event(const sapp_event *e) {
    if (e->type == SAPP_EVENTTYPE_MOUSE_ENTER) {
        state.entered = true;
    }
    if (e->type == SAPP_EVENTTYPE_MOUSE_LEAVE) {
        state.entered = false;
    }
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat) {
        if (state.entered) {
            if (e->key_code == SAPP_KEYCODE_ESCAPE) {
                sapp_lock_mouse(!sapp_mouse_locked());
            }
        }
    }
            if (e->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
                //float left_right   = (float)(e->key_code == SAPP_KEYCODE_D)
                //                   - (float)(e->key_code == SAPP_KEYCODE_A);
                float forward_back = (float)(e->scroll_y * 0.125f);
                auto forward = get_rotated_direction({0, 0, forward_back, 0});
                state.camx += forward[0] * 1000;
                state.camy += forward[1] * 1000;
                state.camz += forward[2] * 1000;
            }
    if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        if (sapp_mouse_locked()) {
            state.ry += e->mouse_dx * 0.0625f;
            state.rx += e->mouse_dy * 0.0625f;
        }
    }
}

hmm_mat4 infinitePerspectiveFovReverseZLH_ZO(float fov, float width, float height, float zNear) {
    float Cotangent = 1.0f / tanf(fov * (HMM_PI32 / 360.0f));
    hmm_mat4 Result = HMM_PREFIX(Mat4)();
    Result.Elements[0][0] = Cotangent / (width/height);
    Result.Elements[1][1] = Cotangent;
    Result.Elements[2][3] = -1;
    Result.Elements[2][2] = -1;
    Result.Elements[3][2] = -zNear;
    Result.Elements[3][3] = 0.0f;
    return Result;
};
void frame(void) {
    /* NOTE: the vs_params_t struct has been code-generated by the shader-code-gen */
    vs_params_t vs_params;
    const float w = sapp_widthf();
    const float h = sapp_heightf();
    {
        hmm_mat4 proj = infinitePerspectiveFovReverseZLH_ZO(90.0f, w, h, 1.0f);
        //hmm_mat4 view = HMM_LookAt(HMM_Vec3(0.0f, 1.5f, 10.0f), HMM_Vec3(0.0f, 0.0f, 0.0f), HMM_Vec3(0.0f, 1.0f, 0.0f));
        //hmm_mat4 view_proj = HMM_MultiplyMat4(proj, view);
        hmm_mat4 rxm = HMM_Rotate(state.rx, HMM_Vec3(1.0f, 0.0f, 0.0f));
        hmm_mat4 rym = HMM_Rotate(state.ry, HMM_Vec3(0.0f, 1.0f, 0.0f));
        hmm_mat4 model = HMM_MultiplyMat4(rxm, rym);
        model = HMM_MultiplyMat4(model, HMM_Translate( { state.camx, state.camy, state.camz }));
        auto mvp = HMM_MultiplyMat4(proj, model);
        memcpy(vs_params.mvp, &mvp, sizeof(vs_params.mvp));
    }
    {
        sg_pass_action a = {};
        a.colors[0].action = SG_ACTION_CLEAR;
        a.colors[0].value = {0.1098f, 0.1294f, 0.1372f, 1.0f };
        sg_begin_default_pass(a, sapp_width(), sapp_height());
    }
    sg_apply_pipeline(state.pip);
    sg_apply_bindings(&state.bind);
    sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE(vs_params));
    sg_draw(0, (int)state.num_verts, 1);
    {
        hmm_mat4 proj = infinitePerspectiveFovReverseZLH_ZO(90.0f, w, h, 0.01f);
        //hmm_mat4 view = HMM_LookAt(HMM_Vec3(0.0f, 1.5f, 10.0f), HMM_Vec3(0.0f, 0.0f, 0.0f), HMM_Vec3(0.0f, 1.0f, 0.0f));
        //hmm_mat4 view_proj = HMM_MultiplyMat4(proj, view);
        hmm_mat4 rxm = HMM_Rotate(state.rx, HMM_Vec3(1.0f, 0.0f, 0.0f));
        hmm_mat4 rym = HMM_Rotate(state.ry, HMM_Vec3(0.0f, 1.0f, 0.0f));
        hmm_mat4 model = HMM_MultiplyMat4(rxm, rym);
        model = HMM_MultiplyMat4(model, HMM_Translate(get_rotated_direction({0, 0, -10, 0}).XYZ));
        auto mvp = HMM_MultiplyMat4(proj, model);
        memcpy(vs_params.mvp, &mvp, sizeof(vs_params.mvp));
    }
    sg_apply_bindings(&state.bind2);
    sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE(vs_params));
    sg_draw(0, 3, 1);

    // __dbgui_draw();
    sg_end_pass();
    sg_commit();
}

void cleanup(void) {
    // __dbgui_shutdown();
    sg_shutdown();
}

sapp_desc sokol_main(int, char **) {
    sapp_desc d = {};
    d.win32_console_create = true;
    d.init_cb = init;
    d.event_cb = event;
    d.frame_cb = frame;
    d.cleanup_cb = cleanup;
    d.sample_count = 4;
    return d;
}