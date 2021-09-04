/* SPDX-FileCopyrightText: © 2021 Phillip Trudeau-Tavara <pmttavara@protonmail.com>
   SPDX-License-Identifier: 0BSD */

#include <errno.h>
#include <stdio.h>
#include <io.h>
#include <assert.h>

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

            fopen_result = fopen_s(&f, b, "rb");
            if (!fopen_result) {
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
