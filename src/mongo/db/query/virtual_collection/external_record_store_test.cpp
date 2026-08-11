// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/query/virtual_collection/input_stream.h"
#include "mongo/db/query/virtual_collection/multi_bson_stream_cursor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/virtual_collection_options.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/named_pipe/named_pipe.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"

namespace mongo {
namespace {

static const std::string nonExistingPath = "non-existing";
static constexpr int kNumPipes = 2;

class PipeWaiter {
public:
    void notify() {
        {
            std::unique_lock lk(m);
            pipeCreated = true;
        }
        cv.notify_one();
    }

    void wait() {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return pipeCreated; });
    }

private:
    std::mutex m;
    stdx::condition_variable cv;
    bool pipeCreated = false;
};

class ExternalRecordStoreTest : public unittest::Test {
public:
    // Gets a random string of 'count' length consisting of printable ASCII chars (32-126).
    std::string getRandomString(const int count) {
        std::string buf;
        buf.reserve(count);

        for (int i = 0; i < count; ++i) {
            buf.push_back(static_cast<char>(32 + _random.nextInt32(95)));
        }

        return buf;
    }

    void setRandomSeed(int64_t seed) {
        _random = PseudoRandom{seed};
    }

    static constexpr int kBufferSize = 1024;
    char _buffer[kBufferSize];  // buffer amply big enough to fit any BSONObj used in this test
    PseudoRandom _random{SecureRandom{}.nextInt64()};

    static void createNamedPipe(PipeWaiter* pw,
                                const std::string& pipePath,
                                long numToWrite,
                                const std::vector<BSONObj>& bsonObjs);

    static std::string createPipeFilename(const std::string& name) {
        // The NamedPipe API in MongoDB allows the caller to specify a directory, but only for POSIX
        // systems, and not on Windows. To ensure that we avoid naming conflicts, we just choose a
        // unique name for the pipe.
        return uniqueTestPrefix + name;
    }

private:
    static const std::string uniqueTestPrefix;
};

// Introduce randomness into the pipe name, to avoid conflicts.
const std::string ExternalRecordStoreTest::uniqueTestPrefix =
    boost::filesystem::unique_path("ERSTest-%%%%-%%%%-%%%%-%%%%-").string();

// Creates a named pipe of BSON objects.
//   pipeWaiter - synchronization for pipe creation
//   pipePath - file path for the named pipe
//   numToWrite - number of bsons to write to the pipe
//   bsonObjs - vector of bsons to write round-robin to the pipe
void ExternalRecordStoreTest::createNamedPipe(PipeWaiter* pw,
                                              const std::string& pipePath,
                                              long numToWrite,
                                              const std::vector<BSONObj>& bsonObjs) {
    NamedPipeOutput pipeWriter(pipePath);
    // We need to notify before opening, since the writer expects the reader to open first.
    pw->notify();
    pipeWriter.open();

    const int numObjs = bsonObjs.size();
    int objIdx = 0;
    for (int num = 0; num < numToWrite; ++num) {
        pipeWriter.write(bsonObjs[objIdx].objdata(), bsonObjs[objIdx].objsize());
        objIdx = (objIdx + 1) % numObjs;
    }

    pipeWriter.close();
}

TEST_F(ExternalRecordStoreTest, NamedPipeBasicRead) {
    auto srcBsonObj = BSON("a" << 1);
    auto count = srcBsonObj.objsize();
    PipeWaiter pw;
    const auto pipePath = createPipeFilename("NamedPipeBasicReadPipe");
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath);
        pw.notify();
        pipeWriter.open();

        for (int i = 0; i < 100; ++i) {
            pipeWriter.write(srcBsonObj.objdata(), count);
        }

        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    auto inputStream = InputStream<NamedPipeInput>(pipePath);
    for (int i = 0; i < 100; ++i) {
        int nRead = inputStream.readBytes(count, _buffer);
        ASSERT_EQ(nRead, count) << fmt::format("Failed to read data up to {} bytes", count);
        ASSERT_EQ(std::memcmp(srcBsonObj.objdata(), _buffer, count), 0)
            << "Read data is not same as the source data";
    }
}

