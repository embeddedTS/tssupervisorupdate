#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <getopt.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define I2C_ADDR 0x10

#define SUPER_MODEL         0
#define SUPER_REV_INFO      1
#define SUPER_ADC_CHAN_ADV  2
#define SUPER_FEATURES0     3
#define SUPER_CMDS          8
#define SUPER_GEN_FLAGS     16
#define SUPER_GEN_INPUTS    24
#define SUPER_ADC_BASE      128
#define SUPER_TEMPERATURE   159

#define SUPER_FL_MAGIC_KEY0  65024 // 0xFE00
#define SUPER_FL_MAGIC_KEY1  65025 // 0xFE01
#define SUPER_FL_SZ0         65030 // 0xFE06
#define SUPER_FL_SZ1         65031 // 0xFE07
#define SUPER_FL_BLOCK_DATA  65033 // 0xFE09 /* 128 bytes long, or 64 16-bit registers */
#define SUPER_FL_BLOCK_CRC   65097 // 0xFE49
#define SUPER_FL_FLASH_CMD   65098 // 0xFE4A
#define SUPER_FL_FLASH_STS   65099 // 0xFE4B
#define SUPER_FL_BLOCK_DATA_LEN 64

/* Getopt flags */
int dry_run_flag;
int force_flag;
int info_flag;
char *updatefile;

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

static int speekstream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size);
static int spokestream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size);
static int super_init(int i2cbus, int i2caddr);
static void spoke16(int i2cfd, int i2caddr, uint16_t addr, uint16_t data);
static uint16_t speek16(int i2cfd, int i2caddr, uint16_t addr);

static char *get_dt_model(void)
{
	FILE *proc;
	char mdl[256];
	char *ptr;
	int sz;

	proc = fopen("/proc/device-tree/model", "r");
	if (!proc) {
		perror("model");
		return 0;
	}
	sz = fread(mdl, 256, 1, proc);
	ptr = strstr(mdl, "TS-");
	return strndup(ptr, sz - (mdl - ptr));
}

/*
 * The updates themselves are encrypted/signed, but the below key is just used to
 * prevent unintentional writes to i2c causing writes to the flash.
 */
const uint32_t magic_key = 0xf092c858;

/* Read-back status values */
/* Default value of status, closed */
#define STATUS_CLOSED       0x00
/* Once the flashwrite process is set up, but no data written */
#define STATUS_READY        0xAA
/* Flashwrite process has seen full length of data written and is considered done */
#define STATUS_DONE         0x01
/* Flashwrite is in process, meaning SOME data has been written, but not the full length */
#define STATUS_IN_PROC      0x02
/* A CRC error occurred at ANY point during data write. Note that this status
 * is not set if CRC fails for open process, the system simply does not open
 */
#define STATUS_CRC_ERR      0x03
/* An error occurred while trying to erase the actual flash */
#define STATUS_ERASE_ERR    0x04
/* An error occurred at ANY point during data write. */
#define STATUS_WRITE_ERR    0x05
/* Erase was successful, but, the area to be written was not blank */
#define STATUS_NOT_BLANK    0x06
/* A BSP error opening and closing flash. Most errors are buggy code, configurations, or unrecoverable */
#define STATUS_OPEN_ERR	    0x07
/* Wait state while processing a write */
#define STATUS_WAIT         0x08
/* Request the uC reboot at any time after its open status */
#define STATUS_RESET        0x55

struct micro_update_footer_v1 {
        uint32_t bin_size;
        uint16_t revision;
        uint8_t flags;
        uint8_t misc;
	uint16_t model;
        uint8_t footer_version;
        uint8_t magic[11];
} __attribute__((packed));

struct micro_update_footer_v0 {
	uint32_t bin_size;
	uint8_t revision;
	uint8_t flags;
	uint8_t misc;
	uint8_t footer_version;
	uint8_t magic[11];
} __attribute__((packed));

/* Pack the struct to be sure it is only as large as we need */
struct open_header {
	uint32_t magic_key;
	uint32_t loc;
	uint32_t len;
	uint8_t crc;
} __attribute__((packed));

