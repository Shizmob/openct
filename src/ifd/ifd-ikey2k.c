/*
 * driver for Rainbow iKey 2032 devices
 *
 * Copyright (C) 2003, Andreas Jellinghaus <aj@dungeon.inka.de>
 * Copyright (C) 2003, Olaf Kirch <okir@suse.de>
 * Copyright (C) 2021, Shiz <hi@shiz.me>
 */

#include "internal.h"
#include <stdlib.h>
#include <string.h>


#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif


enum ikey2k_command {
	IKEY2K_CMD_RESET = 0,
	IKEY2K_CMD_GET_RESPONSE = 1,
	IKEY2K_CMD_GET_STATUS = 2,
	IKEY2K_CMD_LED_CONTROL = 3,
	IKEY2K_CMD_UNK_DIRECTORY = 4,
	IKEY2K_CMD_OPEN = 5,
	IKEY2K_CMD_CLOSE = 6,
	IKEY2K_CMD_READ = 7,
	IKEY2K_CMD_WRITE = 8,
	IKEY2K_CMD_UNK_DECREMENT = 9,
	IKEY2K_CMD_CREATE_DIR = 10,
	IKEY2K_CMD_CREATE_FILE = 11,
	IKEY2K_CMD_DELETE_DIR = 12,
	IKEY2K_CMD_DELETE_FILE = 13,
	IKEY2K_CMD_UNK_VERIFY1 = 14,
	IKEY2K_CMD_UNK_VERIFY2 = 15,
	IKEY2K_CMD_UNK_HASH = 16,
	IKEY2K_CMD_GEN_RANDOM = 18,
	IKEY2K_CMD_CARD_CTL = 22,
	IKEY2K_CMD_CARD_IO = 23,
};

static int ikey2k_do_send(ifd_reader_t *reader, enum ikey2k_command cmd, const unsigned char *data, size_t len)
{
	uint16_t value = 0, index = 0;
	if (len > 0) {
		value |= *data++; len--;
	}
	if (len > 0) {
		value |= *data++ << 8; len--;
	}
	if (len > 0) {
		index |= *data++; len--;
	}
	if (len > 0) {
		index |= *data++ << 8; len--;
	}
	return ifd_usb_control(reader->device, 0x41, cmd, value, index, data, len, 1000);
}

static int ikey2k_do_recv(ifd_reader_t *reader, enum ikey2k_command cmd, void *data, size_t len)
{
	return ifd_usb_control(reader->device, 0xC1, cmd, 0, 0, data, len, 1000);
}

static int ikey2k_do_cmd(ifd_reader_t *reader, enum ikey2k_command cmd, const void *indata, size_t inlen, void *outdata, size_t outlen)
{
	int rc;

	rc = ikey2k_do_send(reader, cmd, indata, inlen);
	if (rc >= 0 && outdata && outlen) {
		rc = ikey2k_do_recv(reader, IKEY2K_CMD_GET_RESPONSE, outdata, outlen);
	}
	return rc;
}



enum ikey2k_card_command {
	IKEY2K_CARDCMD_RESET = 0x00,
	IKEY2K_CARDCMD_GET_ATR = 0x01,
	IKEY2K_CARDCMD_UNK = 0x02,
	IKEY2K_CARDCMD_EXCHANGE = 0x03,
};

static int ikey2k_do_card_send(ifd_reader_t *reader, const void *indata, size_t inlen)
{
	return ikey2k_do_send(reader, IKEY2K_CMD_CARD_IO, indata, inlen);
}

static int ikey2k_do_card_recv(ifd_reader_t *reader, unsigned char *outdata, size_t outlen)
{
	return ikey2k_do_recv(reader, IKEY2K_CMD_GET_RESPONSE, outdata, outlen);
}

static int ikey2k_do_card_cmd(ifd_reader_t *reader, enum ikey2k_card_command cmd, int arg1, const void *indata, size_t inlen, void *outdata, size_t outlen)
{
	int rc;
	unsigned char buf[256];
	buf[0] = cmd;
	buf[1] = arg1;
	if (inlen) {
		memcpy(buf + 2, indata, min(sizeof(buf) - 2, inlen));
	}

	rc = ikey2k_do_send(reader, IKEY2K_CMD_CARD_CTL, buf, inlen + 2);
	if (rc >= 0 && outdata && outlen) {
		rc = ikey2k_do_recv(reader, IKEY2K_CMD_GET_RESPONSE, outdata, outlen);
	}
	return rc;
}