TEST_F(ExternalRecordStoreTest, NamedPipeReadPartialData) {
    auto srcBsonObj = BSON("a" << 1);
    auto count = srcBsonObj.objsize();
    PipeWaiter pw;
    const auto pipePath = createPipeFilename("NamedPipeReadPartialDataPipe");
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath);
        pw.notify();
        pipeWriter.open();
        pipeWriter.write(srcBsonObj.objdata(), count);
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    auto inputStream = InputStream<NamedPipeInput>(pipePath);
    // Requests more data than the pipe contains. Should only get the bytes it does contain.
    int nRead = inputStream.readBytes(kBufferSize, _buffer);
    ASSERT_EQ(nRead, count) << fmt::format("Expected nRead == {} but got {}", count, nRead);
    ASSERT_EQ(std::memcmp(srcBsonObj.objdata(), _buffer, count), 0)
        << "Read data is not same as the source data";
}

TEST_F(ExternalRecordStoreTest, NamedPipeReadUntilProducerDone) {
    auto srcBsonObj = BSON("a" << 1);
    auto count = srcBsonObj.objsize();
    const auto nSent = _random.nextInt32(100);
    PipeWaiter pw;
    const auto pipePath = createPipeFilename("NamedPipeReadUntilProducerDonePipe");
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath);
        pw.notify();
        pipeWriter.open();

        for (int i = 0; i < nSent; ++i) {
            pipeWriter.write(srcBsonObj.objdata(), count);
        }

        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    auto inputStream = InputStream<NamedPipeInput>(pipePath);
    auto nReceived = 0;
    while (true) {
        int nRead = inputStream.readBytes(count, _buffer);
        if (nRead != count) {
            ASSERT_EQ(nRead, 0) << fmt::format(
                "Expected nRead == 0 for EOF but got something else {}", nRead);
            break;
        }
        ASSERT_EQ(std::memcmp(srcBsonObj.objdata(), _buffer, count), 0)
            << "Read data is not same as the source data";
        ++nReceived;
    }

    ASSERT_EQ(nReceived, nSent) << fmt::format(
        "Received count {} is different from the sent count {}", nReceived, nSent);
}

TEST_F(ExternalRecordStoreTest, NamedPipeOpenNonExisting) {
    ASSERT_THROWS_CODE(
        [] {
            (void)std::make_unique<InputStream<NamedPipeInput>>(nonExistingPath);
        }(),
        DBException,
        ErrorCodes::FileNotOpen);
}

