// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/shell/named_pipe_test_helper.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/query/virtual_collection/multi_bson_stream_cursor.h"
#include "mongo/db/shard_role/shard_catalog/virtual_collection_options.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/named_pipe/named_pipe.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {
uint64_t synchronizedRandom() {
    static StaticImmortal<synchronized_value<PseudoRandom>> random{
        PseudoRandom{SecureRandom{}.nextInt64()}};
    return (*random)->nextInt64();
}

/** `n` sizes from range [`min`,`max`] */
std::vector<size_t> randomLengths(size_t n, size_t min, size_t max) {
    PseudoRandom random{synchronizedRandom()};
    std::vector<size_t> vec;
    vec.reserve(n);
    for (size_t i = 0; i < n; ++i)
        vec.push_back(min + random.nextInt64(max - min + 1));
    return vec;
}

template <typename Function>
void runAndLogExceptions(const char* method, Function&& function) noexcept {
    try {
        std::forward<Function>(function)();
    } catch (...) {
        LOGV2_ERROR(13212600,
                    "Caught exception",
                    "method"_attr = method,
                    "error"_attr = exceptionToStatus());
    }
}

/**
 * Opens 'pipeWriter' (blocking until a reader attaches), writes 'objects' random BSON objects with
 * stringMinSize <= "string".length() <= stringMaxSize, then closes it. 'pipeWriter' must have
 * already been constructed, i.e. the pipe itself must already exist. Absorbs exceptions because
 * this is called by an async detached thread, so escaping exceptions will cause fuzzer tests to
 * fail as its try blocks are only around the main thread.
 */
void writeToPipeAsyncImpl(std::unique_ptr<NamedPipeOutput> pipeWriter,
                          long objects,
                          long stringMinSize,
                          long stringMaxSize) noexcept {
    const char* method = "NamedPipeHelper::writeToPipeAsync";
    runAndLogExceptions(method, [&] {
        pipeWriter->open();
        for (size_t length : randomLengths(objects, stringMinSize, stringMaxSize)) {
            auto bsonObj = BSONObjBuilder{}
                               .append("length", static_cast<int>(length))
                               .append("string", std::string(length, 'a'))
                               .obj();
            pipeWriter->write(bsonObj.objdata(), bsonObj.objsize());
        }
        pipeWriter->close();
        LOGV2_INFO(13212601,
                   "pipeWriter closed",
                   "method"_attr = method,
                   "pipe"_attr = pipeWriter->getAbsolutePath());
    });
}

/**
 * Opens 'pipeWriter' (blocking until a reader attaches), writes 'objects' BSON objects
 * round-robinned from 'bsonObjs', then closes it. 'pipeWriter' must have already been constructed,
 * i.e. the pipe itself must already exist.
 */
void writeObjectsToPipe(NamedPipeOutput& pipeWriter,
                        long objects,
                        const std::vector<BSONObj>& bsonObjs) {
    uassert(13212602, "bsonObjs must not be empty", !bsonObjs.empty());
    const int kNumBsonObjs = bsonObjs.size();
    pipeWriter.open();
    for (long i = 0; i < objects; ++i) {
        BSONObj bsonObj{bsonObjs[i % kNumBsonObjs]};
        pipeWriter.write(bsonObj.objdata(), bsonObj.objsize());
    }
    pipeWriter.close();
}

/**
 * Detached-thread entry point for writing 'objects' BSON objects round-robinned from 'bsonObjs'.
 */
void writeObjectsToPipeAsyncImpl(std::unique_ptr<NamedPipeOutput> pipeWriter,
                                 long objects,
                                 std::vector<BSONObj> bsonObjs) noexcept {
    const char* method = "NamedPipeHelper::writeToPipeObjectsAsync";
    runAndLogExceptions(method, [&] {
        writeObjectsToPipe(*pipeWriter, objects, bsonObjs);
        LOGV2_INFO(13212603,
                   "pipeWriter closed",
                   "method"_attr = method,
                   "pipe"_attr = pipeWriter->getAbsolutePath());
    });
}

}  // namespace

/**
 * Reads all BSON objects from all named pipes in 'pipeRelativePaths' and returns the following
 * stats in a BSON object:
 *   {
 *     "objects": number of objects read,
 *     "time": { total time consumed in...
 *       "sec":  seconds,
 *       "msec": milliseconds,
 *       "usec": microseconds,
 *       "nsec": nanoseconds,
 *     },
 *     "rate": { data processing rate in...
 *       "mbps": megabytes / second,
 *       "gbps": gigabytes / second,
 *     },
 *     "totalSize": { total size of all objects in
 *       "bytes": bytes,
 *       "kb":    kilobytes,
 *       "mb":    megabytes,
 *       "gb":    gigabytes,
 *     }
 *   }
 */
