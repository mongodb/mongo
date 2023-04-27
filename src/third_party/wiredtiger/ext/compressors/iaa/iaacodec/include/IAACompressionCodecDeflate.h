#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <sys/mman.h>
#include <iostream>
#include <atomic>
#include <qpl/qpl.h>
#include <vector>
#include <map>
#include <x86intrin.h>
#include <memory>
#include <thread>
#include <wiredtiger.h>
#include <wiredtiger_ext.h>
namespace DB::IAA {

class DeflateJobHWPool {
public:
    DeflateJobHWPool(WT_COMPRESSOR *compressor, WT_SESSION *session);
    ~DeflateJobHWPool();
    static DeflateJobHWPool &instance(
      WT_COMPRESSOR *compressor = nullptr, WT_SESSION *session = nullptr);
    static constexpr auto jobPoolSize = 256;
    static constexpr qpl_path_t PATH = qpl_path_hardware;
    static qpl_job *jobPool[jobPoolSize];
    static std::atomic_bool jobLock[jobPoolSize];
    bool jobPoolEnabled;
    std::atomic_uint64_t jobIndex;

    inline bool
    jobPoolReady()
    {
        return jobPoolEnabled;
    }
    inline qpl_job *
    acquireJob(uint32_t *job_id)
    {
        if (jobPoolEnabled) {
            uint32_t retry = 0;
            auto index = jobIndex.fetch_add(1) % jobPoolSize;
            while (tryLockJob(index) == false) {
                retry++;
                if (retry > jobPoolSize) {
                    return nullptr;
                }
                index = jobIndex.fetch_add(1) % jobPoolSize;
            }
            *job_id = jobPoolSize - index;
            return jobPool[index];
        } else
            return nullptr;
    }
    inline qpl_job *
    releaseJob(uint32_t job_id)
    {
        if (!jobPoolEnabled || job_id == 0 || job_id > jobPoolSize)
            return nullptr;
        uint32_t index = jobPoolSize - job_id;
        ReleaseJobObjectGuard _(index);
        return jobPool[index];
    }
    inline qpl_job *
    getJobPtr(uint32_t job_id)
    {
        if (jobPoolEnabled) {
            uint32_t index = jobPoolSize - job_id;
            return jobPool[index];
        } else {
            return nullptr;
        }
    }

private:
    inline int32_t
    get_job_size_helper()
    {
        static uint32_t size = 0;
        if (size == 0) {
            const auto status = qpl_get_job_size(PATH, &size);
            if (status != QPL_STS_OK) {
                return -1;
            }
        }
        return size;
    }

    inline int32_t
    init_job_helper(qpl_job *qpl_job_ptr)
    {
        if (qpl_job_ptr == nullptr) {
            return -1;
        }
        auto status = qpl_init_job(PATH, qpl_job_ptr);
        if (status != QPL_STS_OK) {
            return -1;
        }
        return 0;
    }

    inline int32_t
    initJobPool()
    {
        static bool initialized = false;

        if (initialized == false) {
            const int32_t size = get_job_size_helper();
            if (size < 0)
                return -1;
            for (int i = 0; i < jobPoolSize; ++i) {
                jobPool[i] = nullptr;
                qpl_job *qpl_job_ptr = reinterpret_cast<qpl_job *>(new uint8_t[size]);
                if (init_job_helper(qpl_job_ptr) < 0)
                    return -1;
                jobPool[i] = qpl_job_ptr;
                jobLock[i].store(false);
            }
            initialized = true;
        }
        return 0;
    }

    inline bool
    tryLockJob(size_t index)
    {
        bool expected = false;
        return jobLock[index].compare_exchange_strong(expected, true);
    }

    inline void
    destroyJobPool()
    {
        const uint32_t size = get_job_size_helper();
        for (uint32_t i = 0; i < jobPoolSize && size > 0; ++i) {
            while (tryLockJob(i) == false) {
            }
            if (jobPool[i]) {
                qpl_fini_job(jobPool[i]);
                delete[] jobPool[i];
            }
            jobPool[i] = nullptr;
            jobLock[i].store(false);
        }
    }

    struct ReleaseJobObjectGuard {
        uint32_t index;
        ReleaseJobObjectGuard() = delete;

    public:
        inline ReleaseJobObjectGuard(const uint32_t i) : index(i)
        {
            // Nothing to do.
        }
        inline ~ReleaseJobObjectGuard()
        {
            jobLock[index].store(false);
        }
    };
};
class SoftwareCodecDeflate {
public:
    SoftwareCodecDeflate();
    ~SoftwareCodecDeflate();
    uint32_t doCompressData(
      uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t dest_size);
    uint32_t doDecompressData(
      uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t uncompressed_size);

private:
    /*Software Job Codec Ptr.*/
    qpl_job *jobSWPtr;
    std::unique_ptr<uint8_t[]> jobSWbuffer;
    qpl_job *getJobCodecPtr();
};

class HardwareCodecDeflate {
public:
    bool hwEnabled;
    HardwareCodecDeflate(WT_COMPRESSOR *compressor, WT_SESSION *session);
    ~HardwareCodecDeflate();
    uint32_t doCompressData(
      uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t dest_size) const;
    uint32_t doDecompressData(
      uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t uncompressed_size) const;
};

class CompressionCodecDeflate {
public:
    CompressionCodecDeflate(WT_COMPRESSOR *compressor = nullptr, WT_SESSION *session = nullptr);
    uint32_t doCompressData(
      uint8_t *source, uint32_t source_size, uint8_t *dest, uint32_t dest_size) const;
    void doDecompressData(uint8_t *source, uint32_t source_size, uint8_t *dest,
      uint32_t uncompressed_size, uint32_t *result_lenp = NULL) const;
    uint32_t getMaxCompressedDataSize(uint32_t uncompressed_size) const;

private:
    std::unique_ptr<HardwareCodecDeflate> hwCodec;
    std::unique_ptr<SoftwareCodecDeflate> swCodec;
};

} // namespace DB::IAA
