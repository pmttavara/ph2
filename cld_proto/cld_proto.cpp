// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD

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
struct Collision_Cylinder_With_Stats {
    Collision_Cylinder cylinder;
    int touched[16];
};

typedef struct Collision_Offset_Table {
    uint32_t group_0_index_buffer_offsets[16];
    uint32_t group_1_index_buffer_offsets[16];
    uint32_t group_2_index_buffer_offsets[16];
    uint32_t group_3_index_buffer_offsets[16];
    uint32_t group_4_index_buffer_offsets[16];
    uint32_t group_0_face_buffer_offset;
    uint32_t group_1_face_buffer_offset;
    uint32_t group_2_face_buffer_offset;
    uint32_t group_3_face_buffer_offset;
    uint32_t group_4_cylinder_buffer_offset;
} Collision_Offset_Table;

typedef struct Collision_Header {
    Vector2 origin;
    uint32_t floor_group_length;
    uint32_t wall_group_length;
    uint32_t something_group_length;
    uint32_t furniture_group_length;
    uint32_t radial_group_length;
    uint32_t padding;
    Collision_Offset_Table offset_table;
} Collision_Header;
static_assert(sizeof(Collision_Offset_Table) == 0x154 && 0x154 == 85 * 4, "");
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
    group_buffers[0] = collision_read_face_buffer(f, header.offset_table.group_0_face_buffer_offset);
    group_buffers[1] = collision_read_face_buffer(f, header.offset_table.group_1_face_buffer_offset);
    group_buffers[2] = collision_read_face_buffer(f, header.offset_table.group_2_face_buffer_offset);
    group_buffers[3] = collision_read_face_buffer(f, header.offset_table.group_3_face_buffer_offset);
    group_4_buffer = collision_read_cylinder_buffer(f, header.offset_table.group_4_cylinder_buffer_offset);
    {
        assert(header.floor_group_length == (group_buffers[0].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.wall_group_length == (group_buffers[1].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.something_group_length == (group_buffers[2].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.furniture_group_length == (group_buffers[3].faces.size() + 1) * sizeof(Collision_Face));
        assert(header.radial_group_length == (group_4_buffer.cylinders.size() + 1) * sizeof(Collision_Cylinder));
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
        do_checks(header.offset_table.group_0_index_buffer_offsets, group_buffers[0].faces);
        do_checks(header.offset_table.group_1_index_buffer_offsets, group_buffers[1].faces);
        do_checks(header.offset_table.group_2_index_buffer_offsets, group_buffers[2].faces);
        do_checks(header.offset_table.group_3_index_buffer_offsets, group_buffers[3].faces);
        do_checks(header.offset_table.group_4_index_buffer_offsets, group_4_buffer.cylinders);
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