unsigned char const crc8x_table[] = {
	0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15,
	0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
	0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65,
	0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
	0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5,
	0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
	0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85,
	0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
	0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2,
	0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
	0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2,
	0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
	0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32,
	0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
	0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42,
	0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
	0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C,
	0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
	0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC,
	0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
	0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C,
	0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
	0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C,
	0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
	0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B,
	0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
	0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B,
	0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
	0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB,
	0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
	0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB,
	0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3
};

#define FTR_V1_SZ (22U)
static int micro_update_parse_footer_v1(int binfd, struct micro_update_footer_v1 *ftr)
{
	uint8_t data[FTR_V1_SZ];
	off_t full_size;
	int ret;

	full_size = lseek(binfd, 0, SEEK_END);
	lseek(binfd, (full_size - FTR_V1_SZ), SEEK_SET);

	ret = read(binfd, &data, FTR_V1_SZ);
	if (ret != FTR_V1_SZ)
		error(1, 0, "footer read failed!");

	memcpy(&ftr->bin_size, &data[0], 4);
	memcpy(&ftr->model, &data[4], 2);
	memcpy(&ftr->revision, &data[6], 2);
	ftr->flags = data[8];
	ftr->misc = data[9];
	ftr->footer_version = data[10];
	memcpy(&ftr->magic, &data[11], 11);

	if (strncmp("TS_UC_RA4M2", (char *)&ftr->magic, 11) != 0)
		error(1, 1, "Invalid update file");

	if (ftr->bin_size == 0 || ftr->bin_size > 128*1024)
		error(1, 1, "Bin size is incorrect");

	return 0;
}

#define FTR_V0_SZ 19
static int micro_update_parse_footer_v0(int binfd, struct micro_update_footer_v0 *ftr)
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

	if (ftr->bin_size == 0 || ftr->bin_size > 128*1024)
		error(1, 1, "Bin size is incorrect");

	return 0;
}

static int super_init(int i2cbus, int i2caddr)
{
	static int fd = -1;
	char i2c_bus_path[20];

	if (fd != -1)
		return fd;

	snprintf(i2c_bus_path, sizeof(i2c_bus_path), "/dev/i2c-%d", i2cbus);
	fd = open(i2c_bus_path, O_RDWR);
	if (fd == -1) {
		perror("Couldn't open i2c device");
		exit(1);
	}

	/*
	 * We use force because there is typically a driver attached. This is
	 * safe because we are using only i2c_msgs and not read()/write() calls
	 */
	if (ioctl(fd, I2C_SLAVE_FORCE, i2caddr) < 0) {
		perror("Supervisor did not ACK\n");
		exit(1);
	}

	return fd;
}

static void spoke16(int i2cfd, int i2caddr, uint16_t addr, uint16_t data)
{
	int ret;

	ret = spokestream16(i2cfd, i2caddr, addr, &data, 2);
	if (ret) {
		perror("Failed to write to supervisory micro");
		exit(1);
	}
}

static uint16_t speek16(int i2cfd, int i2caddr, uint16_t addr)
{
	uint16_t data = 0;
	int ret = speekstream16(i2cfd, i2caddr, addr, &data, 2);

	if (ret) {
		perror("Failed to read from supervisory micro");
		exit(1);
	}
	return data;
}

static int speekstream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size)
{
	struct i2c_rdwr_ioctl_data packets;
	struct i2c_msg msgs[2];

	msgs[0].addr = i2caddr;
	msgs[0].flags = 0;
	msgs[0].len	= 2;
	msgs[0].buf	= (uint8_t *)&addr;

	msgs[1].addr = i2caddr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len	= size;
	msgs[1].buf	= (uint8_t *)data;

	packets.msgs  = msgs;
	packets.nmsgs = 2;

	if (ioctl(i2cfd, I2C_RDWR, &packets) < 0) {
		perror("Unable to send data");
		return 1;
	}
	return 0;
}

static int spokestream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size)
{
	struct i2c_rdwr_ioctl_data packets;
	struct i2c_msg msg;
	uint8_t outdata[4096];

	/*
	 * Linux only supports 4k transactions at a time, and we need
	 * two bytes for the address
	 */
	assert(size <= 4094);

	memcpy(outdata, &addr, 2);
	memcpy(&outdata[2], data, size);

	msg.addr = i2caddr;
	msg.flags = 0;
	msg.len	= 2 + size;
	msg.buf	= outdata;

	packets.msgs  = &msg;
	packets.nmsgs = 1;

	if (ioctl(i2cfd, I2C_RDWR, &packets) < 0)
		return 1;
	return 0;
}

