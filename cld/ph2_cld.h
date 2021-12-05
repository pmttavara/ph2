/* SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
   SPDX-License-Identifier: 0BSD */

/*
    PH2CLD - CLD file parsing library - v0.01
    "Public domain" (Zero-clause BSD) license at bottom of file.
    Filenames are UTF-8. Windows extended-paths are supported.

    QUICK START - READING:
        PH2CLD_Collision_Data data = PH2CLD_get_collision_data_from_file("ap01.cld");
        // do stuff with data
        PH2CLD_free_collision_data(data);

    QUICK START - WRITING:
        // build your collision data
        PH2CLD_bool write_result = PH2CLD_write_cld(data, "my_PH2CLD_output.cld");
        // it's your job to free collision data that you built yourself :)

    QUICK START - SAMPLE CODE:
        if (data.valid) {
            printf("\n Group 0: {\n");
            for (size_t i = 0; i < data.group_0_faces_count; i++) {
                PH2CLD_Face face = data.group_0_faces[i];
                printf("  Face %zu (", i);
                for (int bit = 15; bit >= 0; bit--) if (face.subgroups & (1 << bit)) printf("1"); else printf("o");
                printf("): (%f, %f, %f), ", face.vertices[0][0], face.vertices[0][1], face.vertices[0][2]);
                printf("(%f, %f, %f), ", face.vertices[1][0], face.vertices[1][1], face.vertices[1][2]);
                printf("(%f, %f, %f)", face.vertices[2][0], face.vertices[2][1], face.vertices[2][2]);
                if (face.quad) {
                    printf(", (%f, %f, %f)", face.vertices[3][0], face.vertices[3][1], face.vertices[3][2]);
                }
                printf("\n");
            }
            printf("}\n Group 4: {\n");
            for (size_t i = 0; i < data.group_4_cylinders_count; i++) {
                PH2CLD_Cylinder cyl = data.group_4_cylinders[i];
                printf("  Cylinder %zu (", i);
                for (int bit = 15; bit >= 0; bit--) if (cyl.subgroups & (1 << bit)) printf("1"); else printf("o");
                printf("): (%f, %f, %f), ", cyl.position[0], cyl.position[1], cyl.position[2]);
                printf("Height %f, Radius %f\n", cyl.height, cyl.radius);
            }
        } else {
            printf("Couldn't get CLD data\n");
        }

    To avoid including <stdio.h>, #define PH2CLD_NO_STDIO.
    To override allocation, either #define PH2CLD_malloc and PH2CLD_free
    or call the *_with_allocator() versions of functions.
    
    To speed up the reading/writing code by 2x-4x, #define PH2CLD_NO_SANITY_CHECKS.
    This WILL allow NaNs and other broken collision to be read/written, so use this
    with care.
 */
#ifndef PH2CLD_H
#define PH2CLD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
typedef enum PH2CLD_bool { PH2CLD_false, PH2CLD_true } PH2CLD_bool;

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

#ifndef PH2CLD_NO_STDIO
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file(const char *filename);
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_with_allocator(const char *filename, void *(*alloc_func)(size_t n, void *userdata), void (*free_func)(void *p, void *userdata), void *userdata);
#endif
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_memory(const void *file_data, size_t file_bytes);
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_memory_with_allocator(const void *file_data, size_t file_bytes, void *(*alloc_func)(size_t n, void *userdata), void *userdata);
PH2CLD_bool PH2CLD_get_collision_memory_length_from_file_memory(const void *file_data, size_t file_bytes, size_t *collision_memory_length);
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_memory_and_collision_memory(
    const void *file_data,
    size_t file_bytes,
    void *collision_memory,
    size_t collision_memory_bytes);

#ifndef PH2CLD_NO_STDIO
PH2CLD_bool PH2CLD_write_cld(PH2CLD_Collision_Data data, const char *filename);
PH2CLD_bool PH2CLD_write_cld_with_allocator(PH2CLD_Collision_Data data,
    const char *filename,
    void *(*alloc_func)(size_t n, void *userdata), void (*free_func)(void *p, void *userdata), void *userdata);
#endif
PH2CLD_bool PH2CLD_write_cld_memory(PH2CLD_Collision_Data data, void **file_ptr, size_t *file_len);
PH2CLD_bool PH2CLD_write_cld_memory_with_allocator(PH2CLD_Collision_Data data,
    void **file_ptr, size_t *file_len,
    void *(*alloc_func)(size_t n, void *userdata), void *userdata);
PH2CLD_bool PH2CLD_write_cld_filesize(PH2CLD_Collision_Data data, size_t *file_bytes_needed);
PH2CLD_bool PH2CLD_write_cld_to_memory(PH2CLD_Collision_Data data, void *file_data, size_t file_bytes);