// Test reading multiple pipes with a MultiBsonStreamCursor. In this test each pipe contains many
// copies of a pipe-specific BSONObj, and everything in each pipe fits into a single read buffer.
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes1) {
    const int kObjsPerPipe = 50;
    std::vector<BSONObj> bsonObjs[kNumPipes] = {{BSON("a" << 1)}, {BSON("zed" << "two")}};

    // Create two pipes. The first has only "a" objects and the second has only "zed" objects.
    stdx::thread pipeThreads[kNumPipes];
    PipeWaiter pw[kNumPipes];
    const std::string pipePaths[] = {createPipeFilename("NamedPipeMultiplePipes1Pipe1"),
                                     createPipeFilename("NamedPipeMultiplePipes1Pipe2")};
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pipeThreads[pipeIdx] = stdx::thread(
            createNamedPipe, &pw[pipeIdx], pipePaths[pipeIdx], kObjsPerPipe, bsonObjs[pipeIdx]);
    }
    ON_BLOCK_EXIT([&] {
        for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
            pipeThreads[pipeIdx].join();
        }
    });

    // Gives some time to the producers so they can initialize the named pipes.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pw[pipeIdx].wait();
    }

    // Create metadata describing the pipes and a MultiBsonStreamCursor to read them.
    VirtualCollectionOptions vopts;
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        ExternalDataSourceMetadata meta(
            fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.emplace_back(meta);
    }
    MultiBsonStreamCursor msbc = MultiBsonStreamCursor(vopts);

    // Use MultiBsonStreamCursor to read the pipes.
    int objsRead[kNumPipes] = {0, 0};
    boost::optional<Record> record = boost::none;
    long recIdExpected = 0;
    long pipeIdx = 0;
    do {
        record = msbc.next();
        if (record) {
            ++objsRead[pipeIdx];
            long recId = record->id.getLong();
            ASSERT_EQ(recIdExpected, recId)
                << fmt::format("Expected record->id {} but got {}", recIdExpected, recId);
            ASSERT_EQ(record->data.size(), bsonObjs[pipeIdx][0].objsize())
                << fmt::format("record->data.size() {} != original size {}",
                               record->data.size(),
                               bsonObjs[pipeIdx][0].objsize());
            ASSERT_EQ(std::memcmp(record->data.data(),
                                  bsonObjs[pipeIdx][0].objdata(),
                                  bsonObjs[pipeIdx][0].objsize()),
                      0)
                << "Read data is not same as the source data";

            ++recIdExpected;
            pipeIdx = recIdExpected / kObjsPerPipe;
        }
    } while (record);
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        ASSERT_EQ(objsRead[pipeIdx], kObjsPerPipe) << fmt::format(
            "Expected objsRead[{}] == {} but got {}", pipeIdx, kObjsPerPipe, objsRead[pipeIdx]);
    }
}

// Test reading multiple pipes with a MultiBsonStreamCursor that uses large enough pipes to exercise
// the cases of partial objects at the end of a single block read inside MultiStreamBsonCursor,
// which must then be completed by the subsequent call to next(). This test writes and reads back a
// few million bsons to and from each pipe.
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes2) {
    const std::vector<BSONObj> bsonObjs = {
        BSON("One" << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        BSON("Twofer" << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        BSON("field3" << "THREE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"),
        BSON("four hundred forty-four" << "444444444444444444444444444444444444444444444444444444"),
        BSON("5" << "five five five five five five five five five five five five five five five"),
        BSON("Sixx" << "6666666666666666666666666666666666666666666666666666666666666666666666666"),
        BSON("Lucky_Seven" << "777777777777777777777777777777777777777777777777777777777777777777"),
    };
    const int numObjs = bsonObjs.size();

    int bsonSizeTotal = 0;
    for (int bsonIdx = 0; bsonIdx < numObjs; ++bsonIdx) {
        bsonSizeTotal += bsonObjs[bsonIdx].objsize();
    }

    const int mb32 = 32 * 1024 * 1024;
    const int groupsIn32Mb = mb32 / bsonSizeTotal + 1;

    // Create pipes with different numbers of varying-sized bsons. For stress testing, these are
    // substantially larger than 32 MB (the largest size MultiBsonStreamCursor's buffer can reach),
    // so they will cause several wraps. The largish size of the objects makes it highly likely that
    // some reads will leave a partial object that must be completed on a later next() call.
    stdx::thread pipeThreads[kNumPipes];
    PipeWaiter pw[kNumPipes];
    long numToWrites[] = {(3 * groupsIn32Mb * numObjs), (5 * groupsIn32Mb * numObjs)};
    long numToWrite = 0;

    const std::string pipePaths[] = {createPipeFilename("NamedPipeMultiplePipes2Pipe1"),
                                     createPipeFilename("NamedPipeMultiplePipes2Pipe2")};
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pipeThreads[pipeIdx] = stdx::thread(
            createNamedPipe, &pw[pipeIdx], pipePaths[pipeIdx], numToWrites[pipeIdx], bsonObjs);
        numToWrite += numToWrites[pipeIdx];
    }
    ON_BLOCK_EXIT([&] {
        for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
            pipeThreads[pipeIdx].join();
        }
    });

    // Gives some time to the producers so they can initialize the named pipes.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pw[pipeIdx].wait();
    }

    // Create metadata describing the pipes and a MultiBsonStreamCursor to read them.
    VirtualCollectionOptions vopts;
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        ExternalDataSourceMetadata meta(
            fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.emplace_back(meta);
    }
    MultiBsonStreamCursor msbc(vopts);

    // Use MultiBsonStreamCursor to read the pipes.
    long objsRead = 0;
    boost::optional<Record> record = boost::none;
    long recIdExpected = 0;
    int objIdx = 0;
    do {
        record = msbc.next();
        if (record) {
            ++objsRead;
            long recId = record->id.getLong();
            ASSERT_EQ(recIdExpected, recId)
                << fmt::format("Expected record->id {} but got {}", recIdExpected, recId);
            ASSERT_EQ(record->data.size(), bsonObjs[objIdx].objsize())
                << fmt::format("recId {}: record->data.size() {} != original size {}",
                               recId,
                               record->data.size(),
                               bsonObjs[objIdx].objsize());
            ASSERT_EQ(std::memcmp(record->data.data(),
                                  bsonObjs[objIdx].objdata(),
                                  bsonObjs[objIdx].objsize()),
                      0)
                << fmt::format("recId {}: Read data is not same as the source data", recId);

            ++recIdExpected;
            objIdx = (objIdx + 1) % numObjs;
        }
    } while (record);
    ASSERT_EQ(objsRead, numToWrite)
        << fmt::format("Expected objsRead == {} but got {}", numToWrite, objsRead);
}

