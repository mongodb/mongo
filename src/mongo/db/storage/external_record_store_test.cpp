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

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fmt/format.h>

#include "mongo/db/storage/input_stream.h"
#include "mongo/db/storage/multi_bson_stream_cursor.h"
#include "mongo/db/storage/named_pipe.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
using namespace fmt::literals;

static const std::string pipePath1 = "named_pipe1";
static const std::string pipePath2 = "named_pipe2";
static const std::string nonExistingPath = "non-existing";
static constexpr int kNumPipes = 2;
static const std::string pipePaths[kNumPipes] = {pipePath1, pipePath2};

class PipeWaiter {
public:
    void notify() {
        {
            stdx::unique_lock lk(m);
            pipeCreated = true;
        }
        cv.notify_one();
    }

    void wait() {
        stdx::unique_lock lk(m);
        cv.wait(lk, [&] { return pipeCreated; });
    }

private:
    Mutex m;
    stdx::condition_variable cv;
    bool pipeCreated = false;
};

class ExternalRecordStoreTest : public unittest::Test {
public:
    // Gets a random string of 'count' length consisting of printable ASCII chars (32-126).
    static std::string getRandomString(const int count) {
        std::string buf;
        buf.reserve(count);

        for (int i = 0; i < count; ++i) {
            buf.push_back(static_cast<char>(32 + std::rand() % 95));
        }

        return buf;
    }

    static constexpr int kBufferSize = 1024;
    char _buffer[kBufferSize];  // buffer amply big enough to fit any BSONObj used in this test

    static void createNamedPipe(PipeWaiter* pw,
                                const std::string& pipePath,
                                long numToWrite,
                                const std::vector<BSONObj>& bsonObjs);

    void setUp() override {
        std::srand(std::time(0));
    }
};

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
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath1);
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

    auto inputStream = InputStream<NamedPipeInput>(pipePath1);
    for (int i = 0; i < 100; ++i) {
        int nRead = inputStream.readBytes(count, _buffer);
        ASSERT_EQ(nRead, count) << "Failed to read data up to {} bytes"_format(count);
        ASSERT_EQ(std::memcmp(srcBsonObj.objdata(), _buffer, count), 0)
            << "Read data is not same as the source data";
    }
}

TEST_F(ExternalRecordStoreTest, NamedPipeReadPartialData) {
    auto srcBsonObj = BSON("a" << 1);
    auto count = srcBsonObj.objsize();
    PipeWaiter pw;
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath1);
        pw.notify();
        pipeWriter.open();
        pipeWriter.write(srcBsonObj.objdata(), count);
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    auto inputStream = InputStream<NamedPipeInput>(pipePath1);
    // Requests more data than the pipe contains. Should only get the bytes it does contain.
    int nRead = inputStream.readBytes(kBufferSize, _buffer);
    ASSERT_EQ(nRead, count) << "Expected nRead == {} but got {}"_format(count, nRead);
    ASSERT_EQ(std::memcmp(srcBsonObj.objdata(), _buffer, count), 0)
        << "Read data is not same as the source data";
}

TEST_F(ExternalRecordStoreTest, NamedPipeReadUntilProducerDone) {
    auto srcBsonObj = BSON("a" << 1);
    auto count = srcBsonObj.objsize();
    const auto nSent = std::rand() % 100;
    PipeWaiter pw;
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter(pipePath1);
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

    auto inputStream = InputStream<NamedPipeInput>(pipePath1);
    auto nReceived = 0;
    while (true) {
        int nRead = inputStream.readBytes(count, _buffer);
        if (nRead != count) {
            ASSERT_EQ(nRead, 0) << "Expected nRead == 0 for EOF but got something else {}"_format(
                nRead);
            break;
        }
        ASSERT_EQ(std::memcmp(srcBsonObj.objdata(), _buffer, count), 0)
            << "Read data is not same as the source data";
        ++nReceived;
    }

    ASSERT_EQ(nReceived, nSent) << "Received count {} is different from the sent count {}"_format(
        nReceived, nSent);
}

TEST_F(ExternalRecordStoreTest, NamedPipeOpenNonExisting) {
    ASSERT_THROWS_CODE(
        [] { (void)std::make_unique<InputStream<NamedPipeInput>>(nonExistingPath); }(),
        DBException,
        ErrorCodes::FileNotOpen);
}

// Test reading multiple pipes with a MultiBsonStreamCursor. In this test each pipe contains many
// copies of a pipe-specific BSONObj, and everything in each pipe fits into a single read buffer.
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes1) {
    const int kObjsPerPipe = 50;
    std::vector<BSONObj> bsonObjs[kNumPipes] = {{BSON("a" << 1)},
                                                {BSON("zed"
                                                      << "two")}};

    // Create two pipes. The first has only "a" objects and the second has only "zed" objects.
    stdx::thread pipeThreads[kNumPipes];
    PipeWaiter pw[kNumPipes];
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
            (ExternalDataSourceMetadata::kUrlProtocolFile + pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.push_back(meta);
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
                << "Expected record->id {} but got {}"_format(recIdExpected, recId);
            ASSERT_EQ(record->data.size(), bsonObjs[pipeIdx][0].objsize())
                << "record.data.size() {} != original size {}"_format(
                       record->data.size(), bsonObjs[pipeIdx][0].objsize());
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
        ASSERT_EQ(objsRead[pipeIdx], kObjsPerPipe)
            << "Expected objsRead[{}] == {} but got {}"_format(
                   pipeIdx, kObjsPerPipe, objsRead[pipeIdx]);
    }
}

