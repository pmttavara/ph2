
@ctype vec3 hmm_vec3
@ctype mat4 hmm_mat4

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
    frag_color = vec4(light);
    // frag_color *= vec4(fract(worldpos), 1);
    //frag_color = vec4(0.5, 1, 0.5, 0.1);
}
@end

@program cld cld_vs cld_fs

@vs map_vs
	
uniform vs_params {
	mat4 M;
	mat4 V;
	mat4 P;
	vec3 cam_pos;
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
	worldpos = (M * in_position).xyz;
	cam_relative_pos = (M * in_position).xyz - cam_pos;
	gl_Position = P * V * M * in_position;
	//gl_PointSize = 5.0;
	normal = in_normal;
	color = in_color.bgra;
	uv = in_uv;
}
@end
	
@fs map_fs
in vec3 worldpos;
in vec3 cam_relative_pos;
in vec3 normal;
in vec4 color;
in vec2 uv;
out vec4 frag_color;
void main() {
	vec3 N = normal; 
	if (length(N) == 0) {
		N = normalize(cross(dFdx(cam_relative_pos.xyz), dFdy(cam_relative_pos.xyz)));
	}
	vec3 L = normalize(vec3(-1, -10, -4));
	float light = clamp(dot(N, L), 0, 1);
	frag_color = color;
	// frag_color += vec4(light);
	// frag_color = vec4((uv + 1) / 3, 0, 1);
	// frag_color = vec4(uv, 0, 1);
	// frag_color = color;
	// frag_color = vec4(N * 0.5 + 0.5, 1);

	// frag_color *= vec4(fract(worldpos), 1);
	//frag_color = vec4(0.5, 1, 0.5, 0.1);
	frag_color = clamp(frag_color, 0, 1);
}
@end

@program map map_vs map_fs
