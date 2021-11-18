
@ctype mat4 hmm_mat4

@vs vs

uniform vs_params {
    mat4 M;
    mat4 V;
    mat4 P;
};
in vec4 position;
out vec4 color;

void main() {
    gl_Position = P * V * M * position;
    // gl_Position = P * V * M * position;
    color = vec4(1, 1, 1, 1);
}
@end

@fs fs
in vec4 color;
out vec4 frag_color;
void main() {
    frag_color = vec4(1, 1, 1, 1);
}
@end

@program main vs fs
