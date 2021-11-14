/* SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
   SPDX-License-Identifier: 0BSD */

#ifndef PH2CLD_H
#define PH2CLD_H
#include <stdint.h>
typedef enum PH2CLD_bool { PH2CLD_false, PH2CLD_true } PH2CLD_bool;
/*
    Reading:
    -> give me all 5 collision groups + origin from a filename (via fopen and malloc)
    -> give me all 5 collision groups + origin from a filename (via fopen) and an allocator
    -> give me all 5 collision groups + origin from a file char* (via malloc)
    -> give me all 5 collision groups + origin from a file char* and an allocator
    -> get the length of the pre-allocated buffer from a file char *
    -> given a pre-allocated buffer, give me all 5 collision groups + origin from a file char*
    
    Writing:
    -> given a filename and all 5 collision groups + origin, write out the CLD file (via fopen)
    -> given all 5 collision groups + origin, write the CLD file to memory and return the buffer (via malloc)
    -> get the length of the pre-allocated buffer from all 5 collision groups + origin
    -> given a pre-allocated buffer and all 5 collision groups + origin, write out the CLD file

    All of these can fail in several ways each and different ways each (OOM, file not found, out of bounds, bad input).
*/

typedef struct PH2CLD_Face {
    uint8_t quad; /* 1 if this face is a quad, 0 if a triangle. */
    uint8_t material; /* One of various material types in Silent Hill 2. */
    uint16_t subgroups; /* Bitfield representing occupancy within each of the 16 subgroups. */
    float vertices[4][3]; /* four vertices, each 3-dimensional. 4th vertex is (0, 0, 0) if this face is a triangle. */
} PH2CLD_Face;
typedef struct PH2CLD_Cylinder {
    uint16_t material; /* One of various material types in Silent Hill 2. Note that this would be a uint8_t if not for padding. */
    uint16_t subgroups; /* Bitfield representing occupancy within each of the 16 subgroups. */
    float position[3];
    float height;
    float radius;
} PH2CLD_Cylinder;

typedef struct PH2CLD_Collision_Data {
    PH2CLD_bool valid; /* PH2CLD_false (0) if creation failed */
    float origin[2]; /* 2-dimensional origin point of the map as saved in the header. */
    /* Note that the first 4 groups [0] ... [3] consist
       of faces (walls/floors/etc.), and the 5th group [4]
       consists of cylinders (pillars/etc.). */
    PH2CLD_Face *group_0_faces;
    size_t       group_0_faces_count;
    PH2CLD_Face *group_1_faces;
    size_t       group_1_faces_count;
    PH2CLD_Face *group_2_faces;
    size_t       group_2_faces_count;
    PH2CLD_Face *group_3_faces;
    size_t       group_3_faces_count;
    PH2CLD_Cylinder *group_4_cylinders;
    size_t           group_4_cylinders_count;
} PH2CLD_Collision_Data;
/* given a pre-allocated buffer, give me all 5 collision groups + origin from a file char* */
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_memory(
    const void *file_data,
    size_t file_bytes,
    void *collision_memory,
    size_t collision_memory_bytes);

#endif /* PH2CLD_H */

#if defined(PH2CLD_IMPLEMENTATION)
#ifndef PH2CLD_IMPLEMENTED
#define PH2CLD_IMPLEMENTED
#include <string.h>
#include <stdio.h> /* printf. This is just for testing code so they would nice to remove. */
#include <math.h> /* isfinite, fabsf. These are just for testing code so they would nice to remove. */

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++98-compat"
#endif

/* These macros only exist because of warnings */
#if defined(__cplusplus) && __cplusplus > 199711L
#define PH2CLD_NULL nullptr
#elif defined(__cplusplus)
#define PH2CLD_NULL 0
#else
#define PH2CLD_NULL (void *)0
#endif
#ifdef __cplusplus
#define PH2CLD_cast(T, e) static_cast<T>(e)
#define PH2CLD_reinterpret_cast(T, e) reinterpret_cast<T>(e)
#else
#define PH2CLD_cast(T, e) (T)(e)
#define PH2CLD_reinterpret_cast(T, e) (T)(e)
#endif

/* These match the binary CLD format's data structures exactly.
   Their sizeof() will evaluate to the exact sizes of the binary structs
   in the file. */