static int ts7970_stream_read(int twifd, uint8_t *data, int bytes)
{
	struct i2c_rdwr_ioctl_data packets;
	struct i2c_msg msg;
	int retry = 0;

retry:
	msg.addr = I2C_ADDR;
	msg.flags = I2C_M_RD;
	msg.len	= bytes;
	msg.buf	= data;

	packets.msgs  = &msg;
	packets.nmsgs = 1;

	if (ioctl(twifd, I2C_RDWR, &packets) < 0) {
		perror("Unable to read data");
		retry++;
		if (retry < 10)
			goto retry;
	}
	return 0;
}

static int ts7970_stream_write(int twifd, uint8_t *data, int bytes)
{
	struct i2c_rdwr_ioctl_data packets;
	struct i2c_msg msg;

	msg.addr = I2C_ADDR;
	msg.flags = 0;
	msg.len	= bytes;
	msg.buf	= data;

	packets.msgs  = &msg;
	packets.nmsgs = 1;

	if (ioctl(twifd, I2C_RDWR, &packets) < 0) {
		perror("Unable to send data");
		return 1;
	}
	return 0;
}

static uint8_t crc8(uint8_t *input_str, size_t num_bytes)
{
	size_t a;
	uint8_t crc = 0;
	uint8_t *ptr = input_str;

	if (ptr != NULL)
		for (a = 0; a < num_bytes; a++)
			crc = crc8x_table[(*ptr++) ^ crc];

	return crc;
}

static int ts7970_get_revision(int twifd)
{
	uint8_t buf[32];
	int r;
	int rev;

	/*
	 * First get the uC rev, attempting to send the MAC address data
	 * to an older uC rev will cause an erroneous sleep.
	 */
	r = ts7970_stream_read(twifd, buf, 32);
	if (r) {
		printf("i2c read failed!\n");
		exit(1);
	}
	rev = (buf[30] << 8) | buf[31];

	return rev;
}

/*
 * The TS-7970 is very similar to the common update mechanism, but as the
 * supervisor that supports in field updates was deployed around an existing
 * design, we could not change the register interface to be compatible. This
 * method works around the existing 7970 i2c register set
 */