void PH2CLD_free_collision_data(PH2CLD_Collision_Data data);
void PH2CLD_free_collision_data_with_allocator(PH2CLD_Collision_Data data, void (*free_func)(void *p, void *userdata), void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PH2CLD_H */

#if defined(PH2CLD_IMPLEMENTATION)
#ifndef PH2CLD_IMPLEMENTED
#define PH2CLD_IMPLEMENTED

#ifdef __cplusplus
extern "C" {
#define PH2CLD_EXTERN extern "C"
#else
#define PH2CLD_EXTERN extern
#endif

#include <string.h> /* memcpy, memcmp */

#if defined(PH2CLD_malloc) && defined(PH2CLD_free)
/* ok */
#elif !defined(PH2CLD_malloc) && !defined(PH2CLD_free)
/* ok */
#else
#error "Must define both or neither of PH2CLD_malloc and PH2CLD_free."
#endif

#ifndef PH2CLD_malloc
#include <stdlib.h> /* malloc, free */
#define PH2CLD_malloc(n) malloc(n)
#define PH2CLD_free(p) free(p)
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif

/* These macros only exist because of warnings. */
#ifdef __cplusplus
#define PH2CLD_cast(T, e) (static_cast<T>(e))
#define PH2CLD_reinterpret_cast(T, e) (reinterpret_cast<T>(e))
#else
#define PH2CLD_cast(T, e) ((T)(e))
#define PH2CLD_reinterpret_cast(T, e) ((T)(e))
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

#ifndef PH2CLD_NO_STDIO
#include <stdio.h> /* fopen, fclose, fseek, fread, fwrite, fgetpos, rewind, FILE, fpos_t */
/* @Attribution: STB_image.h stbi__fopen */
#if defined(_WIN32)
PH2CLD_EXTERN __declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int cp, unsigned long flags, const char *str, int cbmb, wchar_t *widestr, int cchwide);
PH2CLD_EXTERN __declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int cp, unsigned long flags, const wchar_t *widestr, int cchwide, char *str, int cbmb, const char *defchar, int *used_default);
#endif
static FILE *PH2CLD__fopen(char const *filename, char const *mode)
{
    FILE *f;
#if defined(_WIN32)
    wchar_t wMode[64];
    wchar_t wFilename[32768];
    if (0 == MultiByteToWideChar(65001 /* UTF8 */, 0, filename, -1, wFilename, sizeof(wFilename)/sizeof(*wFilename)))
        return PH2CLD_reinterpret_cast(FILE *, 0);
    
    if (0 == MultiByteToWideChar(65001 /* UTF8 */, 0, mode, -1, wMode, sizeof(wMode)/sizeof(*wMode)))
        return PH2CLD_reinterpret_cast(FILE *, 0);
    
#if defined(_MSC_VER) && _MSC_VER >= 1400
    if (0 != _wfopen_s(&f, wFilename, wMode))
        f = PH2CLD_reinterpret_cast(FILE *, 0);
#else
    f = _wfopen(wFilename, wMode);
#endif
    
#else
    f = fopen(filename, mode);
#endif
    return f;
}
#endif

static void *PH2CLD__malloc(size_t n, void *userdata) {
    PH2CLD_cast(void, userdata);
    return PH2CLD_malloc(n);
}
static void PH2CLD__free(void *p, void *userdata) {
    PH2CLD_cast(void, userdata);
    PH2CLD_free(p);
}
#ifndef PH2CLD_NO_SANITY_CHECKS
#define PH2CLD_FPCOPY(dst, src) do { int i; for (i = 0; i < 4; i++) PH2CLD_reinterpret_cast(char *, &(dst))[i] = PH2CLD_reinterpret_cast(char *, &(src))[i]; } while (0)
static PH2CLD_bool PH2CLD__sanity_check_float(float f) {
    uint32_t u = 0;
    PH2CLD_FPCOPY(u, f);
    return PH2CLD_cast(PH2CLD_bool, ~u & 0x7f800000);
}
static PH2CLD_bool PH2CLD__sanity_check_float2(float f[2]) {
    return PH2CLD_cast(PH2CLD_bool, PH2CLD__sanity_check_float(f[0]) &&
                                    PH2CLD__sanity_check_float(f[1]));
}
static PH2CLD_bool PH2CLD__sanity_check_float3(float f[3]) {
    return PH2CLD_cast(PH2CLD_bool, PH2CLD__sanity_check_float(f[0]) &&
                                    PH2CLD__sanity_check_float(f[1]) &&
                                    PH2CLD__sanity_check_float(f[2]));
}
static PH2CLD_bool PH2CLD__sanity_check_float4(float f[4]) {
    return PH2CLD_cast(PH2CLD_bool, PH2CLD__sanity_check_float(f[0]) &&
                                    PH2CLD__sanity_check_float(f[1]) &&
                                    PH2CLD__sanity_check_float(f[2]) &&
                                    PH2CLD__sanity_check_float(f[3]));
}
#endif

#define PH2CLD_read(file_data, file_bytes, byte_idx, val) \
    ((byte_idx) + sizeof(val) <= (file_bytes) && \
        (memcpy(&(val), PH2CLD_cast(const char *, file_data) + (byte_idx), sizeof(val)), 1))
#define PH2CLD_write(file_data, write_idx, val) \
    (memcpy(PH2CLD_cast(char *, file_data) + (write_idx), &(val), sizeof(val)), (write_idx) += sizeof(val))

