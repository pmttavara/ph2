// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD

#ifndef defer
struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable: 4201)

#include <math.h>
#include <stdio.h>
#include <io.h>
#include <vector>

#undef assert
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#include <Windows.h>
static int assert_(const char *s) {
    int x = MessageBoxA(nullptr, s, "Assert Fired", 0x2102);
    if (x == 3) ExitProcess(1);
    return x == 4;
}
#define assert_2(LINE) #LINE
#define assert_1(LINE) assert_2(LINE)
#define assert(e) ((e) || assert_("At " __FILE__ ":" assert_1(__LINE__) ":\n\n" #e "\n\nPress Retry to debug.") && (__debugbreak(), 0))


struct Vector2 {
    float e[2];
};
union Vector3 {
    float e[3] = {};
    struct {
        float x, y, z;
    };
    Vector3() = default;
    Vector3(float x, float y, float z) : x{x}, y{y}, z{z} {}
};
Vector3 operator-(Vector3 v) { return { -v.x, -v.y, -v.z }; }
Vector3 operator+(Vector3 a, Vector3 b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
Vector3 operator-(Vector3 a, Vector3 b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
union Vector4 {
    float e[4] = {};
    struct {
        float x, y, z, w;
    };
    Vector3 xyz;
    Vector4() : e{} {}
};
float vector3_length(Vector3 v) {
    return sqrtf(v.e[0] * v.e[0] + v.e[1] * v.e[1] + v.e[2] * v.e[2]);
}

enum Collision_Shape_Type {
    COLLISION_TRI = 0,
    COLLISION_QUAD = 1,
    COLLISION_CYLINDER = 3,
};

typedef struct Collision_Shape_Header {
    uint8_t present;
    uint8_t shape;
    uint16_t padding0;
    uint32_t weight; // always 4
    uint32_t material;
    uint32_t padding1;
} Collision_Shape_Header;

// sometimes known as "Hitpoly Plane"
typedef struct Collision_Face {
    // header.shape is always COLLISION_QUAD or COLLISION_TRI
    Collision_Shape_Header header;
    Vector4 vertices[4]; // w always 1
} Collision_Face;
static_assert(sizeof(Collision_Face) == 0x50, "");
struct Collision_Face_With_Stats {
    Collision_Face face;
    int touched[16];
};

// sometimes known as "Hitpoly Column"
typedef struct Collision_Cylinder {
    // header.shape is always COLLISION_CYLINDER
    Collision_Shape_Header header;
    Vector4 position; // w always 1
    Vector3 height; // x,z always 0
    float radius;
} Collision_Cylinder;
static_assert(sizeof(Collision_Cylinder) == 0x30, "");
struct Collision_Cylinder_With_Stats {
    Collision_Cylinder cylinder;
    int touched[16];
};

typedef struct Collision_Header {
    Vector2 origin;
    // Group 0 is floors, 1 is walls, 2 is something, 3 is furniture, 4 is cylinders (different data structure)
    uint32_t group_lengths[5];
    uint32_t padding;
    uint32_t group_index_buffer_offsets[5][16];
    uint32_t group_collision_buffer_offsets[5];
} Collision_Header;
static_assert(sizeof(Collision_Header) == 0x174, "");

struct Collision_Index_Buffer {
    std::vector<uint32_t> indices = {};
};
struct Collision_Face_Buffer {
    std::vector<Collision_Face_With_Stats> faces = {};
};
struct Collision_Cylinder_Buffer {
    std::vector<Collision_Cylinder_With_Stats> cylinders = {};
};

void sanity_check_float(float f) {
    assert(isfinite(f) && fabsf(f) < 400000);
}

#define Read(f, var) assert(fread(&var, 1, sizeof(var), f) == sizeof(var))
Collision_Index_Buffer collision_read_index_buffer(FILE *f, uint32_t start_offset) {
    Collision_Index_Buffer buf = {};
    // @Todo: assert the offset is within bounds! :BoundsCheck
    fseek(f, start_offset, SEEK_SET); // :BoundsCheck
    for (;;) {
        uint32_t idx = 0;
        Read(f, idx); // :BoundsCheck
        if (idx == 0xffffffff) break;
        buf.indices.push_back(idx);
    }
    return buf;
}
int num_faces = 0;
int num_tris = 0;
int num_cylinders = 0;

int material_values_face[256];
int material_values_cylinder[256];

Collision_Face_Buffer collision_read_face_buffer(FILE *f, uint32_t start_offset) {
    Collision_Face_Buffer buf = {};
    assert(fseek(f, start_offset, SEEK_SET) == 0);
    for (;;) {
        Collision_Face face = {};
        Read(f, face);
        if (face.header.present == 1) { // :SanityCheck face
            assert(face.header.shape == COLLISION_QUAD || face.header.shape == COLLISION_TRI);
            assert(face.header.weight == 4);
            assert((face.header.material & ~0x0f) == 0 || face.header.material == 99);
            material_values_face[face.header.material]++;
            assert(face.header.padding0 == 0);
            assert(face.header.padding1 == 0);

            num_faces++;
            if (face.header.shape == COLLISION_TRI) {
                num_tris++;
                // :SanityCheck tri
                assert(face.vertices[3].e[0] == 0); // 4th vertex unused
                assert(face.vertices[3].e[1] == 0); // 4th vertex unused
                assert(face.vertices[3].e[2] == 0); // 4th vertex unused
            } else {
                // Quads can be trapezoids/etc., so they can't be implicitly encoded via 3 vertices.
                //Vector3 _0 = face.vertices[0].xyz;
                //Vector3 _1 = face.vertices[1].xyz;
                //Vector3 _2 = face.vertices[2].xyz;
                //Vector3 _3_encoded = -(_1 - _0) + (_2 - _0) + _0;
                //Vector3 disp = face.vertices[3].xyz - _3_encoded;
                //float distance = vector3_length(disp);
                //assert(distance < 1.0e-04f);
            }
            // :SanityCheck vertices
            for (auto v : face.vertices) {
                assert(v.e[3] == 1); // w always 1
                for (auto coord : v.e) {
                    sanity_check_float(coord);
                }
            }
        } else {
            // :SanityCheck sentinel
            Collision_Face zero = {};
            assert(memcmp(&face, &zero, sizeof(face)) == 0);
            break;
        }
        buf.faces.push_back( Collision_Face_With_Stats{ face, {} } );
    }
    return buf;
}
Collision_Cylinder_Buffer collision_read_cylinder_buffer(FILE *f, uint32_t start_offset) {
    Collision_Cylinder_Buffer buf = {};
    fseek(f, start_offset, SEEK_SET); // :BoundsCheck
    for (;;) {
        Collision_Cylinder cylinder = {};
        Read(f, cylinder);
        if (cylinder.header.present == 1) {
            num_cylinders++;
            // :SanityCheck cylinder
            assert(cylinder.header.shape == COLLISION_CYLINDER);
            assert(cylinder.header.weight == 4);
            assert((cylinder.header.material & ~0x0f) == 0 || cylinder.header.material == 99);
            material_values_cylinder[cylinder.header.material]++;
            assert(cylinder.header.padding0 == 0);
            assert(cylinder.header.padding1 == 0);
            for (auto coord : cylinder.position.e) {
                sanity_check_float(coord);
            }
            for (auto coord : cylinder.height.e) {
                sanity_check_float(coord);
            }
            assert(cylinder.position.e[3] == 1);
            assert(cylinder.height.e[0] == 0);
            assert(cylinder.height.e[2] == 0);
            assert(cylinder.radius > 0);
        } else {
            // :SanityCheck sentinel
            Collision_Cylinder zero = {};
            assert(memcmp(&cylinder, &zero, sizeof(cylinder)) == 0);
            break;
        }
        buf.cylinders.push_back(Collision_Cylinder_With_Stats { cylinder, {} });
    }
    return buf;
}

int total_surfaces = 0;
int total_surface_references = 0;
int total_surface_references_by_distinct_subgroups = 0;
void check_cld_file(FILE *f, bool *only_first_subgroup, bool *discontiguous, bool *nonmonotonic) {
    Collision_Header header = {};
    Read(f, header);
    { // :SanityCheck header
        sanity_check_float(header.origin.e[0]);
        sanity_check_float(header.origin.e[1]);
    }
    Collision_Face_Buffer group_buffers[4] = {};
    Collision_Cylinder_Buffer group_4_buffer = {};
    group_buffers[0] = collision_read_face_buffer(f, header.group_collision_buffer_offsets[0]);
    group_buffers[1] = collision_read_face_buffer(f, header.group_collision_buffer_offsets[1]);
    group_buffers[2] = collision_read_face_buffer(f, header.group_collision_buffer_offsets[2]);
    group_buffers[3] = collision_read_face_buffer(f, header.group_collision_buffer_offsets[3]);
    group_4_buffer = collision_read_cylinder_buffer(f, header.group_collision_buffer_offsets[4]);
    {
        assert(header.group_lengths[0] == (group_buffers[0].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.group_lengths[1] == (group_buffers[1].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.group_lengths[2] == (group_buffers[2].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.group_lengths[3] == (group_buffers[3].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.group_lengths[4] == (group_4_buffer.cylinders.size() + 1) * sizeof(Collision_Cylinder));
    }
    for (int i = 0; i < 16; i++) {
        auto do_checks = [&] (auto & offsets, auto & group_buffer_items) {
            Collision_Index_Buffer buf = collision_read_index_buffer(f, offsets[i]);
            if (i > 0) {
                if (!buf.indices.empty()) {
                    *only_first_subgroup = false;
                }
            }
            uint32_t monotonicityChecker = 0;
            for (size_t j = 0; j < buf.indices.size(); j++) {
                auto idx = buf.indices[j];
                if (j == 0) {
                    monotonicityChecker = idx;
                }
                assert(idx < group_buffer_items.size());
                if (idx < monotonicityChecker) {
                    *nonmonotonic = true;
                }
                if (idx > monotonicityChecker + 1) {
                    *discontiguous = true;
                }
                monotonicityChecker = idx;
                group_buffer_items[idx].touched[i]++;
            }
        };
        do_checks(header.group_index_buffer_offsets[0], group_buffers[0].faces);
        do_checks(header.group_index_buffer_offsets[1], group_buffers[1].faces);
        do_checks(header.group_index_buffer_offsets[2], group_buffers[2].faces);
        do_checks(header.group_index_buffer_offsets[3], group_buffers[3].faces);
        do_checks(header.group_index_buffer_offsets[4], group_4_buffer.cylinders);
    }
    for (auto & buf : group_buffers) {
        for (auto c : buf.faces) {
            int sum = 0;
            int distinct = 0;
            for (int i = 0; i < 16; i++) {
                sum += c.touched[i];
                if (c.touched[i] > 0) {
                    distinct += 1;
                }
                // :SanityCheck - by being <= 1, we can encode subgroups as bitfields
                assert(c.touched[i] <= 1);
            }
            total_surfaces += 1;
            total_surface_references += sum;
            total_surface_references_by_distinct_subgroups += distinct;
        }
    }
    {
        auto & buf = group_4_buffer;
        for (auto c : buf.cylinders) {
            int sum = 0;
            int distinct = 0;
            for (int i = 0; i < 16; i++) {
                sum += c.touched[i];
                if (c.touched[i] > 0) {
                    distinct += 1;
                }
                // :SanityCheck - by being <= 1, we can encode subgroups as bitfields
                assert(c.touched[i] <= 1);
            }
            total_surfaces += 1;
            total_surface_references += sum;
            total_surface_references_by_distinct_subgroups += distinct;
        }
    }
    { // Test Write File - Roundtrippability
        fseek(f, 0, SEEK_END);
        int file_length = ftell(f);
        char *file_data = (char *)malloc(file_length);
        defer {
            free(file_data);
        };
        fseek(f, 0, SEEK_SET);
        assert(fread(file_data, 1, file_length, f) == file_length);
        
        Collision_Header new_header = {};
        new_header.origin = header.origin;
        new_header.group_lengths[0] = (uint32_t)(group_buffers[0].faces.size() + 1) * sizeof(Collision_Face);
        new_header.group_lengths[1] = (uint32_t)(group_buffers[1].faces.size() + 1) * sizeof(Collision_Face);
        new_header.group_lengths[2] = (uint32_t)(group_buffers[2].faces.size() + 1) * sizeof(Collision_Face);
        new_header.group_lengths[3] = (uint32_t)(group_buffers[3].faces.size() + 1) * sizeof(Collision_Face);
        new_header.group_lengths[4] = (uint32_t)(group_4_buffer.cylinders.size() + 1) * sizeof(Collision_Cylinder);
        int index_buffer_lengths[5][16] = {};
        for (int group = 0; group < 4; group++) {
            for (auto c : group_buffers[group].faces) {
                for (int subgroup = 0; subgroup < 16; subgroup++) {
                    assert(c.touched[subgroup] <= 1);
                    if (c.touched[subgroup]) {
                        index_buffer_lengths[group][subgroup]++;
                    }
                }
            }
        }
        {
            for (auto c : group_4_buffer.cylinders) {
                for (int subgroup = 0; subgroup < 16; subgroup++) {
                    assert(c.touched[subgroup] <= 1);
                    if (c.touched[subgroup]) {
                        index_buffer_lengths[4][subgroup]++;
                    }
                }
            }
        }
        // just @Test code
        for (int subgroup = 0; subgroup < 16; subgroup++) {
            auto do_checks = [&] (int group, auto & offsets) {
                Collision_Index_Buffer buf = collision_read_index_buffer(f, offsets[subgroup]);
                assert(index_buffer_lengths[group][subgroup] == buf.indices.size());
            };
            do_checks(0, header.group_index_buffer_offsets[0]);
            do_checks(1, header.group_index_buffer_offsets[1]);
            do_checks(2, header.group_index_buffer_offsets[2]);
            do_checks(3, header.group_index_buffer_offsets[3]);
            do_checks(4, header.group_index_buffer_offsets[4]);
        }
        int running_offset = sizeof(Collision_Header);
        for (int group = 0; group < 5; group++) {
            for (int subgroup = 0; subgroup < 16; subgroup++) {
                new_header.group_index_buffer_offsets[group][subgroup] = running_offset;
                // Add sentinel value
                uint32_t index_buffer_length_in_bytes = (index_buffer_lengths[group][subgroup] + 1) * sizeof(uint32_t);
                running_offset += index_buffer_length_in_bytes;
            }
        }
        // @Important! SH2 .CLD files round up the start of the first collision buffers to the next 16 byte boundary.
        // (And therefore, all the later buffers also start at 16 bytes since the size of the collision shape is 80 which is divisible by 16.)
        // The intervening padding bytes are filled with 0.
        // If the start is already rounded, another 16 bytes gets added on. Don't ask me why.
        running_offset += 16;
        running_offset &= ~15;
        for (int group = 0; group < 5; group++) {
            new_header.group_collision_buffer_offsets[group] = running_offset;
            uint32_t collision_buffer_length_in_bytes = new_header.group_lengths[group];
            running_offset += collision_buffer_length_in_bytes;
        }
        int new_file_length = running_offset;
        assert(new_file_length == file_length);
        // just @Test code
        {
            assert(header.origin.e[0] == new_header.origin.e[0]);
            assert(header.origin.e[1] == new_header.origin.e[1]);
            assert(new_header.padding == 0);
            for (int group = 0; group < 5; group++) {
                for (int subgroup = 0; subgroup < 16; subgroup++) {
                    assert(new_header.group_index_buffer_offsets[group][subgroup] == header.group_index_buffer_offsets[group][subgroup]);
                }
                assert(new_header.group_collision_buffer_offsets[group] == header.group_collision_buffer_offsets[group]);
            }
        }
        char *new_file_data = (char *)malloc(new_file_length);
        defer {
            free(new_file_data);
        };
        {
            char *ptr = new_file_data;
            int len = new_file_length;
            char *end = ptr + len;
#define Write(x) (assert(ptr + sizeof(x) <= end), memcpy(ptr, &x, sizeof(x)), ptr += sizeof(x))
            Write(new_header);
            for (int group = 0; group < 4; group++) {
                for (int subgroup = 0; subgroup < 16; subgroup++) {
                    for (auto &c : group_buffers[group].faces) {
                        uint32_t idx = (uint32_t)(&c - group_buffers[group].faces.data()); // semi-@Hack @SemiHack
                        assert(c.touched[subgroup] <= 1);
                        if (c.touched[subgroup]) {
                            Write(idx);
                        }
                    }
                    uint32_t sentinel = 0xffffffff;
                    Write(sentinel);
                }
            }
            {
                for (int subgroup = 0; subgroup < 16; subgroup++) {
                    for (auto &c : group_4_buffer.cylinders) {
                        uint32_t idx = (uint32_t)(&c - group_4_buffer.cylinders.data()); // semi-@Hack @SemiHack
                        assert(c.touched[subgroup] <= 1);
                        if (c.touched[subgroup]) {
                            Write(idx);
                        }
                    }
                    uint32_t sentinel = 0xffffffff;
                    Write(sentinel);
                }
            }
            { // @Important: round up to next 16 bytes
                char *new_ptr = ptr + 16;
                new_ptr = (char *)((uintptr_t)new_ptr & ~15);
                while (ptr < new_ptr) {
                    uint8_t zero = 0;
                    Write(zero);
                }
            }
            for (int group = 0; group < 4; group++) {
                for (auto &c : group_buffers[group].faces) {
                    // @Note: I'm trying to reflect my expected packed data representation here.
                    Collision_Face face = {0};
                    face.header.present = 1;
                    bool quad = (c.face.header.shape == COLLISION_QUAD);
                    face.header.shape = quad ? (uint8_t)COLLISION_QUAD : (uint8_t)COLLISION_TRI;
                    face.header.padding0 = 0;
                    face.header.weight = 4;
                    face.header.material = c.face.header.material;
                    face.header.padding1 = 0;
                    face.vertices[0].e[0] = c.face.vertices[0].e[0];
                    face.vertices[0].e[1] = c.face.vertices[0].e[1];
                    face.vertices[0].e[2] = c.face.vertices[0].e[2];
                    face.vertices[0].e[3] = 1;
                    
                    face.vertices[1].e[0] = c.face.vertices[1].e[0];
                    face.vertices[1].e[1] = c.face.vertices[1].e[1];
                    face.vertices[1].e[2] = c.face.vertices[1].e[2];
                    face.vertices[1].e[3] = 1;
                    
                    face.vertices[2].e[0] = c.face.vertices[2].e[0];
                    face.vertices[2].e[1] = c.face.vertices[2].e[1];
                    face.vertices[2].e[2] = c.face.vertices[2].e[2];
                    face.vertices[2].e[3] = 1;
                    
                    if (quad) {
                        face.vertices[3].e[0] = c.face.vertices[3].e[0];
                        face.vertices[3].e[1] = c.face.vertices[3].e[1];
                        face.vertices[3].e[2] = c.face.vertices[3].e[2];
                        face.vertices[3].e[3] = 1;
                    } else {
                        face.vertices[3].e[0] = 0;
                        face.vertices[3].e[1] = 0;
                        face.vertices[3].e[2] = 0;
                        face.vertices[3].e[3] = 1;
                    }
                    Write(face);
                }
                Collision_Face sentinel = {0};
                Write(sentinel);
            }
            {
                for (auto &c : group_4_buffer.cylinders) {
                    // @Note: I'm trying to reflect my expected packed data representation here.
                    Collision_Cylinder cylinder = {0};
                    cylinder.header.present = 1;
                    cylinder.header.shape = (uint8_t)COLLISION_CYLINDER;
                    cylinder.header.padding0 = 0;
                    cylinder.header.weight = 4;
                    cylinder.header.material = c.cylinder.header.material;
                    cylinder.header.padding1 = 0;
                    cylinder.position.e[0] = c.cylinder.position.e[0];
                    cylinder.position.e[1] = c.cylinder.position.e[1];
                    cylinder.position.e[2] = c.cylinder.position.e[2];
                    cylinder.position.e[3] = 1;
                    cylinder.height.e[0] = 0;
                    cylinder.height.e[1] = c.cylinder.height.e[1];
                    cylinder.height.e[2] = 0;
                    cylinder.radius = c.cylinder.radius;
                    Write(cylinder);
                }
                Collision_Cylinder sentinel = {0};
                Write(sentinel);
            }
            assert(ptr == end);
            for (int i = 0; i < file_length; i++) {
                assert(new_file_data[i] == file_data[i]);
            }
            assert(memcmp(new_file_data, file_data, file_length) == 0);
        }
    }
}

int main() {
    printf("CLD file start\n");
    int num_files = 0;
    int more_than_one_subgroup = 0;
    int discontiguous_subgroups = 0;
    int non_monotonic_subgroups = 0;
    struct _finddata_t find_data;
    intptr_t directory = _findfirst("cld/*.cld", &find_data);
    if (directory >= 0) {
        while (1) {
            char b[260 + sizeof("cld/")];
            snprintf(b, sizeof(b), "cld/%s", find_data.name);

            auto f = fopen(b, "rb");
            assert(f);
            bool only_first_subgroup = true;
            bool subgroups_discontiguous = false;
            bool nonmonotonic = false;
            check_cld_file(f, &only_first_subgroup, &subgroups_discontiguous, &nonmonotonic);
            num_files++;
            if (!only_first_subgroup) {
                more_than_one_subgroup++;
            }
            if (subgroups_discontiguous) {
                discontiguous_subgroups++;
            }
            if (nonmonotonic) {
                non_monotonic_subgroups++;
            }
            fclose(f);
            if (_findnext(directory, &find_data) < 0) {
                if (errno == ENOENT) break;
                else assert(false);
            }
        }
        _findclose(directory);
    }
    printf("Done. %d files, %d faces (%d tris - %d%%), %d cylinders\n", num_files, num_faces, num_tris, (num_tris * 100 / num_faces), num_cylinders);
    printf("%d files (%d%%) use more than just their first subgroup in a group\n", more_than_one_subgroup, more_than_one_subgroup * 100 / num_files);
    printf("%d files (%d%%) have discontiguous subgroups\n", discontiguous_subgroups, discontiguous_subgroups * 100 / num_files);
    printf("%d files (%d%%) have non-monotonic subgroups\n", non_monotonic_subgroups, non_monotonic_subgroups * 100 / num_files);
    printf("%d surfaces, %d references (%.2f each), %d distinct subgroup references (%.2f each)\n",
        total_surfaces,
        total_surface_references,
        (float)total_surface_references / total_surfaces,
        total_surface_references_by_distinct_subgroups,
        (float)total_surface_references_by_distinct_subgroups / total_surfaces
    );
    for (int i = 0; i < 256; i++) {
        auto v = material_values_face[i];
        if (v) {
            printf("Collision faces had material value of 0x%02x %d times (%d%%)\n", i, v, v * 100 / num_faces);
        }
    }
    for (int i = 0; i < 256; i++) {
        auto v = material_values_cylinder[i];
        if (v) {
            printf("Collision cylinders had material value of 0x%02x %d times (%d%%)\n", i, v, v * 100 / num_cylinders);
        }
    }
}
