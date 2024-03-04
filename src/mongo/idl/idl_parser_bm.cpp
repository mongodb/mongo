/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <array>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/idl_parser_bm_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

// YCSB findOne query
BSONObj getFindDocWithDb() {
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    return BSON("find"
                << "usertable"
                << "filter"
                << BSON("_id"
                        << "user4409142221109489457")
                << "limit" << 1 << "singleBatch" << true << "$db"
                << "ycsb"
                << "lsid" << BSON("id" << u));
}

// $clusterTime
BSONObj getClusterTime() {
    char hash_bytes[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    return BSON(
        "clusterTime" << Timestamp(1654272333, 13) << "signature"
                      << BSON("hash" << BSONBinData(hash_bytes, sizeof(hash_bytes), BinDataGeneral))
                      << "keyId" << 0);
}

void BM_FIND_ONE_BSON(benchmark::State& state) {
    // Perform setup here
    auto doc = getFindDocWithDb();

    for (auto _ : state) {
        // This code gets timed
        FindCommandRequestBase::parse(IDLParserContext("foo"), doc);
    }
}

BENCHMARK(BM_FIND_ONE_BSON)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);

void BM_FIND_ONE_OP_MSG(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    BSONObj doc = BSON("find"
                       << "usertable"
                       << "filter"
                       << BSON("_id"
                               << "user4409142221109489457")
                       << "limit" << 1 << "singleBatch" << true << "lsid" << BSON("id" << u)
                       << "$clusterTime" << getClusterTime());

    auto request = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired /* db is not tenanted */,
        DatabaseName::createDatabaseName_forTest(boost::none, "ycsb"),
        doc);

    for (auto _ : state) {
        // This code gets timed
        FindCommandRequestBase::parse(IDLParserContext("foo"), request);
    }
}

BENCHMARK(BM_FIND_ONE_OP_MSG)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);


char field_bytes[] = {0x34, 0x27, 0x33, 0x27, 0x32, 0x26, 0x26, 0x2d, 0x3b, 0x21, 0x23, 0x36, 0x25,
                      0x3f, 0x2d, 0x32, 0x3b, 0x3e, 0x36, 0x3b, 0x20, 0x35, 0x30, 0x34, 0x2f, 0x25,
                      0x36, 0x37, 0x37, 0x37, 0x35, 0x38, 0x29, 0x29, 0x21, 0x22, 0x25, 0x37, 0x34,
                      0x3d, 0x37, 0x29, 0x39, 0x32, 0x36, 0x3a, 0x25, 0x24, 0x3c, 0x2b, 0x3a, 0x37,
                      0x29, 0x32, 0x22, 0x28, 0x39, 0x22, 0x3f, 0x35, 0x3c, 0x36, 0x23, 0x37, 0x24,
                      0x27, 0x29, 0x23, 0x3a, 0x2e, 0x23, 0x28, 0x26, 0x36, 0x2d, 0x20, 0x36, 0x35,
                      0x2c, 0x39, 0x2f, 0x37, 0x3d, 0x22, 0x2e, 0x27, 0x34, 0x33, 0x3e, 0x29, 0x3c,
                      0x2c, 0x3a, 0x2f, 0x3f, 0x28, 0x3d, 0x2b, 0x2c, 0x32};

// YCSB insert
BSONObj getInsertDoc() {
    return BSON("_id"
                << "user2996592682669871081"
                << "field1" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field0" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field7" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field6" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field9" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field8" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field3" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field2" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field5" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral)
                << "field4" << BSONBinData(field_bytes, sizeof(field_bytes), BinDataGeneral));
}


void BM_INSERT_ONE_BSON(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    BSONObj doc = BSON("insert"
                       << "usertable"
                       << "documents" << BSON_ARRAY(getInsertDoc()) << "ordered" << true << "$db"
                       << "ycsb"
                       << "lsid" << BSON("id" << u) << "$clusterTime" << getClusterTime());

    for (auto _ : state) {
        // This code gets timed
        write_ops::InsertCommandRequest::parse(IDLParserContext("foo"), doc);
    }
}

BENCHMARK(BM_INSERT_ONE_BSON)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);


void BM_INSERT_ONE_OP_MSG(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    BSONObj doc = BSON("insert"
                       << "usertable"
                       << "ordered" << true << "lsid" << BSON("id" << u) << "$clusterTime"
                       << getClusterTime());

    auto request = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired /* db is not tenanted */,
        DatabaseName::createDatabaseName_forTest(boost::none, "ycsb"),
        doc);
    request.sequences.push_back({"documents", {getInsertDoc()}});

    for (auto _ : state) {
        // This code gets timed
        write_ops::InsertCommandRequest::parse(IDLParserContext("foo"), request);
    }
}

BENCHMARK(BM_INSERT_ONE_OP_MSG)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);

// YCSB updateOne
BSONObj getUpdateDoc() {
    return BSON("q" << BSON("_id"
                            << "user4409142221109489457")
                    << "u"
                    << BSON("$set" << BSON("field3" << BSONBinData(
                                               field_bytes, sizeof(field_bytes), BinDataGeneral)))
                    << "multi" << false << "upsert" << false);
}


