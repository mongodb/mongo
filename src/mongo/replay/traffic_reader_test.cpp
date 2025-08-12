/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/data_builder.h"
#include "mongo/bson/json.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/replay/traffic_recording_iterator.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/uuid.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace mongo {

bool equalSequences(auto first1, auto last1, auto first2, auto last2) {
    // std ranges is currently a banned header, and the recording iterators
    // make use of sentinel end values. Non-ranges algorithms (e.g., std::equal)
    // do not allow this. Thus, manually compare two ranges are equal.
    while (first1 != last1 && first2 != last2) {
        if (*first1 != *first2) {
            return false;
        }
        ++first1;
        ++first2;
    }
    // Confirm that neither sequence has remaining items.
    return first1 == last1 && first2 == last2;
}

class TrafficReaderTest : public unittest::Test {
public:
    void setUp() override {
        dirname = std::string("traffic-reader-test") + UUID::gen().toString();
        if (std::filesystem::exists(dirname)) {
            std::filesystem::remove_all(dirname);
        }
        std::filesystem::create_directory(dirname);
        writtenPackets.clear();
    }

    void tearDown() override {
        std::filesystem::remove_all(dirname);
    }

    auto createRecordingFile() {
        auto filename = std::filesystem::path(dirname);
        filename /= std::to_string(fileCounter++);
        filename += ".bin";
        auto os = std::ofstream(filename.string(), std::ios_base::binary | std::ios_base::out);
        invariant(os.is_open());
        return os;
    }

    void appendToRecording(std::ofstream& os, TrafficRecordingPacket packet) {
        // Store the packet in-memory, for validation.
        writtenPackets.push_back(packet);

        DataBuilder db;
        appendPacketHeader(db, packet);
        // Write out the serialised packet.
        os.write(db.getCursor().data(), db.size());
        os.write(packet.message.buf(), packet.message.size());
    }

    void appendTestPacket(std::ofstream& os) {
        appendToRecording(os,
                          {.id = 1,
                           .session = "test-session",
                           .now = csm.now(),
                           .order = messageCounter++,
                           .message = makeMessage()});
    }

    /**
     * Make a message suitable for appending to a recording.
     */
    Message makeMessage() {
        OpMsgBuilder builder;
        {
            auto seqBuilder = builder.beginDocSequence("foobar-docs");
            seqBuilder.append(fromjson("{'some':'document'}"));
            seqBuilder.append(fromjson("{'randomValue':'" + std::to_string(rand()) + "'}"));
        }
        builder.setBody(fromjson("{'the':'body', 'randomValue':'" + std::to_string(rand()) + "'}"));
        return builder.finish();
    }

    /**
     * Validate the data written to disk in `dirname` matches the in-memory data collected in
     * writtenPackets.
     */
    void validateRecording() {
        // This iterator walks all files in the directory, and then all packets within each file.
        RecordingSetIterator itr(dirname);
        ASSERT(equalSequences(itr, end(itr), writtenPackets.begin(), writtenPackets.end()));
    }


    ClockSourceMock csm;
    size_t fileCounter = 0;
    size_t messageCounter = 0;
    // When writing to disk, packets will also be stashed here.
    // This will be used to validate what is then _read_ from disk.
    std::vector<TrafficRecordingPacket> writtenPackets;
    std::mt19937 rand;
    std::string dirname;
};

TEST_F(TrafficReaderTest, Iterator) {
    TrafficRecordingPacket packet{};
    {
        // Create a single recording file.
        auto f = createRecordingFile();
        appendTestPacket(f);
    }

    validateRecording();
}

TEST_F(TrafficReaderTest, IteratorMultipleRecords) {
    {
        // Create a single recording file.
        auto f = createRecordingFile();
        appendTestPacket(f);
        appendTestPacket(f);
    }

    validateRecording();
}

TEST_F(TrafficReaderTest, IteratorMultipleFiles) {
    {
        // Create the first recording file.
        auto f = createRecordingFile();
        appendTestPacket(f);
        appendTestPacket(f);
    }

    {
        // Create the second recording file.
        auto f = createRecordingFile();
        appendTestPacket(f);
        appendTestPacket(f);
    }

    validateRecording();
}

TEST_F(TrafficReaderTest, IteratorCopySharesMapping) {
    {
        // Create the first recording file.
        auto f = createRecordingFile();
        appendTestPacket(f);
        appendTestPacket(f);
    }

    {
        // Create the second recording file.
        auto f = createRecordingFile();
        appendTestPacket(f);
        appendTestPacket(f);
    }

    RecordingSetIterator iter(dirname);
    auto iter2 = iter;

    // Advance both copies, to ensure a "fresh" value has been read.
    ++iter;
    ++iter2;

    // Message is a const view into the memory-mapped file.
    // If both iterators expose the _same_ pointer, they are correctly
    // both using the same mapped file (i.e., copying the iterator has not re-mapped the file
    // unnecessarily).
    ASSERT_EQ(iter->message.view2ptr(), iter2->message.view2ptr());

    // Advance iter to the first packet in the _second_ file. This will on demand map the new file
    // when first used.
    ++iter;
    // iter2 should find the cached memory map in the shared RecordingInfo.
    ++iter2;

    // Same expectation as before, but into the new mapped file.
    ASSERT_EQ(iter->message.view2ptr(), iter2->message.view2ptr());
}
}  // namespace mongo