#ifndef PH2CLD_NO_STDIO
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file(const char *filename) {
    return PH2CLD_get_collision_data_from_file_with_allocator(filename, PH2CLD__malloc, PH2CLD__free, PH2CLD_reinterpret_cast(void *, 0));
}
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_with_allocator(const char *filename, void *(*alloc_func)(size_t n, void *userdata), void (*free_func)(void *p, void *userdata), void *userdata) {
    PH2CLD_Collision_Data result;
    FILE *fp;
    fpos_t raw_file_length;
    uintmax_t checked_filesize;
    size_t file_memory_length;
    void *file_memory;
    memset(&result, 0, sizeof(result));
    if (!filename) {
        return result;
    }
    if (!alloc_func) {
        return result;
    }
    if (!free_func) {
        return result;
    }
    fp = PH2CLD__fopen(filename, "rb");
    if (!fp) {
        return result;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return result;
    }
    if (fgetpos(fp, &raw_file_length) != 0 || raw_file_length < 0) {
        fclose(fp);
        return result;
    }
    checked_filesize = PH2CLD_cast(uintmax_t, raw_file_length);
    if (checked_filesize > SIZE_MAX) {
        fclose(fp);
        return result;
    }
    file_memory_length = PH2CLD_cast(size_t, checked_filesize);
    file_memory = alloc_func(file_memory_length, userdata);
    if (!file_memory) {
        fclose(fp);
        return result;
    }
    rewind(fp);
    if (fread(file_memory, 1, file_memory_length, fp) != file_memory_length) {
        free_func(file_memory, userdata);
        fclose(fp);
        return result; /* Allocation succeeded, but read failed */
    }
    result = PH2CLD_get_collision_data_from_file_memory_with_allocator(file_memory, file_memory_length, alloc_func, userdata);
    free_func(file_memory, userdata);
    return result;
}
#endif
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_memory(const void *file_data, size_t file_bytes) {
    return PH2CLD_get_collision_data_from_file_memory_with_allocator(file_data, file_bytes, PH2CLD__malloc, PH2CLD_reinterpret_cast(void *, 0));
}
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_memory_with_allocator(const void *file_data, size_t file_bytes, void *(*alloc_func)(size_t n, void *userdata), void *userdata) {
    PH2CLD_Collision_Data result;
    size_t collision_memory_length = 0;
    void *collision_memory;
    memset(&result, 0, sizeof(result));
    if (!alloc_func) {
        return result;
    }
    if (!PH2CLD_get_collision_memory_length_from_file_memory(file_data, file_bytes, &collision_memory_length)) {
        return result;
    }
    collision_memory = alloc_func(collision_memory_length, userdata);
    if (!collision_memory) {
        return result;
    }
    return PH2CLD_get_collision_data_from_file_memory_and_collision_memory(file_data, file_bytes, collision_memory, collision_memory_length);
}
PH2CLD_bool PH2CLD_get_collision_memory_length_from_file_memory(const void *file_data, size_t file_bytes, size_t *collision_memory_length) {
    PH2CLD__Collision_Header header;
    if (!collision_memory_length) {
        return PH2CLD_false;
    }
    *collision_memory_length = 0;
    if (!file_data) {
        return PH2CLD_false;
    }
    if (!PH2CLD_read(file_data, file_bytes, 0, header)) {
        return PH2CLD_false;
    }
    *collision_memory_length =
        (
            (header.group_bytes[0] / sizeof(PH2CLD__Collision_Face) - 1) + 
            (header.group_bytes[1] / sizeof(PH2CLD__Collision_Face) - 1) + 
            (header.group_bytes[2] / sizeof(PH2CLD__Collision_Face) - 1) + 
            (header.group_bytes[3] / sizeof(PH2CLD__Collision_Face) - 1)
        ) * sizeof(PH2CLD_Face) +
        (header.group_bytes[4] / sizeof(PH2CLD__Collision_Cylinder) - 1) * sizeof(PH2CLD_Cylinder);
    return PH2CLD_true;
}
PH2CLD_Collision_Data PH2CLD_get_collision_data_from_file_memory_and_collision_memory(
    const void *file_data,
    size_t file_bytes,
    void *collision_memory,
    size_t collision_memory_bytes) {
    size_t collision_memory_bytes_needed;
    PH2CLD__Collision_Header header;
    PH2CLD_Collision_Data result;
    memset(&result, 0, sizeof(result));
    if (!file_data) return result;
    if (!collision_memory) return result;
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_cast(const char *, collision_memory) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_cast(const char *, collision_memory) + collision_memory_bytes) return result;
    if (!PH2CLD_read(file_data, file_bytes, 0, header)) return result;
#ifndef PH2CLD_NO_SANITY_CHECKS
    if (!PH2CLD__sanity_check_float2(header.origin)) return result;
