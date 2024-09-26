#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

#include "micro.h"
#include "crc8.h"
#include "update-shared.h"

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
	if (ret < 0) {
		perror("Reading footer failed");
		goto err_out;
	}
	if (ret != FTR_V0_SZ) {
		fprintf(stderr, "Did not read correct footer size\n");
		goto err_out;
	}

	/* Note:
	 * This is an intentional choice as it was noted that different compilers
	 * appear to do different things when attempting to memcpy the entire
	 * footer directly overtop the struct, even though it is packed.
	 */
	memcpy(&ftr->bin_size, &data[0], 4);
	ftr->revision = data[4];
	ftr->flags = data[5];
	ftr->misc = data[6];
	ftr->footer_version = data[7];
	memcpy(&ftr->magic, &data[8], 11);

	if (strncmp("TS_UC_RA4M2", (char *)&ftr->magic, 11) != 0) {
		fprintf(stderr, "Invalid update file\n");
		goto err_out;
	}

	/* Ensure that the bin_size specified by the footer both matches the
	 * actual size of the binary as well as it not being more than 128 kbyte
	 * (which is the max size an update can be on this platform).
	 */
	if (ftr->bin_size != (full_size - FTR_V0_SZ) || ftr->bin_size > 128 * 1024) {
		fprintf(stderr, "Bin size is incorrect\n");
		goto err_out;
	}

	/* Check file is 128-byte aligned */
	if (ftr->bin_size & 0x7F) {
		fprintf(stderr, "Update binary is not 128-byte aligned.\n");
		goto err_out;
	}

	return 0;

err_out:
	return -1;
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

	if (v0_stream_read(i2cfd, board->i2c_chip, buf, 32) < 0) {
		fprintf(stderr, "Unable to get revision\n");
		return -1;
	}
	*revision = (buf[30] << 8) | buf[31];

	return 0;
}

int do_v0_micro_print_info(board_t *board, int i2cfd)
{
	int revision;

	if (do_v0_micro_get_rev(board, i2cfd, &revision) < 0)
		return -1;

	printf("revision=%d\n", revision);
	return 0;
}

int do_v0_micro_get_file_rev(board_t *board, int *revision, char *update_path)
{
	struct micro_update_footer_v0 ftr;
	int binfd;
	int ret = 0;

	/* Unused */
	(void)board;

	binfd = open(update_path, O_RDONLY | O_RSYNC);
	if (binfd < 0) {
		perror("Unable to open update file");
		ret = -1;
		goto out;
	}

	if (micro_update_parse_footer_v0(binfd, &ftr) < 0)
		ret = -1;

	if (close(binfd) < 0) {
		perror("Unable to close update file");
		ret = -1;
	}

	*revision = ftr.revision;

out:
	return ret;
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
	uint8_t buf[129];
	int binfd;
	int ret;
	int i;
	int retry_count;

	binfd = open(update_path, O_RDONLY | O_RSYNC);
	if (binfd < 0) {
		perror("Error opening update file");
		return -1;
	}

	if (micro_update_parse_footer_v0(binfd, &ftr) < 0)
		goto err_out;

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
	if (v0_stream_write(i2cfd, board->i2c_chip, (uint8_t *)&hdr, 13) < 0) {
		fprintf(stderr, "Failed to write header to I2C");
		goto err_out;
	}

	/*
	 * Wait a bit, the flash needs to open, erase, and blank check.
	 * Could also loop on I2C read for STATUS_READY to be set
	 */
	usleep(1000000);

	if (v0_stream_read(i2cfd, board->i2c_chip, buf, 1) < 0) {
		fprintf(stderr, "Failed to read device state, aborting!");
		goto err_out;
	}

	if (buf[0] != STATUS_READY) {
		fprintf(stderr, "Device failed to report as opened, aborting!");
		goto err_out;
	}

	/* Write BIN to MCU via I2C */
	for (i = ftr.bin_size; i; i -= 128) {
		printf("\r%d/%d", ftr.bin_size - i, ftr.bin_size);
		fflush(stdout);
		ret = read(binfd, buf, 128);
		if (ret < 0) {
			fprintf(stderr, "Error reading from bin file\n");
			goto err_out;
		} else if (ret < 128) {
			fprintf(stderr, "Short read from bin, got %d, expected 128", ret);
			goto err_out;
		} else {
			buf[128] = crc8(buf, 128);
			if (v0_stream_write(i2cfd, board->i2c_chip, buf, 129) < 0) {
				fprintf(stderr, "Failed to write block\n");
				goto err_out;
			}

			/* There is some unknown amount of time for a write to
			 * complete, its based on the current uC and flash controller
			 * clocks, but 2 milliseconds should be enough in most cases.
			 * Most of the time is taken up by the decryption of the
			 * data block. However, the actual flash write is a non-zero
			 * time too. During which interrupts are disabled for flash
			 * safety. The timeout helps ensure the process completes
			 * before we start polling for state.
			 */
			usleep(2000);
			retry_count = 100;
			do {
				usleep(10);
				v0_stream_read(i2cfd, board->i2c_chip, buf, 1);
				if (!retry_count--)
					break;
			} while (buf[0] == STATUS_WAIT);

			if ((buf[0] != STATUS_IN_PROC) && (buf[0] != STATUS_DONE)) {
				flash_print_error(buf[0]);
				goto err_out;
			}
		}
	}
	printf("\n");

	if (buf[0] == STATUS_DONE)
		printf("Update successful, rebooting uC\n");
	else
		printf("Update incomplete but not errored, rebooting uC\n");

	/* Give time for the message to go to the console */
	fflush(stdout);
	sleep(1);
	/* Provoke microcontroller reset */
	buf[1] = STATUS_RESET;
	v0_stream_write(i2cfd, board->i2c_chip, &buf[1], 1);
	sleep(1);
	/* If we're returning at all, something has gone wrong */
err_out:
	close(binfd);
	return -1;
}