static int ikey2k_parse_desc(ifd_reader_t * reader, const unsigned char *desc, int desclen)
{
	/* example: 0d6300062d2dc0808060800119
	 * [0] 0d: length, 6 <= x <= 0x40
	 * [1] 63: unknown, 0x60 <= x <= 0x6F
	 * [2] 0006: firmware version?
	 * [4] 2d: unknown
	 * [5] 2d: unknown
	 * -optional-
	 * [6] c0: unknown
	 * [7] 80: flags: bit 2 indicates auto-flashing LED availability
	 * [8] 80: unknown
	 * [9] 60: unknown
	 * [A] 80: unknown
	 * [B] 01: unknown
	 * [C] 19: ATR length?
	 */
	if (desclen < 6 || desclen > 0x40 || desc[0] != desclen)
		return -1;
	if (desc[1] < 0x60 || desc[1] > 0x6F)
		return -2;
	if (desclen > 0xC && desc[0xC] != 9 && desc[0xC] != 25)
		return -3;
}



/*
 * Driver API
 *
 */


/*
 * Open the reader.
 */
static int ikey2k_open(ifd_reader_t * reader, const char *device_name)
{
	ifd_device_t *dev;
	ifd_device_params_t params;

	reader->name = "Rainbow Technologies iKey 2032";
	reader->nslots = 1;
	if (!(dev = ifd_device_open(device_name)))
		return -1;
	if (ifd_device_type(dev) != IFD_DEVICE_TYPE_USB) {
		ct_error("ikey2k: device %s is not a USB device", device_name);
		ifd_device_close(dev);
		return -1;
	}

	params = dev->settings;
	params.usb.interface = 0;
	if (ifd_device_set_parameters(dev, &params) < 0) {
		ct_error("ikey2k: setting parameters failed", device_name);
		ifd_device_close(dev);
		return -1;
	}

	reader->device = dev;
	return 0;
}

/*
 * Close the reader.
 */
static int ikey2k_close(ifd_reader_t * reader)
{
	return 0;
}

/*
 * Power up the reader.
 */
static int ikey2k_activate(ifd_reader_t * reader)
{
	char buffer[256];
	/* reset reader and parse desc */
	int desclen = ikey2k_do_recv(reader, IKEY2K_CMD_RESET, buffer, sizeof(buffer));
	if (desclen <= 0 || !ikey2k_parse_desc(reader, buffer, desclen)) {
		ct_error("ikey2k: failed to reset reader");
		return -1;
	}
	return 0;
}

/*
 * Power down the reader.
 */
static int ikey2k_deactivate(ifd_reader_t * reader)
{
	char buffer[2];
	/* reset card to power it down */
	if (ikey2k_do_card_cmd(reader, IKEY2K_CARDCMD_RESET, 0, buffer, 0, buffer, 2) != 1 || buffer[0] != 0) {
		ct_error("ikey2k: failed to reset card");
		return -1;
	}
	return 0;
}

/*
 * Card status - always present.
 */
static int ikey2k_card_status(ifd_reader_t * reader, int slot, int *status)
{
	*status = IFD_CARD_PRESENT;
	return 0;
}

/*
 * Reset card.
 */
static int ikey2k_card_reset(ifd_reader_t * reader, int slot, void *atr, size_t size)
{
	unsigned char buffer[256];
	int atrlen;

	/* reset card and get ATR */
	if (ikey2k_do_card_cmd(reader, IKEY2K_CARDCMD_RESET, 0, buffer, 0, buffer, 2) != 1 || buffer[0] != 0)
		goto failed;
	atrlen = ikey2k_do_card_cmd(reader, IKEY2K_CARDCMD_GET_ATR, 25, buffer, 0, buffer, 25);
	if (atrlen != 25)
		atrlen = ikey2k_do_card_cmd(reader, IKEY2K_CARDCMD_GET_ATR, 9, buffer, 0, buffer, 9);
	if (atrlen < 9 || atrlen > size)
		goto failed;

	memcpy(atr, buffer, atrlen);
	return atrlen;

failed:
	ct_error("ikey2k: failed to activate token");
	return -1;
}

/*
 * Send/receive routines.
 */
static int ikey2k_send(ifd_reader_t * reader, unsigned int dad,
		       const unsigned char *buffer, size_t len)
{
	return ikey2k_do_card_send(reader, buffer, len);
}

static int ikey2k_recv(ifd_reader_t * reader, unsigned int dad,
		       unsigned char *buffer, size_t len, long timeout)
{
	return ikey2k_do_card_recv(reader, buffer, len);
}

/*
 * Driver operations.
 */
const static struct ifd_driver_ops ikey2k_driver = {
	.open        = ikey2k_open,
	.close       = ikey2k_close,
	.activate    = ikey2k_activate,
	.deactivate  = ikey2k_deactivate,
	.card_status = ikey2k_card_status,
	.card_reset  = ikey2k_card_reset,
	.send        = ikey2k_send,
	.recv        = ikey2k_recv,
};

/*
 * Module initialisation.
 */
void ifd_ikey2k_register(void)
{
	ifd_driver_register("ikey2k", &ikey2k_driver);
}
