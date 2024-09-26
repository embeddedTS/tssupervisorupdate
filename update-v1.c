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

#define SUPER_MODEL 0
#define SUPER_REV_INFO 1
#define SUPER_ADC_CHAN_ADV 2
#define SUPER_FEATURES0 3
#define SUPER_CMDS 8
#define SUPER_GEN_FLAGS 16
#define SUPER_GEN_INPUTS 24
#define SUPER_ADC_BASE 128
#define SUPER_TEMPERATURE 159

#define SUPER_FL_MAGIC_KEY0 65024 // 0xFE00
#define SUPER_FL_MAGIC_KEY1 65025 // 0xFE01
#define SUPER_FL_SZ0 65030 // 0xFE06
#define SUPER_FL_SZ1 65031 // 0xFE07
#define SUPER_FL_BLOCK_DATA 65033 // 0xFE09 /* 128 bytes long, or 64 16-bit registers */
#define SUPER_FL_BLOCK_CRC 65097 // 0xFE49
#define SUPER_FL_FLASH_CMD 65098 // 0xFE4A
#define SUPER_FL_FLASH_STS 65099 // 0xFE4B
#define SUPER_FL_BLOCK_DATA_LEN 64

enum super_flash_status {
	SUPER_UPDATE_ON_REBOOT = (1 << 8), /* Set when the APPLY_REBOOT command is issued */
	/* Bits 7:0 are STATUS_ from flashwrite */
};

enum super_flash_cmd {
	SUPER_APPLY_REBOOT = (1 << 3),
	SUPER_CLOSE_FLASH = (1 << 2),
	SUPER_OPEN_FLASH = (1 << 1),
	SUPER_WRITE_BLOCK = (1 << 0),
};

/* Some return values of tend() */
enum i2c_cmds_t {
	I2C_NOCMD = (0 << 0),
	I2C_REBOOT = (1 << 0),
	I2C_HALT = (1 << 1),
};

enum gen_flags_t {
	GEN_FLAG_LED_DAT = (1 << 3),
	GEN_FLAG_OVERRIDE_LED = (1 << 2),
	GEN_FLAG_WAKE_EN = (1 << 1),
	GEN_FLAG_ALARM_TYPE = (1 << 0),
};

enum gen_inputs_t {
	GEN_INPUTS_USB_VBUS = (1 << 1),
	GEN_INPUTS_EN_DB9_CONSOLE = (1 << 0),
};

enum super_features_t {
	SUPER_FEAT_SN = (1 << 2),
	SUPER_FEAT_FWUPD = (1 << 1),
	SUPER_FEAT_RSTC = (1 << 0),
};

struct micro_update_footer_v1 {
	uint32_t bin_size;
	uint16_t revision;
	uint8_t flags;
	uint8_t misc;
	uint16_t model;
	uint8_t footer_version;
	uint8_t magic[11];
} __attribute__((packed));

