################################################
# Copyright (C) 2020 Intel Corporation.
# All rights reserved.
#
# SPDX-License-Identidfier: Apache-2.0
#
################################################

CFLAGS := -I/usr/include/json-c -ljson-c -static
CC := clang

all: guest_rtc_monitor.c
	$(CC) $^ $(CFLAGS) -o guest_rtc_monitor

clean:
	$(RM) guest_rtc_monitor