BSONObj NamedPipeHelper::readFromPipes(const std::vector<std::string>& pipeRelativePaths) {
    std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now();
    double objects = 0.0;         // return stat
    double totalSizeBytes = 0.0;  // return stat

    // Create metadata describing the pipes and a MultiBsonStreamCursor to read them.
    VirtualCollectionOptions vopts;
    for (const std::string& pipeRelativePath : pipeRelativePaths) {
        ExternalDataSourceMetadata meta(
            (std::string{ExternalDataSourceMetadata::kUrlProtocolFile} + pipeRelativePath),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.emplace_back(meta);
    }
    MultiBsonStreamCursor msbc(vopts);

    // Use MultiBsonStreamCursor to read the pipes.
    boost::optional<Record> record = boost::none;
    do {
        record = msbc.next();
        if (record) {
            ++objects;
            totalSizeBytes += record->data.size();
        }
    } while (record);
    std::chrono::system_clock::time_point finishTime = std::chrono::system_clock::now();
    auto duration = finishTime - startTime;

    double sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    double msec = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    double usec = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    double nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    double mbps = (totalSizeBytes / (1024.0 * 1024.0)) / (nsec / (1000.0 * 1000.0 * 1000.0));
    double gbps = mbps / 1024.0;
    return BSON("" << BSON("objects"
                           << objects << "time"
                           << BSON("sec" << sec << "msec" << msec << "usec" << usec << "nsec"
                                         << nsec)
                           << "rate" << BSON("mbps" << mbps << "gbps" << gbps) << "totalSize"
                           << BSON("bytes" << totalSizeBytes << "kb" << (totalSizeBytes / 1024.0)
                                           << "mb" << (totalSizeBytes / (1024.0 * 1024.0)) << "gb"
                                           << (totalSizeBytes / (1024.0 * 1024.0 * 1024.0)))));
}

/**
 * Asynchronously writes 'objects' random BSON objects to named pipe 'pipeRelativePath'. The
 * "string" field of these objects will have stringMinSize <= string.length() <= stringMaxSize.
 */
void NamedPipeHelper::writeToPipeAsync(std::string pipeDir,
                                       std::string pipeRelativePath,
                                       long objects,
                                       long stringMinSize,
                                       long stringMaxSize) {
    // NamedPipeOutput's constructor may throw; exceptions on this calling thread propagate through
    // the shell binding and appear in the JavaScript test log.
    auto pipeWriter = std::make_unique<NamedPipeOutput>(pipeDir, pipeRelativePath);  // producer

    LOGV2_INFO(13212604,
               "Starting pipe writer thread",
               "method"_attr = "NamedPipeHelper::writeToPipeAsync",
               "pipe"_attr = pipeWriter->getAbsolutePath());
    stdx::thread thread(
        writeToPipeAsyncImpl, std::move(pipeWriter), objects, stringMinSize, stringMaxSize);
    thread.detach();
}

/**
 * Synchronously writes 'objects' BSON objects round-robinned from 'bsonObjs' to named pipe
 * 'pipeRelativePath'. Note that the open() call itself will block until a pipe reader attaches to
 * the same pipe. Exceptions on the calling thread propagate through the shell binding and appear
 * in the JS test log.
 */
void NamedPipeHelper::writeToPipeObjects(std::string pipeDir,
                                         std::string pipeRelativePath,
                                         long objects,
                                         std::vector<BSONObj> bsonObjs,
                                         bool persistPipe) {
    NamedPipeOutput pipeWriter(pipeDir, pipeRelativePath, persistPipe);
    writeObjectsToPipe(pipeWriter, objects, bsonObjs);
}

/**
 * Asynchronously writes 'objects' BSON objects round-robinned from 'bsonObjs' to named pipe
 * 'pipeRelativePath'.
 */
void NamedPipeHelper::writeToPipeObjectsAsync(std::string pipeDir,
                                              std::string pipeRelativePath,
                                              long objects,
                                              std::vector<BSONObj> bsonObjs,
                                              bool persistPipe) {
    // NamedPipeOutput's constructor may throw; exceptions on this calling thread propagate through
    // the shell binding and appear in the JavaScript test log.
    auto pipeWriter = std::make_unique<NamedPipeOutput>(pipeDir, pipeRelativePath, persistPipe);
    LOGV2_INFO(13212605,
               "Starting pipe writer thread",
               "method"_attr = "NamedPipeHelper::writeToPipeObjectsAsync",
               "pipe"_attr = pipeWriter->getAbsolutePath());
    stdx::thread thread(
        writeObjectsToPipeAsyncImpl, std::move(pipeWriter), objects, std::move(bsonObjs));
    thread.detach();
}
}  // namespace mongo
