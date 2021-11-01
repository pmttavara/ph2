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
    size_t       group_0_faces_length;
    PH2CLD_Face *group_1_faces;
    size_t       group_1_faces_length;
    PH2CLD_Face *group_2_faces;
    size_t       group_2_faces_length;
    PH2CLD_Face *group_3_faces;
    size_t       group_3_faces_length;
    PH2CLD_Cylinder *group_4_cylinders;
    size_t           group_4_cylinders_length;
} PH2CLD_Collision_Data;
/* given a pre-allocated buffer, give me all 5 collision groups + origin from a file char* */
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_memory(
    const void *file_data,
    size_t file_length,
    void *collision_memory,
    size_t collision_memory_length);

#if defined(PH2CLD_IMPLEMENTATION)/* || !defined(PH2CLD_IMPLEMENTATION)*/
#ifndef PH2CLD_IMPLEMENTED
#define PH2CLD_IMPLEMENTED
#include <string.h>

typedef struct PH2CLD__Collision_Header {
    float origin[2];
    uint32_t group_lengths[5];
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

#define PH2CLD__read(file_data, file_length, byte_index, struct_pointer, struct_size) \
    ((byte_index) + (struct_size) < (file_length) && (memcpy((struct_pointer), (const char *)(file_data) + (byte_index), (struct_size)), 1))

PH2CLD_Collision_Data PH2CLD_get_collision_data_from_memory(
    const void *file_data,
    size_t file_length,
    void *collision_memory,
    size_t collision_memory_length) {
    PH2CLD_Collision_Data result = {0};
    PH2CLD__Collision_Header header;
    if (!file_data) {
        return result;
    }
    if ((const char *)file_data + file_length > (const char *)collision_memory &&
        (const char *)file_data < (const char*)collision_memory + collision_memory_length) {
        return result;
    }
    if (!PH2CLD__read(file_data, file_length, 0, &header, sizeof(header))) {
        return result;
    }
    result.valid = PH2CLD_true;
    return result;
}

#endif /* PH2CLD_IMPLEMENTED */
#endif /* PH2CLD_IMPLEMENTATION */

#endif /* PH2CLD_H */
