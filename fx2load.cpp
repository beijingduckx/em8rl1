#include <stdio.h>
#include <stdint.h>
#include "libusb.h"

// Built-in firmware hex strings.
static const char* firmware[] = {
#include "fx2firm.inc"
	NULL };

#define FIRMWARE_MAX_SIZE_PER_LINE 64
static uint8_t firmware_dat[FIRMWARE_MAX_SIZE_PER_LINE];

//----------------------------------------------------------------------
// USB write RAM
//----------------------------------------------------------------------
#define USB_WRITE_RAM_MAX_SIZE 64
int usb_write_ram(int addr, uint8_t* dat, int size, libusb_device_handle* usb_handle) {

	for (int i = 0; i < size; i += USB_WRITE_RAM_MAX_SIZE) {
		LONG len = (size - i > USB_WRITE_RAM_MAX_SIZE) ? USB_WRITE_RAM_MAX_SIZE : size - i;
		int ret = libusb_control_transfer(usb_handle, LIBUSB_REQUEST_TYPE_VENDOR, 0xa0, addr + i, 0, dat + i, (uint16_t)len, 1000);
		if (ret < 0) {
			fprintf(stderr, "USB: Write Ram at %04x (len %d) failed.\n", addr + i, len);
			return -1;
		}
	}
	return 0;
}

//----------------------------------------------------------------------
// USB load firmware
//----------------------------------------------------------------------
int usb_load_firmware(libusb_device_handle* usb_handle) {
	int ret;

	// Take the CPU into RESET
	uint8_t dat = 1;
	ret = usb_write_ram(0xe600, &dat, sizeof(dat), usb_handle);
	if (ret < 0) {
		return -1;
	}

	// Load firmware
	int size, addr, record_type, tmp_dat;
	for (int i = 0; firmware[i] != NULL; i++) {
		const char* p = firmware[i] + 1;

		// Extract size
		ret = sscanf(p, "%2x", &size);
		if (ret == 0) {
			return -1;
		}
		if (size > FIRMWARE_MAX_SIZE_PER_LINE) {
			return -1;
		}
		p += 2;

		// Extract addr
		ret = sscanf(p, "%4x", &addr);
		if (ret == 0) {
			return -1;
		}
		p += 4;

		// Extract record type
		ret = sscanf(p, "%2x", &record_type);
		if (ret == 0) {
			return -1;
		}
		p += 2;

		// Write program to EZ-USB's RAM (record_type==0).
		if (record_type == 0) {
			for (int j = 0; j < size; j++) {
				ret = sscanf(p, "%2x", &tmp_dat);
				firmware_dat[j] = tmp_dat & 0xff;
				if (ret == 0) {
					return -1;
				}
				p += 2;
			}

			ret = usb_write_ram(addr, firmware_dat, size, usb_handle);
			if (ret < 0) {
				return -1;
			}
		}
	}

	// Take the CPU out of RESET (run)
	dat = 0;
	ret = usb_write_ram(0xe600, &dat, sizeof(dat), usb_handle);
	if (ret < 0) {
		return -1;
	}

	return 0;
}
