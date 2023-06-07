/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef UNALIGNED_H
#define UNALIGNED_H

#include "stdint.h"
#include "string.h"

static inline uint16_t load_u16(uint8_t * buf) {
	uint16_t ret;
	memcpy(&ret, buf, sizeof(ret));
	return ret;
}

static inline uint32_t load_u32(uint8_t * buf) {
	uint32_t ret;
	memcpy(&ret, buf, sizeof(ret));
	return ret;
}

static inline uint64_t load_u64(uint8_t * buf) {
	uint64_t ret;
	memcpy(&ret, buf, sizeof(ret));
	return ret;
}

static inline uintmax_t load_umax(uint8_t * buf) {
	uintmax_t ret;
	memcpy(&ret, buf, sizeof(ret));
	return ret;
}

static inline void store_u16(uint8_t * buf, uint16_t val) {
	memcpy(buf, &val, sizeof(val));
}

static inline void store_u32(uint8_t * buf, uint32_t val) {
	memcpy(buf, &val, sizeof(val));
}

static inline void store_u64(uint8_t * buf, uint64_t val) {
	memcpy(buf, &val, sizeof(val));
}

static inline void store_umax(uint8_t * buf, uintmax_t val) {
	memcpy(buf, &val, sizeof(val));
}

#endif
