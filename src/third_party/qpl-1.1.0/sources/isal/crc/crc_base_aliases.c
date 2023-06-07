/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "crc.h"
#include "crc64.h"
#include <stdint.h>

unsigned int crc32_iscsi(const uint8_t *buffer, int len, uint32_t crc_init)
{
	return crc32_iscsi_base((uint8_t *)buffer, len, crc_init);
}

uint16_t crc16_t10dif(uint16_t seed, const unsigned char *buf, uint64_t len)
{
	return crc16_t10dif_base(seed, (uint8_t *) buf, len);
}

uint16_t crc16_t10dif_copy(uint16_t seed, uint8_t * dst, uint8_t * src, uint64_t len)
{
	return crc16_t10dif_copy_base(seed, dst, src, len);
}

uint32_t crc32_ieee(uint32_t seed, const unsigned char *buf, uint64_t len)
{
	return crc32_ieee_base(seed, (uint8_t *) buf, len);
}

uint32_t crc32_gzip_refl(uint32_t seed, const unsigned char *buf, uint64_t len)
{
	return crc32_gzip_refl_base(seed, (uint8_t *) buf, len);
}

uint64_t crc64_ecma_refl(uint64_t seed, const uint8_t * buf, uint64_t len)
{
	return crc64_ecma_refl_base(seed, buf, len);
}

uint64_t crc64_ecma_norm(uint64_t seed, const uint8_t * buf, uint64_t len)
{
	return crc64_ecma_norm_base(seed, buf, len);
}

uint64_t crc64_iso_refl(uint64_t seed, const uint8_t * buf, uint64_t len)
{
	return crc64_iso_refl_base(seed, buf, len);
}

uint64_t crc64_iso_norm(uint64_t seed, const uint8_t * buf, uint64_t len)
{
	return crc64_iso_norm_base(seed, buf, len);
}

uint64_t crc64_jones_refl(uint64_t seed, const uint8_t * buf, uint64_t len)
{
	return crc64_jones_refl_base(seed, buf, len);
}

uint64_t crc64_jones_norm(uint64_t seed, const uint8_t * buf, uint64_t len)
{
	return crc64_jones_norm_base(seed, buf, len);
}