#define FTR_V1_SZ (22U)
int micro_update_parse_footer_v1(int binfd, struct micro_update_footer_v1 *ftr)
{
	uint8_t data[FTR_V1_SZ];
	off_t full_size;
	int ret;

	full_size = lseek(binfd, 0, SEEK_END);
	lseek(binfd, (full_size - FTR_V1_SZ), SEEK_SET);

	ret = read(binfd, &data, FTR_V1_SZ);
	if (ret < 0) {
		perror("Unable to read footer");
		goto err_out;
	}
	if (ret != FTR_V1_SZ) {
		fprintf(stderr, "Did not read correct footer size\n");
		goto err_out;
	}

	memcpy(&ftr->bin_size, &data[0], 4);
	memcpy(&ftr->model, &data[4], 2);
	memcpy(&ftr->revision, &data[6], 2);
	ftr->flags = data[8];
	ftr->misc = data[9];
	ftr->footer_version = data[10];
	memcpy(&ftr->magic, &data[11], 11);

	if (strncmp("TS_UC_RA4M2", (char *)&ftr->magic, 11) != 0) {
		fprintf(stderr, "Invalid update file\n");
		goto err_out;
	}

	/* Ensure that the bin_size specified by the footer both matches the
	 * actual size of the binary as well as it not being more than 128 kbyte
	 * (which is the max size an update can be on this platform).
	 */
	if (ftr->bin_size != (full_size - FTR_V1_SZ) || ftr->bin_size > 128 * 1024) {
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

int do_v1_micro_get_rev(board_t *board, int i2cfd, int *revision)
{
	if (speek16(i2cfd, board->i2c_chip, SUPER_REV_INFO, (uint16_t *)revision) < 0) {
		fprintf(stderr, "Unable to get revision\n");
		return -1;
	}
	*revision &= 0x7fff;

	return 0;
}

int do_v1_micro_print_info(board_t *board, int i2cfd)
{
	uint16_t revision, modelnum;

	if (speek16(i2cfd, board->i2c_chip, SUPER_MODEL, &modelnum) < 0)
		return -1;
	if (speek16(i2cfd, board->i2c_chip, SUPER_REV_INFO, &revision) < 0)
		return -1;

	printf("modelnum=0x%04X\n", modelnum);
	printf("revision=%d\n", revision & 0x7fff);
	printf("dirty=%d\n", !!(revision & (1 << 15)));
	return 0;
}

int do_v1_micro_get_file_rev(board_t *board, int *revision, char *update_path)
{
	struct micro_update_footer_v1 ftr;
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

	if (micro_update_parse_footer_v1(binfd, &ftr) < 0)
		ret = -1;

	if (close(binfd) < 0) {
		perror("Unable to close update file");
		ret = -1;
	}

	*revision = ftr.revision;

out:
	return ret;
}

int do_v1_micro_update(board_t *board, int i2cfd, char *update_path)
{
	uint16_t status;
	uint16_t crc;
	uint32_t bin_size;
	struct micro_update_footer_v1 ftr;
	uint16_t buf[64];
	int binfd;
	int ret;
	int i;
	int retry_count;

	binfd = open(update_path, O_RDONLY | O_RSYNC);
	if (binfd < 0) {
		perror("Error opening update file");
		return -1;
	}

	if (speek16(i2cfd, board->i2c_chip, SUPER_FEATURES0, &status) < 0)
		goto err_out;

	if (!(status & SUPER_FEAT_FWUPD)) {
		fprintf(stderr, "Firmware does not support updates. (0x%X)\n", status);
		goto err_out;
	}

	if (micro_update_parse_footer_v1(binfd, &ftr) < 0)
		goto err_out;

	if (ftr.model != board->modelnum) {
		fprintf(stderr, "This update is for a %04X, not a %04X.\n", ftr.model, board->modelnum);
		goto err_out;
	}

	/* gcc warns this pointer has alignment issues in packed structure. */
	bin_size = (uint32_t)ftr.bin_size;

	fflush(stdout);

	/*
	 * Let the messages print out. Some of the flash operations will
	 * cause the micro to drop some chars if they output while we touch 
	 * flash 
	 */
	usleep(1000 * 10);

	/* Write magic key and length/location information */
	if (spokestream16(i2cfd, board->i2c_chip, SUPER_FL_MAGIC_KEY0, (uint16_t *)&magic_key, 4) < 0) {
		fprintf(stderr, "Failed to write magic key");
		goto err_out;
	}

	if (spokestream16(i2cfd, board->i2c_chip, SUPER_FL_SZ0, (uint16_t *)&bin_size, 4) < 0) {
		fprintf(stderr, "Failed to write bin length");
		goto err_out;
	}

	lseek(binfd, 0, SEEK_SET);

	/* If flash is already opened from a previous action, close it to reset
	 * the flash state.
	 */
	if (speek16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_STS, &status) < 0)
		goto err_out;

	if ((status & 0xff) != STATUS_CLOSED) {
		if (spoke16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_CMD, SUPER_CLOSE_FLASH) < 0)
			goto err_out;

		if (speek16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_STS, &status) < 0)
			goto err_out;

		if ((status & 0xff) != STATUS_CLOSED) {
			fprintf(stderr, "Couldn't re-close flash!\n");
			goto err_out;
		}
	}

	/* Poll until flash is opened. This also has to check/erase flash
	 * which happens while interrupts are disabled for flash safety. Because
	 * interrupts are disabled, I2C transactions get stalled, and can
	 * generate errors. Wait a long timeout before trying to talk to the
	 * uC again.
	 */
	if (spoke16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_CMD, SUPER_OPEN_FLASH) < 0)
		goto err_out;

	sleep(1);

	if (speek16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_STS, &status) < 0)
		goto err_out;

	if ((status & 0xff) != STATUS_READY) {
		fprintf(stderr, "Failed to open flash!\n");
		if (status != STATUS_CLOSED)
			flash_print_error(status);
		goto err_out;
	}

	/* Write BIN to MCU via I2C */
	for (i = bin_size; i; i -= 128) {
		printf("\r%d/%d", bin_size - i, bin_size);
		fflush(stdout);
		ret = read(binfd, buf, 128);
		if (ret < 0) {
			perror("Error reading from bin file");
			goto err_out;
		} else if (ret < 128) {
			fprintf(stderr, "Short read from bin, got %d, expected 128\n", ret);
			goto err_out;
		} else {
			crc = (uint16_t)crc8((uint8_t *)buf, 128);

			if (spokestream16(i2cfd, board->i2c_chip, SUPER_FL_BLOCK_DATA, buf, 128) < 0)
				goto err_out;

			if (spoke16(i2cfd, board->i2c_chip, SUPER_FL_BLOCK_CRC, crc) < 0)
				goto err_out;

			if (spoke16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_CMD, SUPER_WRITE_BLOCK) < 0)
				goto err_out;

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
				speek16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_STS, &status);
				status &= 0xff;
				if (!retry_count--)
					break;
			} while (status == STATUS_WAIT);

			if (status != STATUS_IN_PROC && status != STATUS_DONE) {
				flash_print_error(status);
				goto err_out;
			}
		}
	}

	/* Do a DONE check to make sure both sides moved as much data as they
	 * both expected. If uC is still IN_PROC then the full amount of data
	 * was not received.
	 */
	if (status != STATUS_DONE) {
		printf("\r                            ");
		fprintf(stderr, "\rError: Updated failed\n");
		goto err_out;
	} else {
		printf("\r                            ");
		printf("\rWrote %d byte supervisor update\n", bin_size);
	}

	if (spoke16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_CMD, SUPER_CLOSE_FLASH) < 0)
		goto err_out;

	/* Poll until flash is closed */
	retry_count = 100;
	do {
		usleep(10);
		speek16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_STS, &status);
		status &= 0xff;
		if (!retry_count--)
			goto err_out;
	} while (status != STATUS_CLOSED);

	/*
	 * If there is a valid image when the microcontroller starts up, it will
	 * switch to it on the next startup. However, the microcontroller does not
	 * normally reboot from the main cpu running a reboot. To apply an update
	 * in the field, we can tell it for the next linux reboot to cause a full
	 * reset for the microcontroller as well.
	 */
	spoke16(i2cfd, board->i2c_chip, SUPER_FL_FLASH_CMD, SUPER_APPLY_REBOOT);
	printf("Update succeeded. On the next reboot the microcontroller update "
	       "will be live. This will force the USB console device to "
	       "disconnect momentarily while the update applies.\n");

	close(binfd);
	return 0;

err_out:
	close(binfd);
	return -1;
}
