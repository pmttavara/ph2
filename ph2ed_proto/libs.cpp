
#define STB_LEAKCHECK_IMPLEMENTATION
#include "common.hpp"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmisleading-indentation"
#endif

#include "imgui.cpp"
#include "imgui_draw.cpp"
#include "imgui_tables.cpp"
#include "imgui_widgets.cpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifndef NDEBUG
#define SOKOL_WIN32_FORCE_MAIN
#endif

#define WinMain sokol__WinMain
#define main sokol__main
#define SOKOL_IMPL
#define SOKOL_D3D11
//#define SOKOL_GLCORE33
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"
#include "sokol_log.h"

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

#include "zip.c"

// #include "meshoptimizer/allocator.cpp"
// #include "meshoptimizer/clusterizer.cpp"
// #include "meshoptimizer/indexcodec.cpp"
#include "meshoptimizer/indexgenerator.cpp"
// #include "meshoptimizer/overdrawanalyzer.cpp"
#include "meshoptimizer/overdrawoptimizer.cpp"
// #include "meshoptimizer/simplifier.cpp"
// #include "meshoptimizer/spatialorder.cpp"
#include "meshoptimizer/stripifier.cpp"
// #include "meshoptimizer/vcacheanalyzer.cpp"
#include "meshoptimizer/vcacheoptimizer.cpp"
// #include "meshoptimizer/vertexcodec.cpp"
// #include "meshoptimizer/vertexfilter.cpp"
// #include "meshoptimizer/vfetchanalyzer.cpp"
#include "meshoptimizer/vfetchoptimizer.cpp"
