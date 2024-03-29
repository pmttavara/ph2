
@ctype vec2 HMM_Vec2
@ctype vec3 HMM_Vec3
@ctype vec4 HMM_Vec4
@ctype mat4 HMM_Mat4

@vs cld_vs

uniform vs_params {
    mat4 M;
    mat4 V;
    mat4 P;
    vec3 cam_pos;
};
in vec4 position;
out vec3 worldpos;
out vec3 cam_relative_pos;

void main() {
    worldpos = (M * position).xyz;
    cam_relative_pos = (M * position).xyz - cam_pos;
    gl_Position = P * V * M * position;
    //gl_PointSize = 5.0;
}
@end

@fs cld_fs
in vec3 worldpos;
in vec3 cam_relative_pos;
out vec4 frag_color;

void main() {
    vec3 N = normalize(cross(dFdx(cam_relative_pos.xyz), dFdy(cam_relative_pos.xyz)));
    vec3 L = normalize(vec3(-1, -10, -4));
    float light = clamp(dot(N, L), 0.1, 1);
    frag_color.rgb = light * vec3(1, 0.5, 0.5);
    // frag_color *= vec4(fract(worldpos), 1);
    //frag_color = vec4(0.5, 1, 0.5, 0.1);
    frag_color.a = 1;
}
@end

@program cld cld_vs cld_fs

@vs map_vs

uniform map_vs_params {
    mat4 M;
    mat4 V;
    mat4 P;
    vec3 cam_pos;
    vec3 displacement;
    vec3 scaling_factor;
    vec3 overall_center;
};
in vec4 in_position;
in vec3 in_normal;
in vec4 in_color;
in vec2 in_uv;
out vec3 worldpos;
out vec3 cam_relative_pos;
out vec3 normal;
out vec4 color;
out vec2 uv;

void main() {
    vec4 edited_world_pos = in_position;
    edited_world_pos.xyz -= overall_center;
    edited_world_pos.xyz *= scaling_factor;
    edited_world_pos.xyz += overall_center;
    edited_world_pos.xyz += displacement;
    edited_world_pos = M * edited_world_pos;
    worldpos = edited_world_pos.xyz;
    cam_relative_pos = edited_world_pos.xyz - cam_pos;
    gl_Position = P * V * edited_world_pos;
    normal = in_normal;
    color = in_color.bgra;
    uv = in_uv;
}
@end

@fs map_fs
uniform map_fs_params {
    vec4 material_diffuse_color_bgra;
    float textured;
    float use_colours;
    float shaded;
    float do_a2c_sharpening;
    float highlight_amount;
};
in vec3 worldpos;
in vec3 cam_relative_pos;
in vec3 normal;
in vec4 color;
in vec2 uv;
out vec4 frag_color;
uniform sampler2D tex;
void main() {
    frag_color.rgba = vec4(1);
    if (textured == 0 && shaded != 0) {
        vec3 N = normal;
        if (length(N) == 0) {
            N = normalize(cross(dFdx(cam_relative_pos.xyz), dFdy(cam_relative_pos.xyz)));
        }
        {
            vec3 L = normalize(vec3(-1, -10, -4));
            float light = clamp(dot(N, L), 0.1, 1);
            frag_color.rgb *= vec3(light) * vec3(1, 1, 1);
        }
        {
            vec3 L = normalize(vec3(-1, 10, 4));
            float light = clamp(dot(N, L), 0.1, 1);
            frag_color.rgb += vec3(light) * vec3(0.1, 0.1, 0.1);
        }
    }
    if (textured != 0) {
        frag_color.rgba = texture(tex, uv);
        if (do_a2c_sharpening != 0) {
            frag_color.a = (frag_color.a - 0.05) / max(fwidth(frag_color.a), 0.0001) + 0.05;
        }
    }
    frag_color.rgba *= sqrt(material_diffuse_color_bgra.zyxw);
    if (use_colours != 0) {
        frag_color.rgb *= color.rgb;
    }
    // frag_color = vec4((uv + 1) / 3, 0, 1);
    // frag_color = vec4(uv, 0, 1);
    // frag_color = color;
    // frag_color = vec4(N * 0.5 + 0.5, 1);

    // frag_color *= vec4(fract(worldpos), 1);
    //frag_color = vec4(0.5, 1, 0.5, 0.1);
    frag_color.rgb = mix(frag_color.rgb, vec3(0.93, 0.39, 0.008), highlight_amount);
    frag_color = clamp(frag_color, 0, 1);
}
@end

@program map map_vs map_fs

@vs highlight_vertex_circle_vs
uniform highlight_vertex_circle_vs_params {
    mat4 MVP;
    vec4 in_color;
};
in vec4 position;
out vec3 worldpos;
out vec2 uv;
out vec4 color;
void main() {
    uv = position.xy;
    color = in_color;
    gl_Position = MVP * position;
}
@end
@fs highlight_vertex_circle_fs
in vec3 worldpos;
in vec2 uv;
in vec4 color;
out vec4 frag_color;
void main() {
    float alpha = 1 - sqrt(uv.x * uv.x + uv.y * uv.y);
    alpha = clamp(alpha / max(fwidth(alpha), 0.0001), 0, 1);
    {
        float inner_alpha = 0.5 - sqrt(uv.x * uv.x + uv.y * uv.y);
        inner_alpha = clamp(inner_alpha / max(fwidth(inner_alpha), 0.0001), 0, 1);
        alpha -= inner_alpha;
    }
    frag_color = color * vec4(1, 1, 1, alpha);
}
@end
@program highlight_vertex_circle highlight_vertex_circle_vs highlight_vertex_circle_fs