// Test reading multiple pipes with a MultiBsonStreamCursor with large BSON objects, much larger
// than the starting buffer size of 8K. This exercises the dynamic buffer expansion.
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes3) {

    std::vector<char> vec1mb;

    for (int i = 0; i < 1024 * 1024; ++i) {
        vec1mb.push_back('Q');
    }
    std::string str1mb(vec1mb.begin(), vec1mb.end());

    // BSON object with 15 1MB string fields
    const std::vector<BSONObj> bsonObjs = {
        BSON("longString00" << str1mb << "longString01" << str1mb << "longString02" << str1mb
                            << "longString03" << str1mb << "longString04" << str1mb
                            << "longString05" << str1mb << "longString06" << str1mb
                            << "longString07" << str1mb << "longString08" << str1mb
                            << "longString09" << str1mb << "longString10" << str1mb
                            << "longString11" << str1mb << "longString12" << str1mb
                            << "longString13" << str1mb << "longString14" << str1mb),
    };

    // Create pipes with large bsons.
    stdx::thread pipeThreads[kNumPipes];
    PipeWaiter pw[kNumPipes];
    long numToWrites[] = {19, 17};
    long numToWrite = 0;

    const std::string pipePaths[] = {createPipeFilename("NamedPipeMultiplePipes3Pipe1"),
                                     createPipeFilename("NamedPipeMultiplePipes3Pipe2")};
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pipeThreads[pipeIdx] = stdx::thread(
            createNamedPipe, &pw[pipeIdx], pipePaths[pipeIdx], numToWrites[pipeIdx], bsonObjs);
        numToWrite += numToWrites[pipeIdx];
    }
    ON_BLOCK_EXIT([&] {
        for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
            pipeThreads[pipeIdx].join();
        }
    });

    // Gives some time to the producers so they can initialize the named pipes.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pw[pipeIdx].wait();
    }

    // Create metadata describing the pipes and a MultiBsonStreamCursor to read them.
    VirtualCollectionOptions vopts;
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        ExternalDataSourceMetadata meta(
            fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.emplace_back(meta);
    }
    MultiBsonStreamCursor msbc(vopts);

    // Use MultiBsonStreamCursor to read the pipes.
    long objsRead = 0;
    boost::optional<Record> record = boost::none;
    long recIdExpected = 0;
    do {
        record = msbc.next();
        if (record) {
            ++objsRead;
            long recId = record->id.getLong();
            ASSERT_EQ(recIdExpected, recId)
                << fmt::format("Expected record->id {} but got {}", recIdExpected, recId);
            ASSERT_EQ(record->data.size(), bsonObjs[0].objsize())
                << fmt::format("record->data.size() {} != original size {}",
                               record->data.size(),
                               bsonObjs[0].objsize());
            ASSERT_EQ(
                std::memcmp(record->data.data(), bsonObjs[0].objdata(), bsonObjs[0].objsize()), 0)
                << "Read data is not same as the source data";

            ++recIdExpected;
        }
    } while (record);
    ASSERT_EQ(objsRead, numToWrite)
        << fmt::format("Expected objsRead == {} but got {}", numToWrite, objsRead);
}

