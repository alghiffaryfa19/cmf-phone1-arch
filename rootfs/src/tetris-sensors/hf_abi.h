/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace ABI of MediaTek's sensor hub driver (/dev/hf_manager), copied from
 * kernel_device_modules drivers/misc/mediatek/sensor/2.0/core/hf_sensor_io.h.
 */
#ifndef TETRIS_HF_ABI_H
#define TETRIS_HF_ABI_H

#include <linux/ioctl.h>
#include <stdbool.h>
#include <stdint.h>

/* hf_sensor_type.h: Android sensor type numbers */
#define HF_ACCEL     1
#define HF_LIGHT     5
#define HF_PROXIMITY 8

enum {
	HF_MANAGER_SENSOR_DISABLE,
	HF_MANAGER_SENSOR_ENABLE,
	HF_MANAGER_SENSOR_FLUSH,
};

enum {
	DATA_ACTION,
	FLUSH_ACTION,
};

struct hf_manager_batch {
	int64_t delay;      /* ns */
	int64_t latency;
} __attribute__((packed, aligned(4)));

struct hf_manager_cmd {
	uint8_t sensor_type;
	uint8_t action : 4;
	uint8_t down_sample : 1;
	uint8_t length;
	uint8_t padding[1];
	int8_t data[48] __attribute__((aligned(4)));
} __attribute__((packed, aligned(4)));

struct hf_manager_event {
	int64_t timestamp;
	uint8_t sensor_type;
	uint8_t accurancy;
	uint8_t action;
	uint8_t padding[1];
	union {
		int32_t word[16];
		int8_t byte[0];
	};
} __attribute__((packed, aligned(4)));

struct sensor_info {
	uint8_t sensor_type;
	uint8_t padding[3];
	uint32_t gain;
	char name[16];
	char vendor[16];
} __attribute__((packed, aligned(4)));

struct ioctl_packet {
	uint8_t sensor_type;
	uint8_t padding[3];
	union {
		bool status;
		int8_t byte[64];
	};
} __attribute__((packed, aligned(4)));

#define HF_MANAGER_REQUEST_REGISTER_STATUS  _IOWR('a', 1, struct ioctl_packet)
#define HF_MANAGER_REQUEST_SENSOR_INFO      _IOWR('a', 6, struct ioctl_packet)

#endif
