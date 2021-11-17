@vs vs

uniform vs_params {
    mat4 mvp;
};
in vec4 position;

out vec4 color;
out vec4 pos;

void main() {
    pos = position;
    pos.x *= 2;
    // pos = mvp * position;
    // pos = position;
    vec4 vertex_pos = mvp * (position * vec4(1,1,1,1));
    gl_Position = vertex_pos;
    color = vec4(1, 1, 1, 1);
}
@end

@fs fs
in vec4 color;
in vec4 pos;
out vec4 frag_color;
float screen_to_eye_depth(float z, float near)
{
    return near / z;
}
float rand(float x) { return fract(sin(x)*100000.0); }
vec3 Tonemap(vec3 x) {
    x = x / (1 + x);
    x *= x;
    return x;
}
void main() {
    vec3 N = normalize(cross(dFdx(pos).xyz,dFdy(pos).xyz));
    //vec3 V = normalize(-pos.xyz); // eye pos
    // sunlight
    vec3 light = vec3(0.8f, 0.8f, 0.2f) * dot(N,normalize(vec3(1.0, 5.0, 2.0)));
    light += vec3(0.2f, 0.2f, 0.8f) * dot(N,normalize(vec3(-3.0, 5.0, -1.0)));
    light = Tonemap(light/2);
    frag_color = vec4(light.x, light.y, light.z, 1.0);
    //frag_color = vec4(color.x
    //    * fract(pos.x / 1000)
    //    , color.y
        //* screen_to_eye_depth(pos.z, 0.01) * 5000
    //    * fract(pos.y / 1000)
    //    , color.z
    //    * fract(pos.z / 1000)
    //    , color.w);
}
@end

@program triangle vs fs
