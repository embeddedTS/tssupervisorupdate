#pragma once

int speekstream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size);
int spokestream16(int i2cfd, int i2caddr, uint16_t addr, uint16_t *data, int size);
int micro_init(int i2cbus, int i2caddr);
void spoke16(int i2cfd, int i2caddr, uint16_t addr, uint16_t data);
uint16_t speek16(int i2cfd, int i2caddr, uint16_t addr);
int v0_stream_write(int i2cfd, int i2caddr, uint8_t *data, int bytes);
int v0_stream_read(int i2cfd, int i2caddr, uint8_t *data, int bytes);

typedef enum update_method {
	UPDATE_V0,
	UPDATE_V1,
} update_meth_t;

typedef struct board {
	const char *compatible;
	uint16_t modelnum;
	int i2c_bus;
	int i2c_chip;
	update_meth_t method;
} board_t;

