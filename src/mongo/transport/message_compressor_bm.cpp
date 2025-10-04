/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/message_compressor_zlib.h"
#include "mongo/transport/message_compressor_zstd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {

namespace {
Message genericMsg() {
    // example simple genny doc
    // https://github.com/mongodb/genny/blob/master/src/workloads/docs/Generators.yml#L9
    std::string sampleDoc =
        R"({
    "_id" : "5fd7c984e0f8d74bdb04d3b4",
    "type" : "simple",
    "int1" : "15",
    "int2" : "19",
    "int3" : "7",
    "int4" : "1835",
    "int5" : "3",
    "int6" : "112",
    "int7" : "3",
    "double" : "12.830223930995928",
    "string1" : "T336d",
    "string2" : "B2A54",
    "string3" : "LChkGtHKBI",
    "counter" : "1008",
    "choose" : "12",
    "join" : "This is a joined string",
    "nested": [
        {
        "_id" : "5fd7c984e0f8d74bdb04d3b8",
        "type" : "nested",
        "a" : "cjyuclu2jVjxpo5nSG",
        "b" : "aaaaaaaaaa5phojR5oVp",
        "c" : "LcSd8voZ-Q7F3jXcT-clWnwH/N-XP7caBjG",
        "task_id" : "test-duH4N0/500",
        "id_with_actor" : "ActorId-7"
        }
    ],
    "ip" : "77.220.130.163",
    "location" : ["17.31871956875809", "-3.062558894823418"],
    "actorNum" : "1",
    "actorString" : "1",
    "airportDataSet" : "KSC"
    })";

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(fromjson(sampleDoc));
    auto msg = msgBuilder.finish();
    msg.header().setId(123456);
    msg.header().setResponseToMsgId(654321);
    msg.header().setOperation(dbMsg);
    return msg;
}

Message copyMessage(const Message& original) {
    auto resultBuffer = SharedBuffer::allocate(original.size());
    std::copy_n(original.buf(), original.size(), resultBuffer.get());
    return Message(std::move(resultBuffer));
}

void setCompressor(MessageCompressorRegistry& registry,
                   std::unique_ptr<MessageCompressorBase> compressorType) {
    registry.setSupportedCompressors({compressorType->getName()});
    registry.registerImplementation(std::move(compressorType));
}

void runCompressBM(benchmark::State& state, std::unique_ptr<MessageCompressorBase> compressorType) {
    MessageCompressorRegistry registry;
    MessageCompressorManager manager(&registry);

    auto compId = compressorType->getId();
    setCompressor(registry, std::move(compressorType));

    size_t totalSize = 0;
    Message sendMsg = genericMsg();
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(uassertStatusOK(manager.compressMessage(sendMsg, &compId)));
        totalSize += sendMsg.dataSize();
    }
    state.SetBytesProcessed(totalSize);
}

void runDecompressBM(benchmark::State& state,
                     std::unique_ptr<MessageCompressorBase> compressorType) {
    MessageCompressorRegistry registry;
    MessageCompressorManager manager(&registry);

    auto compId = compressorType->getId();
    setCompressor(registry, std::move(compressorType));

    size_t totalSize = 0;
    Message sendMsg = genericMsg();
    auto compressedMsg = uassertStatusOK(manager.compressMessage(sendMsg, &compId));
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(
            uassertStatusOK(manager.decompressMessage(compressedMsg, nullptr)));
        totalSize += sendMsg.dataSize();
    }
    state.SetBytesProcessed(totalSize);
}

void BM_copy(benchmark::State& state) {
    size_t totalSize = 0;
    Message input = genericMsg();
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(copyMessage(input));
        totalSize += input.size();
    }
    state.SetBytesProcessed(totalSize);
}

void BM_snappy_compress(benchmark::State& state) {
    runCompressBM(state, std::make_unique<SnappyMessageCompressor>());
}

void BM_snappy_decompress(benchmark::State& state) {
    runDecompressBM(state, std::make_unique<SnappyMessageCompressor>());
}

void BM_zlib_compress(benchmark::State& state) {
    runCompressBM(state, std::make_unique<ZlibMessageCompressor>());
}

void BM_zlib_decompress(benchmark::State& state) {
    runDecompressBM(state, std::make_unique<ZlibMessageCompressor>());
}

void BM_zstd_compress(benchmark::State& state) {
    runCompressBM(state, std::make_unique<ZstdMessageCompressor>());
}

void BM_zstd_decompress(benchmark::State& state) {
    runDecompressBM(state, std::make_unique<ZstdMessageCompressor>());
}

BENCHMARK(BM_copy);

BENCHMARK(BM_snappy_compress);
BENCHMARK(BM_snappy_decompress);

BENCHMARK(BM_zlib_compress);
BENCHMARK(BM_zlib_decompress);

BENCHMARK(BM_zstd_compress);
BENCHMARK(BM_zstd_decompress);
}  // namespace
}  // namespace mongo