static int do_renesas_7970_update(int i2cbus, int i2caddr)
{
	struct micro_update_footer_v0 ftr;
	struct open_header hdr;
	uint8_t revision;
	uint8_t buf[129];
	int binfd;
	int i2cfd;
	int ret;
	int i;

	i2cfd = super_init(i2cbus, i2caddr);

	revision = ts7970_get_revision(i2cfd);

	if (info_flag) {
		printf("modelnum=0x%04X\n", 0x7970);
		printf("revision=%d\n", revision);
		return 0;
	}

	binfd = open(updatefile, O_RDONLY | O_RSYNC);
	if (binfd < 0)
		error(1, errno, "Error opening update file");

	micro_update_parse_footer_v0(binfd, &ftr);

	if (ftr.bin_size == 0 || ftr.bin_size > 128*1024)
		error(1, 1, "Bin size is incorrect");

	/* Check file is 128-byte aligned */
	if (ftr.bin_size & 0x7F)
		error(1, 0, "Binary file must be 128-byte aligned!");
	if (revision < 7) {
		fprintf(stderr, "The microcontroller must be rev 7 or later to support updates.\n");
		return 0;
	}

	if ((revision == ftr.revision) && !force_flag) {
		printf("Already running microcontroller revision %d(%d), not updating\n", revision, ftr.revision);
		return 0;
	}

	printf("Updating from revision %d to %d\n", revision, ftr.revision);
	fflush(stdout);

	/*
	 * Let the message print out.  Some of the flash operations will
	 * cause the micro to drop some chars if they output while we touch 
	 * flash 
	 */
	usleep(1000*10);

	if (dry_run_flag) {
		printf("Dry run specified, closing.\n");
		return 0;
	}

	lseek(binfd, 0, SEEK_SET);

	hdr.magic_key = 0xf092c858;
	hdr.loc = 0x28000;
	hdr.len = ftr.bin_size;
	hdr.crc = crc8((uint8_t *)&hdr, (sizeof(struct open_header) - 1));

	/* Write magic key and length/location information */
	if (ts7970_stream_write(i2cfd, (uint8_t *)&hdr, 13) < 0)
		error(1, errno, "Failed to write header to I2C");

	/*
	 * Wait a bit, the flash needs to open, erase, and blank check.
	 * Could also loop on I2C read for STATUS_READY to be set
	 */
	usleep(1000000);

	ts7970_stream_read(i2cfd, buf, 1);
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
			ret = ts7970_stream_write(i2cfd, buf, 129);
			if (ret)
				error(1, errno, "Failed to write BIN to I2C @ %d (did uC I2C timeout?)", ftr.bin_size - i);

			/*
			 * There is some unknown amount of time for a write to complete, its based
			 * on the current uC clocks and all of that, but 10 microseconds should be
			 * enough in most cases
			 */
			usleep(10);
			read(i2cfd, buf, 1);
			while (buf[0] == STATUS_WAIT)
				read(i2cfd, buf, 1);

			if ((buf[0] != STATUS_IN_PROC) && (buf[0] != STATUS_DONE))
				error(1, 0, "Device reported error status 0x%02X\n", buf[0]);
		}
	}
	printf("\n");

	ts7970_stream_read(i2cfd, buf, 1);
	buf[1] = STATUS_RESET;
	if (buf[0] == STATUS_DONE)
		printf("Update successful, rebooting uC\n");
	else
		printf("Update incomplete but not errored, rebooting uC\n");

	/* Give time for the message to go to the console */
	fflush(stdout);
	sleep(1);
	/* Provoke microcontroller reset */
	ts7970_stream_write(i2cfd, &buf[1], 1);
	sleep(1);
	/* If we're returning at all, something has gone wrong */
	return 1;
}

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

