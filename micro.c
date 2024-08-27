#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "micro.h"

int micro_init(int i2cbus, int i2caddr)
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


void spoke16(int i2cfd, int i2caddr, uint16_t addr, uint16_t data)
{
	int ret;

	ret = spokestream16(i2cfd, i2caddr, addr, &data, 2);
	if (ret) {
		perror("Failed to write to supervisory micro");
		exit(1);
	}
}

uint16_t speek16(int i2cfd, int i2caddr, uint16_t addr)
{
	uint16_t data = 0;
	int ret = speekstream16(i2cfd, i2caddr, addr, &data, 2);

	if (ret) {
		perror("Failed to read from supervisory micro");
		exit(1);
	}
	return data;
}

int speekstream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size)
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

int spokestream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size)
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

int v0_stream_read(int twifd, int i2caddr, uint8_t *data, int bytes)
{
	struct i2c_rdwr_ioctl_data packets;
	struct i2c_msg msg;
	int retry = 0;

retry:
	msg.addr = i2caddr;
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

int v0_stream_write(int twifd, int i2caddr, uint8_t *data, int bytes)
{
	struct i2c_rdwr_ioctl_data packets;
	struct i2c_msg msg;

	msg.addr = i2caddr;
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
