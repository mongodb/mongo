#include <thread>
#include <cstdio>
#include "IAACompressionCodecDeflate.h"

namespace DB::IAA {
/* Local compressor structure. */
typedef struct {
    WT_COMPRESSOR compressor; /* Must come first */
    WT_EXTENSION_API *wt_api; /* Extension API */
} iaa_COMPRESSOR;

static void
iaa_message(
  WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call, const char *info, int msg)
{
    if (compressor == nullptr || session == nullptr) {
        return;
    }
    WT_EXTENSION_API *wt_api;

    wt_api = ((iaa_COMPRESSOR *)compressor)->wt_api;

    (void)wt_api->err_printf(wt_api, session, "iaa message: %s %s: %d", call, info, msg);
}

qpl_job *DeflateJobHWPool::jobPool[jobPoolSize];
std::atomic_bool DeflateJobHWPool::jobLock[jobPoolSize];

DeflateJobHWPool &
DeflateJobHWPool::instance(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    static DeflateJobHWPool ret(compressor, session);
    return ret;
}

DeflateJobHWPool::DeflateJobHWPool(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    iaa_message(compressor, session, "QPL Library Version", qpl_get_library_version(), 0);
    printf("QPL Library Version:%s\n", qpl_get_library_version());
    if (initJobPool() < 0) {
        jobPoolEnabled = false;
        iaa_message(compressor, session, "DeflateJobHWPool",
          "initializing unsucessfully! Please check if IAA hardware support! Here run QPL software "
          "instead of hardware.",
          -2);
    } else {
        jobPoolEnabled = true;
    }
    printf(
      "QPL Library Version:%s HW job Pool Enabled:%d\n", qpl_get_library_version(), jobPoolEnabled);
}

DeflateJobHWPool::~DeflateJobHWPool()
{
    destroyJobPool();
}

HardwareCodecDeflate::HardwareCodecDeflate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    hwEnabled = DeflateJobHWPool::instance(compressor, session).jobPoolReady();
}

HardwareCodecDeflate::~HardwareCodecDeflate()
{
    // Nothing to do.
}

uint32_t
HardwareCodecDeflate::doCompressData(
  uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t dest_size) const
{
    uint32_t job_id = 0;
    qpl_job *job_ptr = DeflateJobHWPool::instance().acquireJob(&job_id);
    if (job_ptr == nullptr) {
        return 0;
    }
    qpl_status status;
    uint32_t compressed_size;

    job_ptr->op = qpl_op_compress;
    job_ptr->next_in_ptr = source;
    job_ptr->next_out_ptr = dest;
    job_ptr->available_in = source_size;
    job_ptr->level = qpl_default_level;
    job_ptr->available_out = dest_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_GZIP_MODE |
      QPL_FLAG_LAST | QPL_FLAG_OMIT_VERIFY;

    // Compression
    status = qpl_execute_job(job_ptr);
    compressed_size = 0;
    if (QPL_STS_OK == status) {
        compressed_size = job_ptr->total_out;
    }

    DeflateJobHWPool::instance().releaseJob(job_id);
    return compressed_size;
}

uint32_t
HardwareCodecDeflate::doDecompressData(
  uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t uncompressed_size) const
{
    uint32_t job_id = 0;
    qpl_job *job_ptr = DeflateJobHWPool::instance().acquireJob(&job_id);
    if (job_ptr == nullptr) {
        return 0;
    }
    qpl_status status;
    uint32_t decompressed_size;

    // Performing a decompression operation.
    job_ptr->op = qpl_op_decompress;
    job_ptr->next_in_ptr = source;
    job_ptr->next_out_ptr = dest;
    job_ptr->available_in = source_size;
    job_ptr->available_out = uncompressed_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_GZIP_MODE | QPL_FLAG_LAST;

    // Decompression
    status = qpl_execute_job(job_ptr);
    decompressed_size = 0;
    if (status == QPL_STS_OK) {
        decompressed_size = job_ptr->total_out;
    }

    DeflateJobHWPool::instance().releaseJob(job_id);
    return decompressed_size;
}

