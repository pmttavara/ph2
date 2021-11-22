
@ctype vec3 hmm_vec3
@ctype mat4 hmm_mat4

@vs vs

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
}
@end

@fs fs
in vec3 worldpos;
in vec3 cam_relative_pos;
out vec4 frag_color;
void main() {
    vec3 N = normalize(cross(dFdx(cam_relative_pos.xyz), dFdy(cam_relative_pos.xyz)));
    vec3 L = normalize(vec3(-1, -10, +4));
    float light = clamp(dot(N, L), 0, 1);
    frag_color = vec4(light) * (1 - (gl_FragCoord.w) / 10000);
}
@end

@program main vs fs
