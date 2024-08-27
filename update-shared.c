#include <stdio.h>
#include <stdint.h>

#include "update-shared.h"

void flash_print_error(uint8_t status)
{
	switch (status) {
	case STATUS_OPEN_ERR:
		fprintf(stderr, "Flash failed to open!\n");
		break;
	case STATUS_NOT_BLANK:
		fprintf(stderr, "Flash not blank\n");
		break;
	case STATUS_ERASE_ERR:
		fprintf(stderr, "Flash failed to erase!\n");
		break;
	case STATUS_WRITE_ERR:
		fprintf(stderr, "Flash failed to write!\n");
		break;
	case STATUS_CRC_ERR:
		fprintf(stderr, "Flash received bad data CRC!\n");
		break;
	default:
		fprintf(stderr, "Unknown flash failure\n");
		break;
	}
}
