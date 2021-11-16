/* SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
   SPDX-License-Identifier: 0BSD */

#include <errno.h>
#include <stdio.h>
#include <io.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ph2_cld.h"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif

#ifdef _WIN32
#pragma warning(disable : 4255)
#pragma warning(disable : 4668)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

static double get_time(void) {
    LARGE_INTEGER counter;
    static double invfreq;
    if (invfreq == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        invfreq = 1.0 / (double)freq.QuadPart;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * invfreq;
}
#else
#include <time.h>
static double get_time(void) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}
#endif

int main(void) {
    struct _finddata_t find_data;
    intptr_t directory = _findfirst("cld/*.cld", &find_data);
    printf("CLD file start\n");
    if (directory >= 0) {
        while (1) {
            char b[260 + sizeof("cld/")];
            FILE *f;
            errno_t fopen_result;
            snprintf(b, sizeof(b), "cld/%s", find_data.name);

#if 1
            /* I was intending to have good error checking here, but there's a lot of agile stuff lying around.
               Whenever you touch this, consider changing some logic to be more error-checky. */
            fopen_result = fopen_s(&f, b, "rb");
            if (!fopen_result) {
                char *file_data = malloc(1024*1024);
                char *roundtrip_data = malloc(1024*1024);
                char *collision_memory;
                size_t file_length = fread(file_data, 1, 1024*1024, f);
                size_t collision_memory_length = 0;
                if (!PH2CLD_get_collision_memory_length_from_file_memory(file_data, file_length, &collision_memory_length)) {
                    assert(0);
                }
                collision_memory = malloc(collision_memory_length);
                if (file_data && roundtrip_data && collision_memory) {
                    PH2CLD_Collision_Data data = {0};
                    PH2CLD_bool write_result;
                    static double read_time;
                    static double write_time;
                    static size_t total_bytes;
                    int i;
                    const int N = 1;
                    total_bytes += file_length;
                    read_time += -get_time() / (double)N;
                    for (i = 0; i < N; i++) {
                        data = PH2CLD_get_collision_data_from_file_memory_and_collision_memory(file_data, file_length, collision_memory, collision_memory_length);
                        assert(data.valid);
                    }
                    read_time += get_time() / (double)N;
                    printf("Reading took %f seconds (%f MB/s)\n", read_time, (double)total_bytes / read_time / 1024 / 1024);

                    roundtrip_data = malloc(file_length);
                    write_time += -get_time() / (double)N;
                    for (i = 0; i < N; i++) {
                        write_result = PH2CLD_write_cld_to_memory(data, roundtrip_data, file_length);
                        assert(write_result);
                    }
                    write_time += get_time() / (double)N;
                    printf("Writing took %f seconds (%f MB/s)\n", write_time, (double)total_bytes / write_time / 1024 / 1024);


                    assert(memcmp(file_data, roundtrip_data, file_length) == 0);

                    free(file_data);
                    free(roundtrip_data);
                    free(collision_memory);

                }
                fclose(f);
            } else {
                printf("File open failed! :(\n");
            }
#else
            printf("File '%s': ", b);
            {
                PH2CLD_Collision_Data data = PH2CLD_get_collision_data_from_file(b);
                if (data.valid) {
                    size_t i = 0;
                    printf("\n Group 0: {\n");
                    for (; i < data.group_0_faces_count; i++) {
                        PH2CLD_Face face = data.group_0_faces[i];
                        int bit = 15;
                        printf("  Face %zu (", i);
                        for (; bit >= 0; bit--) if (face.subgroups & (1 << bit)) printf("1"); else printf("o");
                        printf("): (%f, %f, %f), ", (double)face.vertices[0][0], (double)face.vertices[0][1], (double)face.vertices[0][2]);
                        printf("(%f, %f, %f), ", (double)face.vertices[1][0], (double)face.vertices[1][1], (double)face.vertices[1][2]);
                        printf("(%f, %f, %f)", (double)face.vertices[2][0], (double)face.vertices[2][1], (double)face.vertices[2][2]);
                        if (face.quad) {
                            printf(", (%f, %f, %f)", (double)face.vertices[3][0], (double)face.vertices[3][1], (double)face.vertices[3][2]);
                        }
                        printf("\n");
                    }
                    printf("}\n Group 4: {\n");
                    for (i = 0; i < data.group_4_cylinders_count; i++) {
                        PH2CLD_Cylinder cyl = data.group_4_cylinders[i];
                        int bit = 15;
                        printf("  Cylinder %zu (", i);
                        for (; bit >= 0; bit--) if (cyl.subgroups & (1 << bit)) printf("1"); else printf("o");
                        printf("): (%f, %f, %f), ", (double)cyl.position[0], (double)cyl.position[1], (double)cyl.position[2]);
                        printf("Height %f, Radius %f\n", (double)cyl.height, (double)cyl.radius);
                    }
                    printf("}\n");
                } else {
                    printf("Couldn't get CLD data\n");
                }
                {
                    PH2CLD_bool write_result = PH2CLD_write_cld(data, "my_PH2CLD_output.cld");
                    assert(write_result);
                }
                PH2CLD_free_collision_data(data);
            }
#endif
            if (_findnext(directory, &find_data) < 0) {
                if (errno == ENOENT) break;
                else assert(0);
            }
        }
        _findclose(directory);
        fflush(stdout);
    }
}
