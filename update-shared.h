#pragma once

void flash_print_error(uint8_t status);

/* Read-back status values */
/* Default value of status, closed */
#define STATUS_CLOSED 0x00
/* Once the flashwrite process is set up, but no data written */
#define STATUS_READY 0xAA
/* Flashwrite process has seen full length of data written and is considered done */
#define STATUS_DONE 0x01
/* Flashwrite is in process, meaning SOME data has been written, but not the full length */
#define STATUS_IN_PROC 0x02
/* A CRC error occurred at ANY point during data write. Note that this status
 * is not set if CRC fails for open process, the system simply does not open
 */
#define STATUS_CRC_ERR 0x03
/* An error occurred while trying to erase the actual flash */
#define STATUS_ERASE_ERR 0x04
/* An error occurred at ANY point during data write. */
#define STATUS_WRITE_ERR 0x05
/* Erase was successful, but, the area to be written was not blank */
#define STATUS_NOT_BLANK 0x06
/* A BSP error opening and closing flash. Most errors are buggy code, configurations, or unrecoverable */
#define STATUS_OPEN_ERR 0x07
/* Wait state while processing a write */
#define STATUS_WAIT 0x08
/* Request the uC reboot at any time after its open status */
#define STATUS_RESET 0x55

/*
 * The updates themselves are encrypted/signed, but the below key is just used to
 * prevent unintentional writes to i2c causing writes to the flash.
 */
static const uint32_t magic_key = 0xf092c858;
