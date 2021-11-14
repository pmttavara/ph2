/* SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
   SPDX-License-Identifier: 0BSD */

#include <errno.h>
#include <stdio.h>
#include <io.h>
#include <assert.h>
#include <stdlib.h>

#include "ph2_cld.h"

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

            /* I was intending to have good error checking here, but there's a lot of agile stuff lying around.
               Whenever you touch this, consider changing some logic to be more error-checky. */
            fopen_result = fopen_s(&f, b, "rb");
            if (!fopen_result) {
                size_t file_length = 1024*1024;
                char *file_data = malloc(file_length);
                size_t collision_memory_length = 1024*1024;
                char *collision_memory = malloc(collision_memory_length);
                PH2CLD_Collision_Data data;
                file_length = fread(file_data, 1, file_length, f);
                data = PH2CLD_get_collision_data_from_memory(file_data, file_length, collision_memory, collision_memory_length);
                assert(data.valid);
                fclose(f);
                if (_findnext(directory, &find_data) < 0) {
                    if (errno == ENOENT) break;
                    else assert(0);
                }
            } else {
                printf("File open failed! :(\n");
            }
        }
        _findclose(directory);
    }
}