#endif
    result.origin[0] = header.origin[0];
    result.origin[1] = header.origin[1];
    result.group_0_faces_count = header.group_bytes[0] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_1_faces_count = header.group_bytes[1] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_2_faces_count = header.group_bytes[2] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_3_faces_count = header.group_bytes[3] / sizeof(PH2CLD__Collision_Face) - 1;
    result.group_4_cylinders_count = header.group_bytes[4] / sizeof(PH2CLD__Collision_Cylinder) - 1;
    collision_memory_bytes_needed =
        (
            result.group_0_faces_count + 
            result.group_1_faces_count + 
            result.group_2_faces_count + 
            result.group_3_faces_count
        ) * sizeof(PH2CLD_Face) +
        result.group_4_cylinders_count * sizeof(PH2CLD_Cylinder);
    if (collision_memory_bytes_needed > collision_memory_bytes) {
        return result;
    }
    result.group_0_faces = PH2CLD_cast(PH2CLD_Face *, collision_memory);
    result.group_1_faces = result.group_0_faces + result.group_0_faces_count;
    result.group_2_faces = result.group_1_faces + result.group_1_faces_count;
    result.group_3_faces = result.group_2_faces + result.group_2_faces_count;
    result.group_4_cylinders = PH2CLD_reinterpret_cast(PH2CLD_Cylinder *, result.group_3_faces + result.group_3_faces_count);

    {
        /* Extract collision faces from face groups */
        int group = 0;
        for (; group < 4; group++) {
            size_t i = 0;
            PH2CLD_Face *faces;
            size_t faces_count;
            uint32_t base_offset = header.group_collision_buffer_offsets[group];
            /**/ if (group == 0) { faces = result.group_0_faces; faces_count = result.group_0_faces_count; }
            else if (group == 1) { faces = result.group_1_faces; faces_count = result.group_1_faces_count; }
            else if (group == 2) { faces = result.group_2_faces; faces_count = result.group_2_faces_count; }
            else /*           */ { faces = result.group_3_faces; faces_count = result.group_3_faces_count; }
            /* Parse all of the faces in the group */
            for (; i < faces_count; i++) {
                size_t file_offset = base_offset + i * sizeof(PH2CLD__Collision_Face);
                PH2CLD__Collision_Face face;
                if (!PH2CLD_read(file_data, file_bytes, file_offset, face)) return result;
#ifndef PH2CLD_NO_SANITY_CHECKS
                /* Sanity checks */
                if (face.header.shape != 0 && face.header.shape != 1) return result;
                if (face.header.weight != 4) return result;
                if ((face.header.material & PH2CLD_cast(uint32_t, ~0x0fu)) != 0 && face.header.material != 99) return result;
                if (face.header.padding0 != 0 || face.header.padding1 != 0) return result;
                if (!PH2CLD__sanity_check_float4(face.vertices[0])) return result;
                if (!PH2CLD__sanity_check_float4(face.vertices[1])) return result;
                if (!PH2CLD__sanity_check_float4(face.vertices[2])) return result;
                if (!PH2CLD__sanity_check_float4(face.vertices[3])) return result;
#endif
                if (face.vertices[0][3] != 1) return result;
                if (face.vertices[1][3] != 1) return result;
                if (face.vertices[2][3] != 1) return result;
                if (face.vertices[3][3] != 1) return result;
                if (face.header.shape == 0) {
                    if (face.vertices[3][0] != 0) return result;
                    if (face.vertices[3][1] != 0) return result;
                    if (face.vertices[3][2] != 0) return result;
                }
                /* Extract the meaningful info */
                faces[i].quad = PH2CLD_cast(uint8_t, face.header.shape == 1);
                faces[i].material = face.header.material & 0xff;
                faces[i].subgroups = 0;
                faces[i].vertices[0][0] = face.vertices[0][0];
                faces[i].vertices[0][1] = face.vertices[0][1];
                faces[i].vertices[0][2] = face.vertices[0][2];
                faces[i].vertices[1][0] = face.vertices[1][0];
                faces[i].vertices[1][1] = face.vertices[1][1];
                faces[i].vertices[1][2] = face.vertices[1][2];
                faces[i].vertices[2][0] = face.vertices[2][0];
                faces[i].vertices[2][1] = face.vertices[2][1];
                faces[i].vertices[2][2] = face.vertices[2][2];
                faces[i].vertices[3][0] = face.vertices[3][0];
                faces[i].vertices[3][1] = face.vertices[3][1];
                faces[i].vertices[3][2] = face.vertices[3][2];
            }
            { /* Sanity check the sentinel */
                size_t sentinel_offset = base_offset + faces_count * sizeof(PH2CLD__Collision_Face);
                PH2CLD__Collision_Face face;
                if (!PH2CLD_read(file_data, file_bytes, sentinel_offset, face)) return result;
#ifndef PH2CLD_NO_SANITY_CHECKS
                {
                    static const PH2CLD__Collision_Face zero = { {0, 0, 0, 0, 0, 0}, { {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0} } };
                    if (memcmp(&face, &zero, sizeof(face)) != 0) return result;
                }
#endif
            }
        }
    }
    {
        /* Extract collision cylinders from the cylinder group */
        PH2CLD_Cylinder *cylinders = result.group_4_cylinders;
        size_t cylinders_count = result.group_4_cylinders_count;
        uint32_t base_offset = header.group_collision_buffer_offsets[4];
        size_t i = 0;
        /* Parse all of the faces in the group */
        for (; i < cylinders_count; i++) {
            size_t file_offset = base_offset + i * sizeof(PH2CLD__Collision_Cylinder);
            PH2CLD__Collision_Cylinder cylinder;
            
            if (!PH2CLD_read(file_data, file_bytes, file_offset, cylinder)) return result;
#ifndef PH2CLD_NO_SANITY_CHECKS
            /* Sanity checks */
            if (cylinder.header.shape != 3) return result;
            if (cylinder.header.weight != 4) return result;
            if ((cylinder.header.material & PH2CLD_cast(uint32_t, ~0x0fu)) != 0 && cylinder.header.material != 99) return result;
            if (cylinder.header.padding0 != 0 || cylinder.header.padding1 != 0) return result;
            if (!PH2CLD__sanity_check_float4(cylinder.position)) return result;
            if (!PH2CLD__sanity_check_float3(cylinder.height)) return result;
            if (!PH2CLD__sanity_check_float(cylinder.radius)) return result;
            if (cylinder.position[3] != 1) return result;
            if (cylinder.height[0] != 0) return result;
            if (cylinder.height[2] != 0) return result;
            if (cylinder.radius <= 0) return result;
#endif
            /* Extract the meaningful info */
            cylinders[i].material = cylinder.header.material & 0xff;
            cylinders[i].subgroups = 0;
            cylinders[i].position[0] = cylinder.position[0];
            cylinders[i].position[1] = cylinder.position[1];
            cylinders[i].position[2] = cylinder.position[2];
            cylinders[i].height = cylinder.height[1];
            cylinders[i].radius = cylinder.radius;
        }
        { /* Sanity check the sentinel */
            size_t sentinel_offset = base_offset + cylinders_count * sizeof(PH2CLD__Collision_Cylinder);
            PH2CLD__Collision_Cylinder cylinder;
            if (!PH2CLD_read(file_data, file_bytes, sentinel_offset, cylinder)) return result;
#ifndef PH2CLD_NO_SANITY_CHECKS
            {
            static const PH2CLD__Collision_Cylinder zero = { {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0}, 0 };
                if (memcmp(&cylinder, &zero, sizeof(cylinder)) != 0) return result;
            }
#endif
        }
    }
    { /* Populate the face subgroup bitfields */
        int group = 0;
        for (; group < 4; group++) {
            int subgroup = 0;
            for (; subgroup < 16; subgroup++) {
                PH2CLD_Face *faces;
                size_t faces_count;
                uint32_t index_offset = header.group_index_buffer_offsets[group][subgroup];
                /**/ if (group == 0) { faces = result.group_0_faces; faces_count = result.group_0_faces_count; }
                else if (group == 1) { faces = result.group_1_faces; faces_count = result.group_1_faces_count; }
                else if (group == 2) { faces = result.group_2_faces; faces_count = result.group_2_faces_count; }
                else /*           */ { faces = result.group_3_faces; faces_count = result.group_3_faces_count; }

                for (;; index_offset += 4) {
                    uint32_t index = 0;
                    if (!PH2CLD_read(file_data, file_bytes, index_offset, index)) return result;
                    if (index == 0xffffffff) break;
                    if (index >= faces_count) return result;

                    if (faces[index].subgroups & (1 << subgroup)) return result;
                    faces[index].subgroups |= (1 << subgroup);
                }
            }
        }
    }
    { /* Populate the cylinder subgroup bitfields */
        int group = 4;
        int subgroup = 0;
        for (; subgroup < 16; subgroup++) {
            PH2CLD_Cylinder *cylinders = result.group_4_cylinders;
            size_t cylinders_count = result.group_4_cylinders_count;
            uint32_t index_offset = header.group_index_buffer_offsets[group][subgroup];

            for (; ; index_offset += 4) {
                uint32_t index = 0;
                if (!PH2CLD_read(file_data, file_bytes, index_offset, index)) return result;
                if (index == 0xffffffff) break;
                if (index >= cylinders_count) return result;

                if (cylinders[index].subgroups & (1 << subgroup)) return result;
                cylinders[index].subgroups |= (1 << subgroup);
            }
        }
    }
    
    result.valid = PH2CLD_true;
    return result;
}

