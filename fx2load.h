#pragma once

#include "libusb.h"

int usb_load_firmware(libusb_device_handle* usb_handle);
