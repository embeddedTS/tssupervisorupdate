#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <error.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

#include "micro.h"
#include "crc8.h"
#include "update-shared.h"
#include "update-v0.h"

struct micro_update_footer_v0 {
	uint32_t bin_size;
	uint8_t revision;
	uint8_t flags;
	uint8_t misc;
	uint8_t footer_version;
	uint8_t magic[11];
} __attribute__((packed));

#define FTR_V0_SZ 19
int micro_update_parse_footer_v0(int binfd, struct micro_update_footer_v0 *ftr)
{
	uint8_t data[FTR_V0_SZ];
	off_t full_size;
	int ret;

	full_size = lseek(binfd, 0, SEEK_END);
	lseek(binfd, (full_size - FTR_V0_SZ), SEEK_SET);

	ret = read(binfd, &data, FTR_V0_SZ);
	if (ret != FTR_V0_SZ)
		error(1, 0, "footer read failed!");

	memcpy(&ftr->bin_size, &data[0], 4);
	ftr->revision = data[4];
	ftr->flags = data[5];
	ftr->misc = data[6];
	ftr->footer_version = data[7];
	memcpy(&ftr->magic, &data[8], 11);

	if (strncmp("TS_UC_RA4M2", (char *)&ftr->magic, 11) != 0)
		error(1, 1, "Invalid update file");

	if (ftr->bin_size == 0 || ftr->bin_size > 128 * 1024)
		error(1, 1, "Bin size is incorrect");

	return 0;
}

/* Pack the struct to be sure it is only as large as we need */
struct open_header {
	uint32_t magic_key;
	uint32_t loc;
	uint32_t len;
	uint8_t crc;
} __attribute__((packed));

int do_v0_micro_get_rev(board_t *board, int i2cfd, int *revision)
{
	uint8_t buf[32];
	int r;

	/*
	 * First get the uC rev, attempting to send the MAC address data
	 * to an older uC rev will cause an erroneous sleep.
	 */
	r = v0_stream_read(i2cfd, board->i2c_chip, buf, 32);
	if (r) {
		printf("i2c read failed!\n");
		exit(1);
	}
	*revision = (buf[30] << 8) | buf[31];

	return 0;
}

int do_v0_micro_print_info(board_t *board, int i2cfd)
{
	int revision;
	int ret;

	ret = do_v0_micro_get_rev(board, i2cfd, &revision);
	if (ret != 0) {
		return ret;
	}

	printf("revision=%d\n", revision);
	return 0;
}

int do_v0_micro_get_file_rev(__attribute__((unused)) board_t *board, int *revision, char *update_path)
{
	struct micro_update_footer_v0 ftr;
	int binfd;
	int ret;

	binfd = open(update_path, O_RDONLY | O_RSYNC);
	if (binfd < 0)
		error(1, errno, "Error opening update file");

	ret = micro_update_parse_footer_v0(binfd, &ftr);
	if (ret != 0)
		return ret;

	ret = close(binfd);
	if (ret == -1)
		return ret;

	*revision = ftr.revision;

	return 0;
}

/*
 * The v0 is very similar to the v1 update mechanism, but as the
 * supervisor that supports in field updates was deployed around an existing
 * design, we could not change the register interface to be compatible. This
 * method works around the existing 7970 i2c register set
 */
int do_v0_micro_update(board_t *board, int i2cfd, char *update_path)
{
	struct micro_update_footer_v0 ftr;
	struct open_header hdr;
	int revision;
	uint8_t buf[129];
	int binfd;
	int ret;
	int i;

	ret = do_v0_micro_get_rev(board, i2cfd, &revision);
	if (ret != 0)
		return ret;

	if (revision < 7) {
		fprintf(stderr, "The microcontroller must be rev 7 or later to support updates.\n");
		return 0;
	}

	binfd = open(update_path, O_RDONLY | O_RSYNC);
	if (binfd < 0)
		error(1, errno, "Error opening update file");

	micro_update_parse_footer_v0(binfd, &ftr);

	if (ftr.bin_size == 0 || ftr.bin_size > 128 * 1024)
		error(1, 1, "Bin size is incorrect");

	/* Check file is 128-byte aligned */
	if (ftr.bin_size & 0x7F)
		error(1, 0, "Binary file must be 128-byte aligned!");

	fflush(stdout);

	/*
	 * Let the message print out.  Some of the flash operations will
	 * cause the micro to drop some chars if they output while we touch 
	 * flash 
	 */
	usleep(1000 * 10);

	lseek(binfd, 0, SEEK_SET);

	hdr.magic_key = 0xf092c858;
	hdr.loc = 0x28000;
	hdr.len = ftr.bin_size;
	hdr.crc = crc8((uint8_t *)&hdr, (sizeof(struct open_header) - 1));

	/* Write magic key and length/location information */
	if (v0_stream_write(i2cfd, board->i2c_chip, (uint8_t *)&hdr, 13) < 0)
		error(1, errno, "Failed to write header to I2C");

	/*
	 * Wait a bit, the flash needs to open, erase, and blank check.
	 * Could also loop on I2C read for STATUS_READY to be set
	 */
	usleep(1000000);

	v0_stream_read(i2cfd, board->i2c_chip, buf, 1);
	if (buf[0] != STATUS_READY)
		error(1, 0, "Device failed to report as opened, aborting!");

	/* Write BIN to MCU via I2C */
	for (i = ftr.bin_size; i; i -= 128) {
		printf("\r%d/%d", ftr.bin_size - i, ftr.bin_size);
		fflush(stdout);
		ret = read(binfd, buf, 128);
		if (ret < 0) {
			error(1, errno, "Error reading from BIN @ %d", ftr.bin_size - i);
		} else if (ret < 128) {
			error(1, 0, "Short read from BIN! Aborting!");
		} else {
			buf[128] = crc8(buf, 128);
			ret = v0_stream_write(i2cfd, board->i2c_chip, buf, 129);
			if (ret)
				error(1, errno, "Failed to write BIN to I2C @ %d (did uC I2C timeout?)",
				      ftr.bin_size - i);

			/*
			 * There is some unknown amount of time for a write to complete, its based
			 * on the current uC clocks and all of that, but 10 microseconds should be
			 * enough in most cases
			 */
			usleep(10);
			read(i2cfd, buf, 1);
			while (buf[0] == STATUS_WAIT)
				read(i2cfd, buf, 1);

			if ((buf[0] != STATUS_IN_PROC) && (buf[0] != STATUS_DONE)) {
				flash_print_error(buf[0]);
				return 1;
			}
		}
	}
	printf("\n");

	v0_stream_read(i2cfd, board->i2c_chip, buf, 1);
	buf[1] = STATUS_RESET;
	if (buf[0] == STATUS_DONE)
		printf("Update successful, rebooting uC\n");
	else
		printf("Update incomplete but not errored, rebooting uC\n");

	/* Give time for the message to go to the console */
	fflush(stdout);
	sleep(1);
	/* Provoke microcontroller reset */
	v0_stream_write(i2cfd, board->i2c_chip, &buf[1], 1);
	sleep(1);
	/* If we're returning at all, something has gone wrong */
	return 1;
}
