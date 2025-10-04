#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

uint32_t getMaxCompressedDataSize(uint32_t srcLen);
uint32_t doCompressData(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *source,
  uint32_t source_size, uint8_t *dest, uint32_t dest_size);
void doDecompressData(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *source,
  uint32_t source_size, uint8_t *dest, uint32_t uncompressed_size, uint32_t *result_lenp);

#ifdef __cplusplus
}
#endif