static int do_common_supervisor_update(int i2cbus, int i2caddr)
{
	uint16_t revision, modelnum, features0, status, crc, dirty;
	uint32_t bin_size;
	struct micro_update_footer_v1 ftr;
	uint16_t buf[64];
	int i2cfd, binfd;
	int ret;
	int i;
	int retries = 10;

	i2cfd = super_init(i2cbus, i2caddr);

	modelnum = speek16(i2cfd, i2caddr, SUPER_MODEL);
	revision = speek16(i2cfd, i2caddr, SUPER_REV_INFO);
	features0 = speek16(i2cfd, i2caddr, SUPER_FEATURES0);
	dirty = !!(revision & 0x8000);
	revision &= 0x7fff;

	if (info_flag) {
		printf("modelnum=0x%04X\n", modelnum);
		printf("revision=%d\n", revision);
		printf("dirty=%d\n", dirty);
		return 0;
	}

	if (((features0 & SUPER_FEAT_FWUPD) == 0) && !force_flag) {
		fprintf(stderr, "This supervisor does not support firmware updates. (0x%X)\n", features0);
		return 2;
	}

	binfd = open(updatefile, O_RDONLY | O_RSYNC);
	if (binfd < 0)
		error(1, errno, "Error opening update file");

	micro_update_parse_footer_v1(binfd, &ftr);

	if (ftr.model != modelnum) {
		fprintf(stderr, "This update is for a %04X, not a %04X.\n",
			ftr.model, modelnum);
		return 1;
	}

	/* gcc warns this pointer has alignment issues in packed structure. */
	bin_size = (uint32_t)ftr.bin_size;

	/* Check file is 128-byte aligned */
	if (bin_size & 0x7F)
		error(1, 0, "Binary file must be 128-byte aligned!");

	if ((revision == ftr.revision) && !force_flag) {
		printf("Already running supervisor revision %d, not updating\n", revision);
		return 0;
	}

	printf("Updating from revision %d to %d\n", revision, ftr.revision);

	if (dry_run_flag) {
		printf("Dry run specified, closing.\n");
		return 0;
	}

	fflush(stdout);

	/*
	 * Let the message print out.  Some of the flash operations will
	 * cause the micro to drop some chars if they output while we touch 
	 * flash 
	 */
	usleep(1000*10);

	/* Write magic key and length/location information */
	if (spokestream16(i2cfd, i2caddr, SUPER_FL_MAGIC_KEY0, (uint16_t *)&magic_key, 4) < 0)
		error(1, errno, "Failed to write magic key");

	if (spokestream16(i2cfd, i2caddr, SUPER_FL_SZ0, (uint16_t *)&bin_size, 4) < 0)
		error(1, errno, "Failed to bin length");

retry:
	lseek(binfd, 0, SEEK_SET);

	if (retries == 0) {
		fprintf(stderr, "Failed to update microcontroller, contact support\n");
		return 1;
	}
	retries--;

	/* If flash is already opened from a previous action, close it to reset the flash state. */
	status = speek16(i2cfd, i2caddr, SUPER_FL_FLASH_STS) & 0xff;
	if (status != STATUS_CLOSED) {
		spoke16(i2cfd, i2caddr, SUPER_FL_FLASH_CMD, SUPER_CLOSE_FLASH);
		status = speek16(i2cfd, i2caddr, SUPER_FL_FLASH_STS) & 0xff;
		if (status != STATUS_CLOSED) {
			fprintf(stderr, "Couldn't re-close flash!\n");
			goto retry;
		}
	}

	/* Poll until flash is opened. This also has to check/erase flash
	 * which happens while interrupts are disabled for flash safety. Because
	 * interrupts are disabled, I2C transactions get stalled, and can
	 * generate errors. Wait a long timeout before trying to talk to the
	 * uC again.
	 */
	spoke16(i2cfd, i2caddr, SUPER_FL_FLASH_CMD, SUPER_OPEN_FLASH);
	sleep(1);
	status = speek16(i2cfd, i2caddr, SUPER_FL_FLASH_STS) & 0xff;
	if (status != STATUS_READY) {
		fprintf(stderr, "Failed to open flash!\n");
		if (status != STATUS_CLOSED)
			flash_print_error(status);
		goto retry;
	}

	/* Write BIN to MCU via I2C */
	for (i = bin_size; i; i -= 128) {
		printf("\r%d/%d", bin_size - i, bin_size);
		fflush(stdout);
		ret = read(binfd, buf, 128);
		if (ret < 0) {
			fprintf(stderr, "Error: %s, Error reading from BIN @ %d", strerror(errno), bin_size - i);
			goto retry;
		} else if (ret < 128) {
			fprintf(stderr, "Error: short read from  bin, got %d instead of 128\n", ret);
			goto retry;
		} else {
			crc = (uint16_t) crc8((uint8_t *)buf, 128);

			/*
			 * Prefer streaming interface, but fall back to individual pokes if
			 * larger writes are failing (might be interrupted?)
			 */
			if (retries > 5)
				spokestream16(i2cfd, i2caddr, SUPER_FL_BLOCK_DATA,  buf, 128);
			else
				for (int x = 0; x < 64; x++)
					spoke16(i2cfd, i2caddr, SUPER_FL_BLOCK_DATA + x, buf[x]);

			spoke16(i2cfd, i2caddr, SUPER_FL_BLOCK_CRC, crc);
			spoke16(i2cfd, i2caddr, SUPER_FL_FLASH_CMD, SUPER_WRITE_BLOCK);

			/* There is some unknown amount of time for a write to complete, its based
			 * on the current uC clocks and all of that, but 2 milliseconds should be
			 * enough in most cases. Most of the time is taken up by the decryption of
			 * the block. However, the actual flash write is a non-zero time too. During
			 * which interrupts are disabled for flash safety. The timeout helps ensure
			 * the process completes before we start polling for state.
			 */
			usleep(2000);
			do {
				status = speek16(i2cfd, i2caddr, SUPER_FL_FLASH_STS) & 0xff;
			} while (status == STATUS_WAIT);

			/* Once wait state is complete, check status to ensure no errors */
			if (status != STATUS_IN_PROC &&
			    status != STATUS_DONE) {
				flash_print_error(status);
				goto retry;
			}
		}
	}

	/* Do a DONE check to make sure both sides moved as much data as they
	 * both expected. If uC is still IN_PROC then the full amount of data
	 * was not received.
	 */
	if (status != STATUS_DONE) {
		printf("\r                            ");
		fprintf(stderr, "\rError: Microcontroller not DONE, retrying\n");
		goto retry;
	} else {
		printf("\r                            ");
		printf("\rWrote %d byte supervisor update\n", bin_size);
	}

	spoke16(i2cfd, i2caddr, SUPER_FL_FLASH_CMD, SUPER_CLOSE_FLASH);
	/* Poll until flash is closed*/
	while (1) {
		status = speek16(i2cfd, i2caddr, SUPER_FL_FLASH_STS) & 0xff;
		if (status == STATUS_CLOSED)
			break;
	}

	/*
	 * If there is a valid image when the microcontroller starts up, it will
	 * switch to it on the next startup. However, the microcontroller does not
	 * normally reboot from the main cpu running a reboot. To apply an update
	 * in the field, we can tell it for the next linux reboot to cause a full
	 * reset for the microcontroller as well.
	 */
	spoke16(i2cfd, i2caddr, SUPER_FL_FLASH_CMD, SUPER_APPLY_REBOOT);
	printf("Update succeeded. On the next reboot the microcontroller update "
	       "will be live. This will force the USB console device to "
	       "disconnect momentarily while the update applies.\n");

	return 0;
}

