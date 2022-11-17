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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/virtual_collection_options.h"
#include "mongo/db/storage/multi_bson_stream_cursor.h"
#include "mongo/db/storage/named_pipe.h"
#include "mongo/stdx/chrono.h"

namespace mongo {
/**
 * Gets a string of 'length' 'a' chars as efficiently as possible.
 */
std::string NamedPipeHelper::getString(int length) {
    return std::string(length, 'a');
}

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
void NamedPipeHelper::writeToPipe(const std::string& pipeRelativePath,
                                  long objects,
                                  long stringMinSize,
                                  long stringMaxSize) noexcept {
    const std::string method = "NamedPipeHelper::writeToPipe";

    try {
        NamedPipeOutput pipeWriter(pipeRelativePath);  // producer

        pipeWriter.open();
        for (long obj = 0; obj < objects; ++obj) {
            int length = std::rand() % (1 + stringMaxSize - stringMinSize) + stringMinSize;
            BSONObj bsonObj{BSON("length" << length << "string" << getString(length))};
            pipeWriter.write(bsonObj.objdata(), bsonObj.objsize());
        }
        pipeWriter.close();
    } catch (const DBException& exc) {
        std::cout << method << " caught DBException exception: " << exc.toString() << std::endl;
    } catch (const std::exception& exc) {
        std::cout << method << " caught STL exception: " << exc.what() << std::endl;
    } catch (...) {
        std::cout << method << " caught unknown exception" << std::endl;
    }
}

/**
 * Asynchronously writes 'objects' random BSON objects to named pipe 'pipeRelativePath'. The
 * "string" field of these objects will have stringMinSize <= string.length() <= stringMaxSize.
 */
void NamedPipeHelper::writeToPipeAsync(const std::string& pipeRelativePath,
                                       long objects,
                                       long stringMinSize,
                                       long stringMaxSize) {
    stdx::thread thread(writeToPipe, pipeRelativePath, objects, stringMinSize, stringMaxSize);
    thread.detach();
}

/**
 * Synchronously writes 'objects' BSON objects round-robinned from 'bsonObjs' to named pipe
 * 'pipeRelativePath'. Note that the open() call itself will block until a pipe reader attaches to
 * the same pipe. Absorbs exceptions because this is called by an async detached thread, so escaping
 * exceptions will cause fuzzer tests to fail as its try blocks are only around the main thread.
 */
void NamedPipeHelper::writeToPipeObjects(const std::string& pipeRelativePath,
                                         long objects,
                                         const std::vector<BSONObj>& bsonObjs) noexcept {
    const std::string method = "NamedPipeHelper::writeToPipeObjects";

    try {
        const int kNumBsonObjs = bsonObjs.size();
        NamedPipeOutput pipeWriter(pipeRelativePath);  // producer

        pipeWriter.open();
        for (long obj = 0; obj < objects; ++obj) {
            BSONObj bsonObj{bsonObjs[obj % kNumBsonObjs]};
            pipeWriter.write(bsonObj.objdata(), bsonObj.objsize());
        }
        pipeWriter.close();
    } catch (const DBException& exc) {
        std::cout << method << " caught DBException exception: " << exc.toString() << std::endl;
    } catch (const std::exception& exc) {
        std::cout << method << " caught STL exception: " << exc.what() << std::endl;
    } catch (...) {
        std::cout << method << " caught unknown exception" << std::endl;
    }
}

/**
 * Asynchronously writes 'objects' BSON objects round-robinned from 'bsonObjs' to named pipe
 * 'pipeRelativePath'.
 */
void NamedPipeHelper::writeToPipeObjectsAsync(const std::string& pipeRelativePath,
                                              long objects,
                                              const std::vector<BSONObj>& bsonObjs) {
    stdx::thread thread(
        writeToPipeObjects, std::move(pipeRelativePath), objects, std::move(bsonObjs));
    thread.detach();
}
}  // namespace mongo