void BM_UPDATE_ONE_BSON(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    BSONObj doc = BSON("update"
                       << "usertable"
                       << "updates" << BSON_ARRAY(getUpdateDoc()) << "$db"
                       << "ycsb"
                       << "lsid" << BSON("id" << u) << "$clusterTime" << getClusterTime());

    for (auto _ : state) {
        // This code gets timed
        write_ops::UpdateCommandRequest::parse(IDLParserContext("foo"), doc);
    }
}

BENCHMARK(BM_UPDATE_ONE_BSON)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);


void BM_UPDATE_ONE_OP_MSG(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    BSONObj doc = BSON("update"
                       << "usertable"
                       <<


                       "lsid" << BSON("id" << u) << "$clusterTime" << getClusterTime());

    auto request = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired /* db is not tenanted */,
        DatabaseName::createDatabaseName_forTest(boost::none, "ycsb"),
        doc);
    request.sequences.push_back({"updates", {getUpdateDoc()}});

    for (auto _ : state) {
        // This code gets timed
        write_ops::UpdateCommandRequest::parse(IDLParserContext("foo"), request);
    }
}

BENCHMARK(BM_UPDATE_ONE_OP_MSG)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);


void BM_IS_GENERIC_ARG(benchmark::State& state) {
    // Perform setup here
    auto arg = "lsid"_sd;
    int i = 0;

    for (auto _ : state) {
        // This code gets timed
        i += (int)isGenericArgument(arg);
    }
}

BENCHMARK(BM_IS_GENERIC_ARG)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);

// API Version parses all requests but is often not present. So test API Version against a query
// where it is not present
void BM_API_VERSION_BSON(benchmark::State& state) {
    // Perform setup here
    auto doc = getFindDocWithDb();

    for (auto _ : state) {
        // This code gets timed
        APIParametersFromClient::parse(IDLParserContext("foo"), doc);
    }
}

BENCHMARK(BM_API_VERSION_BSON)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);


void BM_LSID_SIMPLE_BSON(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    auto doc = BSON("id" << u);

    for (auto _ : state) {
        // This code gets timed
        OperationSessionInfoFromClient::parse(IDLParserContext("foo"), doc);
    }
}

BENCHMARK(BM_LSID_SIMPLE_BSON)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);


void BM_LSID_TXN_BSON(benchmark::State& state) {
    // Perform setup here
    auto u = UUID::parse("d4965626-a298-4b09-b80b-8d98d17d02d2").getValue();

    uint8_t uid_bytes[] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                           0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                           0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};

    auto txnU = UUID::parse("ef67ae89-1fd3-48b0-aa86-d8a5dc4c0453").getValue();

    auto doc = BSON("id" << u << "uid" << BSONBinData(uid_bytes, sizeof(uid_bytes), BinDataGeneral)
                         << "txnNumber" << 37LL << "txnUUID" << txnU);

    for (auto _ : state) {
        // This code gets timed
        OperationSessionInfoFromClient::parse(IDLParserContext("foo"), doc);
    }
}

BENCHMARK(BM_LSID_TXN_BSON)->Arg(10)->Arg(100)->Unit(benchmark::kNanosecond);

void BM_ARRAY_BSON(benchmark::State& state) {
    // Perform setup here
    auto nEntries = state.range();
    BSONObjBuilder bob;
    BSONArrayBuilder bab(bob.subarrayStart("value"));
    for (int i = 0; i < nEntries; i++) {
        bab.append(int32_t(i + 101));
    }
    bab.done();
    auto doc = bob.done();
    size_t totalBytes = 0;

    for (auto _ : state) {
        // This code gets timed
        benchmark::DoNotOptimize(idl::test::OneArray::parse(IDLParserContext("test"), doc));
        totalBytes += doc.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

BENCHMARK(BM_ARRAY_BSON)->Arg(10)->Arg(1000)->Arg(10000)->Unit(benchmark::kNanosecond);

void BM_CHECK_AND_ASSERT_TYPES(benchmark::State& state) {
    // Perform setup here.
    // We construct a WriteCommandRequestBase with just the "bypassDocumentValidation"
    // field set, which is of type safeBool, to run the generated checkAndAssertTypes()
    // function.
    auto request = BSON("bypassDocumentValidation" << 1);

    for (auto _ : state) {
        // This code gets timed
        benchmark::DoNotOptimize(
            write_ops::WriteCommandRequestBase::parse(IDLParserContext("test"), request));
    }
}

BENCHMARK(BM_CHECK_AND_ASSERT_TYPES);

void BM_ArrayOfStringEnum10(benchmark::State& state) {
    namespace t = idl::test;
    auto nEntries = state.range();
    auto doc = [&] {
        std::vector<t::HasStringEnum10> vec;
        for (int i = 0; i < nEntries; i++) {
            idl::test::HasStringEnum10 has;
            has.setE(static_cast<t::StringEnum10Enum>(i % 10));
            vec.push_back(has);
        }
        t::ArrayOfHasStringEnum10 arrStruct;
        arrStruct.setValue(vec);

        BSONObjBuilder bob;
        arrStruct.serialize(&bob);
        return bob.obj();
    }();

    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(t::ArrayOfHasStringEnum10::parse(IDLParserContext("test"), doc));
        totalBytes += doc.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

BENCHMARK(BM_ArrayOfStringEnum10)->Arg(10)->Arg(100)->Arg(1000);

}  // namespace
}  // namespace mongo