void usage(char **argv)
{
	fprintf(stderr,
		"Usage: %s [OPTION] ...\n"
		"embeddedTS supervisory microcontroller update utility\n"
		"\n"
		"  -i, --info             Print current revision information and close\n"
		"  -f, --force            Update even if revisions match (not recommended).\n"
		"                         Requires -u.\n"
		"  -n, --dry-run          Check file and current revision, prints the changes\n"
		"                         it would make but does not update.  Requires -u.\n"
		"  -u, --update <file>    Update file.\n"
		"  -b, --bus              Override default i2c bus\n"
		"  -c, --chip-addr        Override default i2c chip address\n"
		"  -h, --help             This message\n"
		"\n",
		argv[0]
	);
}

int main(int argc, char *argv[])
{
	int option_index = 0;
	char *dt_modelname;
	int opt_bus = -1;
	int opt_chip_addr = -1;
	int c;

	dry_run_flag = 0;
	force_flag = 0;
	info_flag = 0;
	updatefile = 0;

	if (argc < 2) {
		usage(argv);
		return 1;
	}

	static struct option long_options[] = {
		{ "info", no_argument, NULL, 'i' },
		{ "force", no_argument, NULL, 'f' },
		{ "update", required_argument, NULL, 'u' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "chip-addr", required_argument, NULL, 'c' },
		{ "bus", required_argument, NULL, 'b' },
		{ "help", no_argument, NULL, 'h' },
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "u:nihfc:b:", long_options, &option_index)) != -1) {
		switch (c) {
		case 'f':
			force_flag = 1;
			break;
		case 'h':
			usage(argv);
			break;
		case 'i':
			info_flag = 1;
			break;
		case 'n':
			dry_run_flag = 1;
			break;
		case 'c':
			opt_chip_addr = strtoul(optarg, NULL, 0);
			break;
		case 'b':
			opt_bus = strtoul(optarg, NULL, 0);
			break;
		case 'u':
			updatefile = optarg;
			break;
		case '?':
		default:
			printf("Unexpected argument \"%s\"\n", optarg);
			return 1;
		}
	}

	if ((dry_run_flag || force_flag) && updatefile == NULL) {
		printf("Must specify the update file\n");
		return 1;
	}

	dt_modelname = get_dt_model();

	if (strncmp(dt_modelname, "TS-7970", strlen("TS-7970")) == 0) {
		if (opt_bus == -1)
			opt_bus = 0;
		if (opt_chip_addr == -1)
			opt_chip_addr = 0x10;
		return do_renesas_7970_update(opt_bus, opt_chip_addr);
	} else if (strncmp(dt_modelname, "TS-7250-V3", strlen("TS-7250-V3")) == 0) {
		if (opt_bus == -1)
			opt_bus = 0;
		if (opt_chip_addr == -1)
			opt_chip_addr = 0x10;

		return do_common_supervisor_update(opt_bus, opt_chip_addr);
	}

	printf("This updater is not supported on this system.\n");
	return 1;
}