static PH2CLD_bool PH2CLD__write_header_and_length(PH2CLD_Collision_Data data, PH2CLD__Collision_Header *header, size_t *file_bytes_needed) {
    size_t index_buffer_counts[5][16];
    /* Sanity check input data */
    if (!data.valid) return PH2CLD_false;
    if (!data.group_0_faces && data.group_0_faces_count > 0) return PH2CLD_false;
    if (!data.group_1_faces && data.group_1_faces_count > 0) return PH2CLD_false;
    if (!data.group_2_faces && data.group_2_faces_count > 0) return PH2CLD_false;
    if (!data.group_3_faces && data.group_3_faces_count > 0) return PH2CLD_false;
    if (!data.group_4_cylinders && data.group_4_cylinders_count > 0) return PH2CLD_false;
#ifndef PH2CLD_NO_SANITY_CHECKS
    if (!PH2CLD__sanity_check_float2(data.origin)) return PH2CLD_false;
    {
        int group = 0;
        for (; group < 4; group++) {
            size_t i = 0;
            PH2CLD_Face *faces;
            size_t faces_count;
            /**/ if (group == 0) { faces = data.group_0_faces; faces_count = data.group_0_faces_count; }
            else if (group == 1) { faces = data.group_1_faces; faces_count = data.group_1_faces_count; }
            else if (group == 2) { faces = data.group_2_faces; faces_count = data.group_2_faces_count; }
            else /*           */ { faces = data.group_3_faces; faces_count = data.group_3_faces_count; }
            for (; i < faces_count; i++) {
                if (faces[i].quad != 0 && faces[i].quad != 1) return PH2CLD_false;
                if ((faces[i].material & PH2CLD_cast(uint32_t, ~0x0fu)) != 0 && faces[i].material != 99) return PH2CLD_false;
                if (!PH2CLD__sanity_check_float3(faces[i].vertices[0])) return PH2CLD_false;
                if (!PH2CLD__sanity_check_float3(faces[i].vertices[1])) return PH2CLD_false;
                if (!PH2CLD__sanity_check_float3(faces[i].vertices[2])) return PH2CLD_false;
            }
        }
    }
    {
        size_t i = 0;
        PH2CLD_Cylinder *cylinders = data.group_4_cylinders;
        size_t cylinders_count = data.group_4_cylinders_count;
        for (; i < cylinders_count; i++) {
            if ((cylinders[i].material & PH2CLD_cast(uint32_t, ~0x0fu)) != 0 && cylinders[i].material != 99) return PH2CLD_false;
            if (!PH2CLD__sanity_check_float3(cylinders[i].position)) return PH2CLD_false;
            if (!PH2CLD__sanity_check_float(cylinders[i].height)) return PH2CLD_false;
            if (!PH2CLD__sanity_check_float(cylinders[i].radius)) return PH2CLD_false;
        }
    }
#endif
    header->origin[0] = data.origin[0];
    header->origin[1] = data.origin[1];
    header->group_bytes[0] = PH2CLD_cast(uint32_t, (data.group_0_faces_count + 1) * sizeof(PH2CLD__Collision_Face));
    header->group_bytes[1] = PH2CLD_cast(uint32_t, (data.group_1_faces_count + 1) * sizeof(PH2CLD__Collision_Face));
    header->group_bytes[2] = PH2CLD_cast(uint32_t, (data.group_2_faces_count + 1) * sizeof(PH2CLD__Collision_Face));
    header->group_bytes[3] = PH2CLD_cast(uint32_t, (data.group_3_faces_count + 1) * sizeof(PH2CLD__Collision_Face));
    header->group_bytes[4] = PH2CLD_cast(uint32_t, (data.group_4_cylinders_count + 1) * sizeof(PH2CLD__Collision_Cylinder));
    header->padding = 0;
    {
        int group = 0;
        for (; group < 4; group++) {
            PH2CLD_Face *faces;
            size_t faces_count;
            int subgroup = 0;
            /**/ if (group == 0) { faces = data.group_0_faces; faces_count = data.group_0_faces_count; }
            else if (group == 1) { faces = data.group_1_faces; faces_count = data.group_1_faces_count; }
            else if (group == 2) { faces = data.group_2_faces; faces_count = data.group_2_faces_count; }
            else /*           */ { faces = data.group_3_faces; faces_count = data.group_3_faces_count; }
            for (; subgroup < 16; subgroup++) {
                size_t i = 0;
                index_buffer_counts[group][subgroup] = 0;
                for (; i < faces_count; i++) {
                    if (faces[i].subgroups & (1 << subgroup)) {
                        index_buffer_counts[group][subgroup]++;
                    }
                }
            }
        }
    }
    {
        int group = 4;
        PH2CLD_Cylinder *cylinders = data.group_4_cylinders;
        size_t cylinders_count = data.group_4_cylinders_count;
        int subgroup = 0;
        for (; subgroup < 16; subgroup++) {
            size_t i = 0;
            index_buffer_counts[group][subgroup] = 0;
            for (; i < cylinders_count; i++) {
                if (cylinders[i].subgroups & (1 << subgroup)) {
                    index_buffer_counts[group][subgroup]++;
                }
            }
        }
    }
    {
        size_t running_offset = sizeof(PH2CLD__Collision_Header);
        {
            int group = 0;
            for (; group < 5; group++) {
                int subgroup = 0;
                for (; subgroup < 16; subgroup++) {
                    size_t index_buffer_length_in_bytes = (index_buffer_counts[group][subgroup] + 1) * sizeof(uint32_t);
                    header->group_index_buffer_offsets[group][subgroup] = PH2CLD_cast(uint32_t, running_offset);
                    running_offset += index_buffer_length_in_bytes;
                }
            }
        }
        /* Note: SH2 .CLD files round up the start of the first collision buffers to the next 16 byte boundary.
            (And therefore, all the later buffers also start at 16 bytes since the size of the collision shape is 80 which is divisible by 16.)
            The intervening padding bytes are filled with 0.
            If the start is already rounded, another 16 bytes gets added on. Don't ask me why. */
        running_offset += 16;
        running_offset &= ~15u;
        {
            int group = 0;
            for (; group < 5; group++) {
                size_t collision_buffer_length_in_bytes = header->group_bytes[group];
                header->group_collision_buffer_offsets[group] = PH2CLD_cast(uint32_t, running_offset);
                running_offset += collision_buffer_length_in_bytes;
            }
        }
        *file_bytes_needed = running_offset;
    }
    return PH2CLD_true;
}