// Tests MultiBsonStreamCursor reading a large number of pipes with random-sized BSON objects and
// randomized data. This test creates 20 threads that each write a pipe containing a randomized
// average of 1K BSON objects, each object holding a string value of randomized average 1K in size
// of random printable ASCII characters, plus field name and overhead. Thus it will scan an expected
// ~20+ MB of data (~1+ MB per pipe).
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes4) {
    setRandomSeed(972134657);      // set a fixed random seed
    constexpr int kNumPipes = 20;  // shadows the global
    std::string pipePaths[kNumPipes];
    stdx::thread pipeThreads[kNumPipes];           // pipe producer threads
    PipeWaiter pw[kNumPipes];                      // pipe waiters
    std::vector<BSONObj> pipeBsonObjs[kNumPipes];  // vector of BSON objects for each pipe
    size_t objsWritten = 0;                        // number of objects written to all pipes

    // Create the BSON objects, averaging 1K objects per pipe with average 1K random data in each.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        int numObjs = _random.nextInt32(2048);
        objsWritten += numObjs;
        std::string fieldName = fmt::format("field_{}", pipeIdx);
        for (int objIdx = 0; objIdx < numObjs; ++objIdx) {
            pipeBsonObjs[pipeIdx].emplace_back(
                BSON(fieldName << getRandomString(_random.nextInt32(2048))));
        }
    }

    // Create the pipes.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pipePaths[pipeIdx] =
            createPipeFilename(fmt::format("NamedPipeMultiplePipes4Pipe1{}", pipeIdx));
        pipeThreads[pipeIdx] = stdx::thread(createNamedPipe,
                                            &pw[pipeIdx],
                                            pipePaths[pipeIdx],
                                            pipeBsonObjs[pipeIdx].size(),
                                            pipeBsonObjs[pipeIdx]);
    }
    ON_BLOCK_EXIT([&] {
        for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
            pipeThreads[pipeIdx].join();
        }
    });

    // Gives some time to the producers so they can initialize the named pipes.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pw[pipeIdx].wait();
    }

    // Create metadata describing the pipes and a MultiBsonStreamCursor to read them.
    VirtualCollectionOptions vopts;
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        ExternalDataSourceMetadata meta(
            fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.emplace_back(meta);
    }
    MultiBsonStreamCursor msbc(vopts);

    // Use MultiBsonStreamCursor to read the pipes.
    size_t objsRead = 0;      // number of objects read from all pipes
    int pipeIdx = 0;          // current pipe index
    size_t pipeObjsRead = 0;  // number of objects read from current pipe
    boost::optional<Record> record = boost::none;
    long recIdExpected = 0;
    do {
        record = msbc.next();
        if (record) {
            ++objsRead;
            ++pipeObjsRead;
            while (pipeObjsRead > pipeBsonObjs[pipeIdx].size()) {  // loop in case 0 objs in a pipe
                ++pipeIdx;
                pipeObjsRead = 1;
            }

            long recId = record->id.getLong();
            ASSERT_EQ(recIdExpected, recId)
                << fmt::format("Expected record->id {} but got {}", recIdExpected, recId);
            ASSERT_EQ(record->data.size(), pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objsize())
                << fmt::format("record->data.size() {} != original size {}",
                               record->data.size(),
                               pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objsize());
            ASSERT_EQ(std::memcmp(record->data.data(),
                                  pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objdata(),
                                  pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objsize()),
                      0)
                << "Read data is not same as the source data";

            ++recIdExpected;
        }
    } while (record);
    ASSERT_EQ(objsRead, objsWritten)
        << fmt::format("Expected objsRead == {} but got {}", objsWritten, objsRead);
}

