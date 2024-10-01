#pragma once

#include "update-shared.h"

int do_v1_micro_update(board_t *board, int i2cfd, char *update_path);
int do_v1_micro_get_rev(board_t *board, int i2cfd, int *revision);
int do_v1_micro_get_file_rev(board_t *board, int *revision, char *update_path);
int do_v1_micro_print_info(board_t *board, int i2cfd);