// Test reading multiple pipes with a MultiBsonStreamCursor that uses large enough pipes to exercise
// the cases of partial objects at the end of a single block read inside MultiStreamBsonCursor,
// which must then be completed by the subsequent call to next(). This test writes and reads back a
// few million bsons to and from each pipe.
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes2) {
    const std::vector<BSONObj> bsonObjs = {
        BSON("One"
             << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        BSON("Twofer"
             << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        BSON("field3"
             << "THREE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"),
        BSON("four hundred forty-four"
             << "444444444444444444444444444444444444444444444444444444"),
        BSON("5"
             << "five five five five five five five five five five five five five five five"),
        BSON("Sixx"
             << "6666666666666666666666666666666666666666666666666666666666666666666666666"),
        BSON("Lucky_Seven"
             << "777777777777777777777777777777777777777777777777777777777777777777"),
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
            (ExternalDataSourceMetadata::kUrlProtocolFile + pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.push_back(meta);
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
                << "Expected record->id {} but got {}"_format(recIdExpected, recId);
            ASSERT_EQ(record->data.size(), bsonObjs[objIdx].objsize())
                << "record.data.size() {} != original size {}"_format(record->data.size(),
                                                                      bsonObjs[objIdx].objsize());
            ASSERT_EQ(std::memcmp(record->data.data(),
                                  bsonObjs[objIdx].objdata(),
                                  bsonObjs[objIdx].objsize()),
                      0)
                << "Read data is not same as the source data";

            ++recIdExpected;
            objIdx = (objIdx + 1) % numObjs;
        }
    } while (record);
    ASSERT_EQ(objsRead, numToWrite)
        << "Expected objsRead == {} but got {}"_format(numToWrite, objsRead);
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
            (ExternalDataSourceMetadata::kUrlProtocolFile + pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.push_back(meta);
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
                << "Expected record->id {} but got {}"_format(recIdExpected, recId);
            ASSERT_EQ(record->data.size(), bsonObjs[0].objsize())
                << "record.data.size() {} != original size {}"_format(record->data.size(),
                                                                      bsonObjs[0].objsize());
            ASSERT_EQ(
                std::memcmp(record->data.data(), bsonObjs[0].objdata(), bsonObjs[0].objsize()), 0)
                << "Read data is not same as the source data";

            ++recIdExpected;
        }
    } while (record);
    ASSERT_EQ(objsRead, numToWrite)
        << "Expected objsRead == {} but got {}"_format(numToWrite, objsRead);
}

// Tests MultiBsonStreamCursor reading a large number of pipes with random-sized BSON objects and
// randomized data. This test creates 100 threads that each write a pipe containing a randomized
// average of 1K BSON objects, each object holding a string value of randomized average 1K in size
// of random printable ASCII characters, plus field name and overhead. Thus it will scan an expected
// ~100+ MB of data (~1+ MB per pipe).
//
// This test uses a fixed random seed so if it fails we can reproduce that easily. C++ does not
// support multiple random number generators, so this reseeds the shared rand() generater with the
// current time at the end as other tests in this file expect non-fixed seed randomosity.
TEST_F(ExternalRecordStoreTest, NamedPipeMultiplePipes4) {
    std::srand(972134657);                         // set a fixed random seed
    constexpr int kNumPipes = 100;                 // shadows the global
    std::string pipePaths[kNumPipes];              // shadows the global
    stdx::thread pipeThreads[kNumPipes];           // pipe producer threads
    PipeWaiter pw[kNumPipes];                      // pipe waiters
    std::vector<BSONObj> pipeBsonObjs[kNumPipes];  // vector of BSON objects for each pipe
    size_t objsWritten = 0;                        // number of objects written to all pipes

    // Create the BSON objects, averaging 1K objects per pipe with average 1K random data in each.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        int numObjs = rand() % 2048;
        objsWritten += numObjs;
        std::string fieldName = "field_{}"_format(pipeIdx);
        for (int objIdx = 0; objIdx < numObjs; ++objIdx) {
            pipeBsonObjs[pipeIdx].push_back(BSON(fieldName << getRandomString(rand() % 2048)));
        }
    }

    // Create the pipes.
    for (int pipeIdx = 0; pipeIdx < kNumPipes; ++pipeIdx) {
        pipePaths[pipeIdx] = "named_pipe{}"_format(pipeIdx);
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
            (ExternalDataSourceMetadata::kUrlProtocolFile + pipePaths[pipeIdx]),
            StorageTypeEnum::pipe,
            FileTypeEnum::bson);
        vopts.dataSources.push_back(meta);
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
                << "Expected record->id {} but got {}"_format(recIdExpected, recId);
            ASSERT_EQ(record->data.size(), pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objsize())
                << "record.data.size() {} != original size {}"_format(
                       record->data.size(), pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objsize());
            ASSERT_EQ(std::memcmp(record->data.data(),
                                  pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objdata(),
                                  pipeBsonObjs[pipeIdx][pipeObjsRead - 1].objsize()),
                      0)
                << "Read data is not same as the source data";

            ++recIdExpected;
        }
    } while (record);
    ASSERT_EQ(objsRead, objsWritten)
        << "Expected objsRead == {} but got {}"_format(objsWritten, objsRead);

    // Reseed random number generator with current time to make it "really" random again.
    setUp();
}
}  // namespace mongo
