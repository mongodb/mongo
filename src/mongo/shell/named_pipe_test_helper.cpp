/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/shell/named_pipe_test_helper.h"

#include <boost/optional.hpp>
#include <exception>
#include <string>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/virtual_collection_options.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/storage/multi_bson_stream_cursor.h"
#include "mongo/db/storage/named_pipe.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

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
    stdx::chrono::system_clock::time_point startTime = stdx::chrono::system_clock::now();
    double objects = 0.0;         // return stat
    double totalSizeBytes = 0.0;  // return stat

    // Create metadata describing the pipes and a MultiBsonStreamCursor to read them.
    VirtualCollectionOptions vopts;
    for (const std::string& pipeRelativePath : pipeRelativePaths) {
        ExternalDataSourceMetadata meta(
            (ExternalDataSourceMetadata::kUrlProtocolFile + pipeRelativePath),
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
    stdx::chrono::system_clock::time_point finishTime = stdx::chrono::system_clock::now();
    auto duration = finishTime - startTime;

    double sec = stdx::chrono::duration_cast<stdx::chrono::seconds>(duration).count();
    double msec = stdx::chrono::duration_cast<stdx::chrono::milliseconds>(duration).count();
    double usec = stdx::chrono::duration_cast<stdx::chrono::microseconds>(duration).count();
    double nsec = stdx::chrono::duration_cast<stdx::chrono::nanoseconds>(duration).count();
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
 * Synchronously writes 'objects' random BSON objects to named pipe 'pipeRelativePath'. The "string"
 * field of these objects will have stringMinSize <= string.length() <= stringMaxSize. Note that
 * the open() call itself will block until a pipe reader attaches to the same pipe. Absorbs
 * exceptions because this is called by an async detached thread, so escaping exceptions will cause
 * fuzzer tests to fail as its try blocks are only around the main thread.
 */
void NamedPipeHelper::writeToPipe(std::string pipeDir,
                                  std::string pipeRelativePath,
                                  long objects,
                                  long stringMinSize,
                                  long stringMaxSize) noexcept {
    const std::string method = "NamedPipeHelper::writeToPipe";

    try {
        NamedPipeOutput pipeWriter(pipeDir, pipeRelativePath);  // producer

        pipeWriter.open();
        for (size_t length : randomLengths(objects, stringMinSize, stringMaxSize)) {
            auto bsonObj = BSONObjBuilder{}
                               .append("length", static_cast<int>(length))
                               .append("string", std::string(length, 'a'))
                               .obj();
            pipeWriter.write(bsonObj.objdata(), bsonObj.objsize());
        }
        pipeWriter.close();
    } catch (const DBException& ex) {
        LOGV2_ERROR(
            7001104, "Caught DBException", "method"_attr = method, "error"_attr = ex.toString());
    } catch (const std::exception& ex) {
        LOGV2_ERROR(
            7001105, "Caught STL exception", "method"_attr = method, "error"_attr = ex.what());
    } catch (...) {
        LOGV2_ERROR(7001106, "Caught unknown exception", "method"_attr = method);
    }
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
    stdx::thread thread(writeToPipe,
                        std::move(pipeDir),
                        std::move(pipeRelativePath),
                        objects,
                        stringMinSize,
                        stringMaxSize);
    thread.detach();
}

/**
 * Synchronously writes 'objects' BSON objects round-robinned from 'bsonObjs' to named pipe
 * 'pipeRelativePath'. Note that the open() call itself will block until a pipe reader attaches to
 * the same pipe. Absorbs exceptions because this is called by an async detached thread, so escaping
 * exceptions will cause fuzzer tests to fail as its try blocks are only around the main thread.
 */
void NamedPipeHelper::writeToPipeObjects(std::string pipeDir,
                                         std::string pipeRelativePath,
                                         long objects,
                                         std::vector<BSONObj> bsonObjs) noexcept {
    const std::string method = "NamedPipeHelper::writeToPipeObjects";

    try {
        const int kNumBsonObjs = bsonObjs.size();
        NamedPipeOutput pipeWriter(pipeDir, pipeRelativePath);  // producer

        pipeWriter.open();
        for (long i = 0; i < objects; ++i) {
            BSONObj bsonObj{bsonObjs[i % kNumBsonObjs]};
            pipeWriter.write(bsonObj.objdata(), bsonObj.objsize());
        }
        pipeWriter.close();
    } catch (const DBException& ex) {
        LOGV2_ERROR(
            7001107, "Caught DBException", "method"_attr = method, "error"_attr = ex.toString());
    } catch (const std::exception& ex) {
        LOGV2_ERROR(
            7001108, "Caught STL exception", "method"_attr = method, "error"_attr = ex.what());
    } catch (...) {
        LOGV2_ERROR(7001109, "Caught unknown exception", "method"_attr = method);
    }
}

/**
 * Asynchronously writes 'objects' BSON objects round-robinned from 'bsonObjs' to named pipe
 * 'pipeRelativePath'.
 */
void NamedPipeHelper::writeToPipeObjectsAsync(std::string pipeDir,
                                              std::string pipeRelativePath,
                                              long objects,
                                              std::vector<BSONObj> bsonObjs) {
    stdx::thread thread(writeToPipeObjects,
                        std::move(pipeDir),
                        std::move(pipeRelativePath),
                        objects,
                        std::move(bsonObjs));
    thread.detach();
}
}  // namespace mongo