// A crafted document with a valid top-level size can embed a negative int32 as the size of a nested
// array element. Make sure that we reject this when validating the BSON.
TEST_F(ExternalRecordStoreTest, RejectsNegativeEmbeddedArraySize) {
    // Minimal malformed BSON: total_size=12, array element "a" with embedded size=-4 (0xFFFFFFFC).
    // clang-format off
    static constexpr char kMalformedBson[] = {
        '\x0C', '\x00', '\x00', '\x00',  // total_size = 12
        '\x04',                          // type: array
        '\x61', '\x00',                  // key: "a\0"
        '\xFC', '\xFF', '\xFF', '\xFF',  // embedded array size = -4
        '\x00',                          // top-level document terminator
    };
    // clang-format on
    static_assert(sizeof(kMalformedBson) == 12);

    PipeWaiter pw;
    const auto pipePath = createPipeFilename("RejectsNegativeEmbeddedArraySizePipe");
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath);
        pw.notify();
        pipeWriter.open();
        pipeWriter.write(kMalformedBson, sizeof(kMalformedBson));
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });
    pw.wait();

    VirtualCollectionOptions vopts;
    ExternalDataSourceMetadata meta(
        fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePath),
        StorageTypeEnum::pipe,
        FileTypeEnum::bson);
    vopts.dataSources.emplace_back(meta);
    MultiBsonStreamCursor msbc(vopts);

    ASSERT_THROWS_CODE(msbc.next(), DBException, 12849400);
}

// A negative top-level size is caught by the explicit size check, before validateBSON() sees it.
TEST_F(ExternalRecordStoreTest, RejectsNegativeTopLevelSize) {
    // clang-format off
    static constexpr char kNegativeSizeBson[] = {
        '\xFC', '\xFF', '\xFF', '\xFF',  // total_size = -4
        '\x00',                          // one byte of body
    };
    // clang-format on

    PipeWaiter pw;
    const auto pipePath = createPipeFilename("RejectsNegativeTopLevelSizePipe");
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath);
        pw.notify();
        pipeWriter.open();
        pipeWriter.write(kNegativeSizeBson, sizeof(kNegativeSizeBson));
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });
    pw.wait();

    VirtualCollectionOptions vopts;
    ExternalDataSourceMetadata meta(
        fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePath),
        StorageTypeEnum::pipe,
        FileTypeEnum::bson);
    vopts.dataSources.emplace_back(meta);
    MultiBsonStreamCursor msbc(vopts);

    ASSERT_THROWS_CODE(msbc.next(), DBException, 13251201);
}

// Appends 'value' to 'doc' as a little-endian int32, which is what BSON requires. Appending the
// host representation instead would byte-swap it on big-endian platforms such as s390x, where the
// cursor reads a size the test never intended (BF-45534).
void appendLittleEndianInt32(std::string& doc, int32_t value) {
    char buf[sizeof(int32_t)];
    DataView(buf).write<LittleEndian<int32_t>>(value);
    doc.append(buf, sizeof(buf));
}

// Builds a well-formed BSON document of exactly 'totalSize' bytes holding one string field "a".
// Laid out by hand because BSONObjBuilder cannot produce sizes above BSONObjMaxUserSize:
//   int32 totalSize | 0x02 (string) | "a\0" | int32 strLen | strLen bytes | 0x00 (doc terminator)
std::string makeRawStringBson(int32_t totalSize) {
    constexpr int32_t kOverhead = 12;
    invariant(totalSize > kOverhead);
    const int32_t strLen = totalSize - kOverhead;  // includes the string's own NUL terminator

    std::string doc;
    doc.reserve(totalSize);
    appendLittleEndianInt32(doc, totalSize);
    doc.push_back('\x02');
    doc.append("a\0", 2);
    appendLittleEndianInt32(doc, strLen);
    doc.append(strLen - 1, 'x');
    doc.push_back('\0');  // string terminator
    doc.push_back('\0');  // document terminator
    invariant(static_cast<int32_t>(doc.size()) == totalSize);

    return doc;
}