typedef struct PH2CLD__Collision_Header {
    float origin[2];
    uint32_t group_bytes[5];
    uint32_t padding;
    uint32_t group_index_buffer_offsets[5][16];
    uint32_t group_collision_buffer_offsets[5];
} PH2CLD__Collision_Header;
typedef struct PH2CLD__Collision_Shape_Header {
    uint8_t present;
    uint8_t shape;
    uint16_t padding0;
    uint32_t weight; /* always 4 */
    uint32_t material;
    uint32_t padding1;
} PH2CLD__Collision_Shape_Header;
typedef struct PH2CLD__Collision_Face {
    PH2CLD__Collision_Shape_Header header;
    float vertices[4][4]; /* w always 1 */
} PH2CLD__Collision_Face;
typedef struct PH2CLD__Collision_Cylinder {
    PH2CLD__Collision_Shape_Header header;
    float position[4]; /* w always 1 */
    float height[3]; /* x,z always 0 */
    float radius;
} PH2CLD__Collision_Cylinder;

static PH2CLD_bool PH2CLD__sanity_check_float(float f) {
    printf("Sanity check float: %f\n", PH2CLD_cast(double, f));
    return PH2CLD_cast(PH2CLD_bool, isfinite(f) && fabsf(f) < 400000);
}
static PH2CLD_bool PH2CLD__sanity_check_float2(float f[2]) {
    return PH2CLD_cast(PH2CLD_bool, PH2CLD__sanity_check_float(f[0]) &&
                         PH2CLD__sanity_check_float(f[1]));
}

#define PH2CLD_impl_read(file_data, file_bytes, byte_index, struct_pointer, struct_size) \
    ((byte_index) + (struct_size) < (file_bytes) && (memcpy((struct_pointer), PH2CLD_cast(const char *, file_data) + (byte_index), (struct_size)), 1))

PH2CLD_Collision_Data PH2CLD_get_collision_data_from_memory(
    const void *file_data,
    size_t file_bytes,
    void *collision_memory,
    size_t collision_memory_bytes) {
    PH2CLD_Collision_Data result = {
        PH2CLD_false,
        { 0.0f, 0.0f },
        PH2CLD_NULL, 0,
        PH2CLD_NULL, 0,
        PH2CLD_NULL, 0,
        PH2CLD_NULL, 0,
        PH2CLD_NULL, 0,
    };
    PH2CLD__Collision_Header header;
    size_t collision_memory_bytes_needed;
    if (!file_data) {
        return result;
    }
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_cast(const char *, collision_memory) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_cast(const char *, collision_memory) + collision_memory_bytes) {
        return result;
    }
    if (!PH2CLD_impl_read(file_data, file_bytes, 0, &header, sizeof(header))) {
        return result;
    }
    if (!PH2CLD__sanity_check_float2(header.origin)) {
        return result;
    }
    result.group_0_faces_count = header.group_bytes[0] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_1_faces_count = header.group_bytes[1] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_2_faces_count = header.group_bytes[2] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_3_faces_count = header.group_bytes[3] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_4_cylinders_count = header.group_bytes[4] / sizeof(PH2CLD__Collision_Cylinder) - 1;
    printf("Header says Group 0 has %zu faces\n", result.group_0_faces_count);
    printf("Header says Group 1 has %zu faces\n", result.group_1_faces_count);
    printf("Header says Group 2 has %zu faces\n", result.group_2_faces_count);
    printf("Header says Group 3 has %zu faces\n", result.group_3_faces_count);
    printf("Header says Group 4 has %zu cylinders\n", result.group_4_cylinders_count);
    collision_memory_bytes_needed =
        (
            result.group_0_faces_count + 
            result.group_1_faces_count + 
            result.group_2_faces_count + 
            result.group_3_faces_count
        ) * sizeof(PH2CLD_Face) +
        result.group_4_cylinders_count * sizeof(PH2CLD_Cylinder);
    printf("That's %zu bytes overall :)\n", collision_memory_bytes_needed);
    if (collision_memory_bytes_needed > collision_memory_bytes) {
        return result;
    }
    result.valid = PH2CLD_true;
    result.group_0_faces = PH2CLD_cast(PH2CLD_Face *, collision_memory);
    result.group_1_faces = result.group_0_faces + result.group_0_faces_count;
    result.group_2_faces = result.group_1_faces + result.group_1_faces_count;
    result.group_3_faces = result.group_2_faces + result.group_2_faces_count;
    result.group_4_cylinders = PH2CLD_reinterpret_cast(PH2CLD_Cylinder *, result.group_3_faces + result.group_3_faces_count);
    return result;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* PH2CLD_IMPLEMENTED */
#endif /* PH2CLD_IMPLEMENTATION */
