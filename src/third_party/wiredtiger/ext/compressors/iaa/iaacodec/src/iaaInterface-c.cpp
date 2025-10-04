#include "iaaInterface-c.h"
#include "IAACompressionCodecDeflate.h"

uint32_t
getMaxCompressedDataSize(uint32_t srcLen)
{
    static thread_local DB::IAA::CompressionCodecDeflate codec = DB::IAA::CompressionCodecDeflate();
    return codec.getMaxCompressedDataSize(srcLen);
}

uint32_t
doCompressData(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *source,
  uint32_t source_size, uint8_t *dest, uint32_t dest_size)
{
    static thread_local DB::IAA::CompressionCodecDeflate codec =
      DB::IAA::CompressionCodecDeflate(compressor, session);
    return codec.doCompressData(source, source_size, dest, dest_size);
}

void
doDecompressData(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *source,
  uint32_t source_size, uint8_t *dest, uint32_t uncompressed_size, uint32_t *result_lenp)
{
    static thread_local DB::IAA::CompressionCodecDeflate codec =
      DB::IAA::CompressionCodecDeflate(compressor, session);
    codec.doDecompressData(source, source_size, dest, uncompressed_size, result_lenp);
}