static void PH2CLD__write_buffers(PH2CLD_Collision_Data data, PH2CLD__Collision_Header header, void *file_data) {
    size_t write_index = 0;
    PH2CLD_write(file_data, write_index, header);
    {
        int group = 0;
        for (; group < 4; group++) {
            int subgroup = 0;
            PH2CLD_Face *faces;
            size_t faces_count;
            /**/ if (group == 0) { faces = data.group_0_faces; faces_count = data.group_0_faces_count; }
            else if (group == 1) { faces = data.group_1_faces; faces_count = data.group_1_faces_count; }
            else if (group == 2) { faces = data.group_2_faces; faces_count = data.group_2_faces_count; }
            else /*           */ { faces = data.group_3_faces; faces_count = data.group_3_faces_count; }
            for (; subgroup < 16; subgroup++) {
                size_t i = 0;
                for (; i < faces_count; i++) {
                    if (faces[i].subgroups & (1 << subgroup)) {
                        uint32_t index = PH2CLD_cast(uint32_t, i);
                        PH2CLD_write(file_data, write_index, index);
                    }
                }
                {
                    static const uint32_t sentinel = 0xffffffff;
                    PH2CLD_write(file_data, write_index, sentinel);
                }
            }
        }
    }
    {
        PH2CLD_Cylinder *cylinders = data.group_4_cylinders;
        size_t cylinders_count = data.group_4_cylinders_count;
        int subgroup = 0;
        for (; subgroup < 16; subgroup++) {
            size_t i = 0;
            for (; i < cylinders_count; i++) {
                if (cylinders[i].subgroups & (1 << subgroup)) {
                    uint32_t index = PH2CLD_cast(uint32_t, i);
                    PH2CLD_write(file_data, write_index, index);
                }
            }
            {
                static const uint32_t sentinel = 0xffffffff;
                PH2CLD_write(file_data, write_index, sentinel);
            }
        }
    }
    { /* Round up to next 16 bytes. */
        size_t new_write_index = write_index + 16;
        new_write_index = new_write_index & ~15u;
        memset(PH2CLD_reinterpret_cast(char *, file_data) + write_index, 0, new_write_index - write_index);
        write_index = new_write_index;
    }
    {
        int group = 0;
        for (; group < 4; group++) {
            size_t i = 0;
            PH2CLD_Face *faces;
            size_t faces_count;
            /**/ if (group == 0) { faces = data.group_0_faces; faces_count = data.group_0_faces_count; }
            else if (group == 1) { faces = data.group_1_faces; faces_count = data.group_1_faces_count; }
            else if (group == 2) { faces = data.group_2_faces; faces_count = data.group_2_faces_count; }
            else /*           */ { faces = data.group_3_faces; faces_count = data.group_3_faces_count; }
            for (; i < faces_count; i++) {
                PH2CLD__Collision_Face face;
                face.header.present = 1;
                face.header.shape = PH2CLD_cast(uint8_t, faces[i].quad ? 1 : 0);
                face.header.padding0 = 0;
                face.header.weight = 4;
                face.header.material = faces[i].material;
                face.header.padding1 = 0;
                face.vertices[0][0] = faces[i].vertices[0][0];
                face.vertices[0][1] = faces[i].vertices[0][1];
                face.vertices[0][2] = faces[i].vertices[0][2];
                face.vertices[0][3] = 1;
                face.vertices[1][0] = faces[i].vertices[1][0];
                face.vertices[1][1] = faces[i].vertices[1][1];
                face.vertices[1][2] = faces[i].vertices[1][2];
                face.vertices[1][3] = 1;
                face.vertices[2][0] = faces[i].vertices[2][0];
                face.vertices[2][1] = faces[i].vertices[2][1];
                face.vertices[2][2] = faces[i].vertices[2][2];
                face.vertices[2][3] = 1;
                if (faces[i].quad) {
                    face.vertices[3][0] = faces[i].vertices[3][0];
                    face.vertices[3][1] = faces[i].vertices[3][1];
                    face.vertices[3][2] = faces[i].vertices[3][2];
                    face.vertices[3][3] = 1;
                } else {
                    face.vertices[3][0] = 0;
                    face.vertices[3][1] = 0;
                    face.vertices[3][2] = 0;
                    face.vertices[3][3] = 1;
                }
                PH2CLD_write(file_data, write_index, face);
            }
            {
                static const PH2CLD__Collision_Face sentinel = { {0, 0, 0, 0, 0, 0}, { {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0} } };
                PH2CLD_write(file_data, write_index, sentinel);
            }
        }
    }
    {
        size_t i = 0;
        PH2CLD_Cylinder *cylinders = data.group_4_cylinders;
        size_t cylinders_count = data.group_4_cylinders_count;
        for (; i < cylinders_count; i++) {
            PH2CLD__Collision_Cylinder cylinder;
            cylinder.header.present = 1;
            cylinder.header.shape = 3;
            cylinder.header.padding0 = 0;
            cylinder.header.weight = 4;
            cylinder.header.material = cylinders[i].material;
            cylinder.header.padding1 = 0;
            cylinder.position[0] = cylinders[i].position[0];
            cylinder.position[1] = cylinders[i].position[1];
            cylinder.position[2] = cylinders[i].position[2];
            cylinder.position[3] = 1;
            cylinder.height[0] = 0;
            cylinder.height[1] = cylinders[i].height;
            cylinder.height[2] = 0;
            cylinder.radius = cylinders[i].radius;
            PH2CLD_write(file_data, write_index, cylinder);
        }
        {
            static const PH2CLD__Collision_Cylinder sentinel = { {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0}, 0 };
            PH2CLD_write(file_data, write_index, sentinel);
        }
    }
}

