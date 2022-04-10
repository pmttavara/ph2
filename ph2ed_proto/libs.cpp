
#define STB_LEAKCHECK_IMPLEMENTATION
#include "common.hpp"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#include "imgui.cpp"
#include "imgui_draw.cpp"
#include "imgui_tables.cpp"
#include "imgui_widgets.cpp"

#define SOKOL_IMPL
#define SOKOL_D3D11
//#define SOKOL_GLCORE33
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"

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