// An oversized document must be rejected even when it fits in the cursor's buffer. Reaching that
// case takes a specific sequence, since expandBuffer()'s check only fires when the buffer grows:
//   1. A document at the limit grows the buffer to its maximum, making the block read size
//      BSONObjMaxUserSize.
//   2. A small document is consumed off the front of the next block read, leaving the oversized
//      document near offset 0 with most of it already buffered.
//   3. Its remainder fits in the buffer's free tail, so it is read without expanding again.
// The overshoot must stay small enough for step 3 to hold; a larger one would force an expansion
// and be caught by expandBuffer() instead.
TEST_F(ExternalRecordStoreTest, RejectsDocumentLargerThanMaxUserSize) {
    static constexpr int32_t kOvershoot = 4 * 1024 * 1024;
    static_assert(kOvershoot > 0 && kOvershoot < BSONObjMaxUserSize);

    const std::string maxSizeDoc = makeRawStringBson(BSONObjMaxUserSize);
    const auto smallDoc = BSON("small" << 1);
    const std::string oversizedDoc = makeRawStringBson(BSONObjMaxUserSize + kOvershoot);

    // The cursor rejects the document partway through it and closes the read end, leaving the
    // blocked producer with a broken pipe.
    static constexpr int kExpectedWriteErrorCode =
#ifdef _WIN32
        7239301;
#else
        7239300;
#endif
    AtomicWord<int> producerWriteErrorCode{0};

    PipeWaiter pw;
    const auto pipePath = createPipeFilename("RejectsDocumentLargerThanMaxUserSizePipe");
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath);
        pw.notify();
        pipeWriter.open();
        try {
            pipeWriter.write(maxSizeDoc.data(), maxSizeDoc.size());
            pipeWriter.write(smallDoc.objdata(), smallDoc.objsize());
            pipeWriter.write(oversizedDoc.data(), oversizedDoc.size());
        } catch (const DBException& ex) {
            // Recorded rather than asserted here: an assertion escaping this thread would terminate
            // the test process. Verified on the main thread after the join below.
            producerWriteErrorCode.store(ex.code());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] {
        if (producer.joinable()) {
            producer.join();
        }
    });
    pw.wait();

    VirtualCollectionOptions vopts;
    ExternalDataSourceMetadata meta(
        fmt::format("{}{}", ExternalDataSourceMetadata::kUrlProtocolFile, pipePath),
        StorageTypeEnum::pipe,
        FileTypeEnum::bson);
    vopts.dataSources.emplace_back(meta);

    {
        MultiBsonStreamCursor msbc(vopts);

        // The first document is exactly at the limit, so it is accepted.
        auto record = msbc.next();
        ASSERT(record) << "Expected to read the BSONObjMaxUserSize document";
        ASSERT_EQ(record->data.size(), BSONObjMaxUserSize);

        record = msbc.next();
        ASSERT(record) << "Expected to read the small document";
        ASSERT_EQ(record->data.size(), smallDoc.objsize());

        // The oversized document fits in the buffer but violates the max user size invariant. It is
        // rejected by the size check.
        ASSERT_THROWS_CODE(msbc.next(), DBException, 13251201);
    }  // Closes the read end of the pipe, unblocking the producer with a broken pipe.

    producer.join();
    ASSERT_EQ(producerWriteErrorCode.load(), kExpectedWriteErrorCode)
        << "Expected the producer to fail writing the remainder of the oversized document";
}

}  // namespace
}  // namespace mongo