#ifndef PH2CLD_NO_STDIO
PH2CLD_bool PH2CLD_write_cld(PH2CLD_Collision_Data data, const char *filename) {
    return PH2CLD_write_cld_with_allocator(data, filename, PH2CLD__malloc, PH2CLD__free, PH2CLD_reinterpret_cast(void *, 0));
}
PH2CLD_bool PH2CLD_write_cld_with_allocator(PH2CLD_Collision_Data data,
    const char *filename,
    void *(*alloc_func)(size_t n, void *userdata), void (*free_func)(void *p, void *userdata), void *userdata) {
    FILE *fp;
    void *file_data;
    size_t file_bytes;
    /* Error on invalid input data */
    if (!data.valid) {
        return PH2CLD_false;
    }
    if (!filename) {
        return PH2CLD_false;
    }
    if (!alloc_func) {
        return PH2CLD_false;
    }
    if (!free_func) {
        return PH2CLD_false;
    }
    fp = PH2CLD__fopen(filename, "wb");
    if (!fp) {
        return PH2CLD_false;
    }
    if (!PH2CLD_write_cld_memory_with_allocator(data, &file_data, &file_bytes, alloc_func, userdata)) {
        fclose(fp);
        return PH2CLD_false;
    }
    if (fwrite(file_data, 1, file_bytes, fp) != file_bytes) {
        free_func(file_data, userdata);
        fclose(fp);
        return PH2CLD_false;
    }
    free_func(file_data, userdata);
    fclose(fp);
    return PH2CLD_true;
}
#endif
PH2CLD_bool PH2CLD_write_cld_memory(PH2CLD_Collision_Data data, void **file_ptr, size_t *file_len) {
    return PH2CLD_write_cld_memory_with_allocator(data, file_ptr, file_len, PH2CLD__malloc, PH2CLD_reinterpret_cast(void *, 0));
}
PH2CLD_bool PH2CLD_write_cld_memory_with_allocator(PH2CLD_Collision_Data data,
    void **file_ptr, size_t *file_len,
    void *(*alloc_func)(size_t n, void *userdata), void *userdata) {
    PH2CLD__Collision_Header header;
    /* Error on invalid input data */
    if (!data.valid) {
        return PH2CLD_false;
    }
    if (!file_ptr) {
        return PH2CLD_false;
    }
    if (!file_len) {
        return PH2CLD_false;
    }
    if (!alloc_func) {
        return PH2CLD_false;
    }
    *file_ptr = PH2CLD_reinterpret_cast(void *, 0);
    *file_len = 0;
    if (!PH2CLD__write_header_and_length(data, &header, file_len)) {
        return PH2CLD_false;
    }
    *file_ptr = alloc_func(*file_len, userdata);
    if (!*file_ptr) {
        return PH2CLD_false;
    }
    PH2CLD__write_buffers(data, header, *file_ptr);
    return PH2CLD_true;
}
PH2CLD_bool PH2CLD_write_cld_filesize(PH2CLD_Collision_Data data, size_t *file_bytes_needed) {
    PH2CLD__Collision_Header header;
    /* Error on invalid input data */
    if (!data.valid) {
        return PH2CLD_false;
    }
    if (!file_bytes_needed) {
        return PH2CLD_false;
    }
    *file_bytes_needed = 0;
    if (!PH2CLD__write_header_and_length(data, &header, file_bytes_needed)) {
        return PH2CLD_false;
    }
    return PH2CLD_true;
}

