// SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
// SPDX-License-Identifier: 0BSD

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdio.h>
#include <io.h>
#include <vector>

#undef assert
static int assert_(const char *s) {
    // int x = MessageBoxA((void *)0, s, "Assert Fired", 0x2102);
    // if (x == 3) ExitProcess(1);
    // return x == 4;
    (void)s;
    return true;
}
#define assert_2(LINE) #LINE
#define assert_1(LINE) assert_2(LINE)
#define assert(e) ((e) || assert_("At " __FILE__ ":" assert_1(__LINE__) ":\n\n" #e "\n\nPress Retry to debug.") && (__debugbreak(), 0))


typedef struct Vector2 {
    float e[2];
} Vector2;
typedef struct Vector3 {
    float e[3];
} Vector3;
typedef struct Vector4 {
    float e[4];
} Vector4;

typedef struct Collision_Face {
    uint32_t flags;
    uint32_t always4;
    uint32_t unknown;
    uint32_t padding;
    Vector4 vertices[4]; // w always 1
} Collision_Face;

typedef struct Collision_Cylinder {
    uint32_t flags;
    uint32_t always4;
    uint32_t unknown;
    uint32_t padding;
    Vector4 position; // w always 1
    Vector3 height; // x,z always 0
    float radius;
} Collision_Cylinder;

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
    std::vector<Collision_Face> faces = {};
};
struct Collision_Cylinder_Buffer {
    std::vector<Collision_Cylinder> cylinders = {};
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

int unknown_values_face[256];
int unknown_values_cylinder[256];

Collision_Face_Buffer collision_read_face_buffer(FILE *f, uint32_t start_offset) {
    Collision_Face_Buffer buf = {};
    assert(fseek(f, start_offset, SEEK_SET) == 0);
    for (;;) {
        Collision_Face face = {};
        Read(f, face);
        if (face.flags & 0x1) { // :SanityCheck face
            assert((face.flags & ~0x101) == 0);
            assert(face.always4 == 4);
            assert((face.unknown & ~0x0f) == 0 || face.unknown == 99);
            unknown_values_face[face.unknown]++;
            assert(face.padding == 0);

            num_faces++;
            if (!(face.flags & 0x100)) {
                num_tris++;
                // :SanityCheck tri
                assert(face.vertices[3].e[0] == 0); // 4th vertex unused
                assert(face.vertices[3].e[1] == 0); // 4th vertex unused
                assert(face.vertices[3].e[2] == 0); // 4th vertex unused
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
        buf.faces.push_back(face);
    }
    return buf;
}
Collision_Cylinder_Buffer collision_read_cylinder_buffer(FILE *f, uint32_t start_offset) {
    Collision_Cylinder_Buffer buf = {};
    fseek(f, start_offset, SEEK_SET); // :BoundsCheck
    for (;;) {
        Collision_Cylinder cylinder = {};
        Read(f, cylinder);
        if (cylinder.flags & 1) {
            num_cylinders++;
            // :SanityCheck cylinder
            assert((cylinder.flags & ~0x301) == 0);
            assert(cylinder.always4 == 4);
            assert((cylinder.unknown & ~0x0f) == 0 || cylinder.unknown == 99);
            unknown_values_cylinder[cylinder.unknown]++;
            assert(cylinder.padding == 0);
            assert((cylinder.flags & 0x300) == 0x300);
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
        buf.cylinders.push_back(cylinder);
    }
    return buf;
}

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
            }
        };
        do_checks(header.offset_table.group_0_index_buffer_offsets, group_buffers[0].faces);
        do_checks(header.offset_table.group_1_index_buffer_offsets, group_buffers[1].faces);
        do_checks(header.offset_table.group_2_index_buffer_offsets, group_buffers[2].faces);
        do_checks(header.offset_table.group_3_index_buffer_offsets, group_buffers[3].faces);
        do_checks(header.offset_table.group_4_index_buffer_offsets, group_4_buffer.cylinders);
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
    for (int i = 0; i < 256; i++) {
        auto v = unknown_values_face[i];
        if (v) {
            printf("Collision faces had unknown value of 0x%02x %d times (%d%%)\n", i, v, v * 100 / num_faces);
        }
    }
    for (int i = 0; i < 256; i++) {
        auto v = unknown_values_cylinder[i];
        if (v) {
            printf("Collision cylinders had unknown value of 0x%02x %d times (%d%%)\n", i, v, v * 100 / num_cylinders);
        }
    }
}
