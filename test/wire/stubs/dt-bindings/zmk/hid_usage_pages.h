/*
 * Host-test stub for <dt-bindings/zmk/hid_usage_pages.h>.
 *
 * Values copied from upstream ZMK (dt-bindings/zmk/hid_usage_pages.h,
 * Copyright (c) 2020 The ZMK Contributors, MIT), which torabo_common/binding.h
 * depends on for the &kp param encoding. Only what binding.h uses.
 */
#pragma once

#define ZMK_HID_USAGE(page, id) ((page << 16) | id)

#define HID_USAGE_KEY (0x07)      /* Keyboard/Keypad */
#define HID_USAGE_CONSUMER (0x0C) /* Consumer */