PH2CLD_bool PH2CLD_write_cld_to_memory(PH2CLD_Collision_Data data, void *file_data, size_t file_bytes) {
    PH2CLD__Collision_Header header;
    size_t file_bytes_needed;
    /* Error on invalid input data or output buffer */
    if (!data.valid) {
        return PH2CLD_false;
    }
    if (!file_data) {
        return PH2CLD_false;
    }
    if (!PH2CLD__write_header_and_length(data, &header, &file_bytes_needed)) {
        return PH2CLD_false;
    }
    if (file_bytes_needed > file_bytes) {
        return PH2CLD_false;
    }
    /* Error on overlapping memory ranges */
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_reinterpret_cast(const char *, data.group_0_faces) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_reinterpret_cast(const char *, data.group_0_faces + data.group_0_faces_count)) {
        return PH2CLD_false;
    }
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_reinterpret_cast(const char *, data.group_1_faces) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_reinterpret_cast(const char *, data.group_1_faces + data.group_1_faces_count)) {
        return PH2CLD_false;
    }
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_reinterpret_cast(const char *, data.group_2_faces) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_reinterpret_cast(const char *, data.group_2_faces + data.group_2_faces_count)) {
        return PH2CLD_false;
    }
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_reinterpret_cast(const char *, data.group_3_faces) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_reinterpret_cast(const char *, data.group_3_faces + data.group_3_faces_count)) {
        return PH2CLD_false;
    }
    if (PH2CLD_cast(const char *, file_data) + file_bytes > PH2CLD_reinterpret_cast(const char *, data.group_4_cylinders) &&
        PH2CLD_cast(const char *, file_data) < PH2CLD_reinterpret_cast(const char *, data.group_4_cylinders + data.group_4_cylinders_count)) {
        return PH2CLD_false;
    }
    PH2CLD__write_buffers(data, header, file_data);
    return PH2CLD_true;
}

void PH2CLD_free_collision_data(PH2CLD_Collision_Data data) {
    PH2CLD_free_collision_data_with_allocator(data, PH2CLD__free, PH2CLD_reinterpret_cast(void *, 0));
}
void PH2CLD_free_collision_data_with_allocator(PH2CLD_Collision_Data data, void (*free_func)(void *p, void *userdata), void *userdata) {
    if (!data.group_0_faces) {
        return;
    }
    if (!free_func) {
        return;
    }
    free_func(data.group_0_faces, userdata);
}

        
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef __cplusplus
}
#endif
    
#endif /* PH2CLD_IMPLEMENTED */
#endif /* PH2CLD_IMPLEMENTATION */

/*
Zero-Clause BSD (0BSD)

Copyright (c) 2021, Phillip Trudeau-Tavara
All rights reserved.

Permission to use, copy, modify, and/or distribute this software
for any purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