SoftwareCodecDeflate::SoftwareCodecDeflate()
{
    jobSWPtr = nullptr;
}

SoftwareCodecDeflate::~SoftwareCodecDeflate()
{
    if (nullptr != jobSWPtr) {
        qpl_fini_job(jobSWPtr);
    }
}

qpl_job *
SoftwareCodecDeflate::getJobCodecPtr()
{
    if (nullptr == jobSWPtr) {
        qpl_status status;
        uint32_t size = 0;
        // Job initialization
        status = qpl_get_job_size(qpl_path_software, &size);
        if (status != QPL_STS_OK) {
            // nothing to do
        }

        jobSWbuffer = std::make_unique<uint8_t[]>(size);
        jobSWPtr = reinterpret_cast<qpl_job *>(jobSWbuffer.get());

        status = qpl_init_job(qpl_path_software, jobSWPtr);
        if (status != QPL_STS_OK) {
            // nothing to do
        }
    }
    return jobSWPtr;
}

uint32_t
SoftwareCodecDeflate::doCompressData(
  uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t dest_size)
{
    qpl_status status;
    qpl_job *job_ptr = getJobCodecPtr();

    /*Performing a compression operation*/
    job_ptr->op = qpl_op_compress;
    job_ptr->next_in_ptr = source;
    job_ptr->next_out_ptr = dest;
    job_ptr->available_in = source_size;
    job_ptr->available_out = dest_size;
    job_ptr->level = qpl_high_level;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_GZIP_MODE |
      QPL_FLAG_LAST | QPL_FLAG_OMIT_VERIFY;

    // Compression
    status = qpl_execute_job(job_ptr);
    if (status != QPL_STS_OK) {
        // nothing to do
    }

    return job_ptr->total_out;
}

uint32_t
SoftwareCodecDeflate::doDecompressData(
  uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t uncompressed_size)
{
    qpl_status status;
    qpl_job *job_ptr = getJobCodecPtr();

    /*Performing a decompression operation*/
    job_ptr->op = qpl_op_decompress;
    job_ptr->next_in_ptr = source;
    job_ptr->next_out_ptr = dest;
    job_ptr->available_in = source_size;
    job_ptr->available_out = uncompressed_size;
    job_ptr->flags = QPL_FLAG_FIRST | QPL_FLAG_GZIP_MODE | QPL_FLAG_LAST;

    // Decompression
    status = qpl_execute_job(job_ptr);
    if (status == QPL_STS_OK) {
        return job_ptr->total_out;
    } else {
        return 0;
    }
}

CompressionCodecDeflate::CompressionCodecDeflate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    hwCodec = std::make_unique<HardwareCodecDeflate>(compressor, session);
    swCodec = std::make_unique<SoftwareCodecDeflate>();
}

#define DEFLATE_COMPRESSBOUND(isize) \
    ((isize) + ((isize) >> 12) + ((isize) >> 14) + ((isize) >> 25) + 13) // Aligned with ZLIB
uint32_t
CompressionCodecDeflate::getMaxCompressedDataSize(uint32_t uncompressed_size) const
{
    return DEFLATE_COMPRESSBOUND(uncompressed_size);
}

uint32_t
CompressionCodecDeflate::doCompressData(
  uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t dest_size) const
{
    uint32_t res = 0;
    if (hwCodec->hwEnabled)
        res = hwCodec->doCompressData(source, source_size, dest, dest_size);
    if (0 == res)
        res = swCodec->doCompressData(source, source_size, dest, dest_size);
    return res;
}

void
CompressionCodecDeflate::doDecompressData(uint8_t *source, uint32_t source_size, uint8_t *dest,
  uint32_t uncompressed_size, uint32_t *result_lenp) const
{
    uint32_t res = 0;
    if (hwCodec->hwEnabled)
        res = hwCodec->doDecompressData(source, source_size, dest, uncompressed_size);
    if (0 == res)
        res = swCodec->doDecompressData(source, source_size, dest, uncompressed_size);
    if (result_lenp) {
        *result_lenp = res;
    }
}

} // namespace DB::IAA
