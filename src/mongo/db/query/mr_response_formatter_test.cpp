/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/mr_response_formatter.h"
#include "mongo/unittest/unittest.h"


// TODO: SERVER-42644 Update for processing stats from aggregation.

namespace mongo {
namespace {

const NamespaceString testNss("", "mr_response_formatter");
const NamespaceString testNssWithDb("db.mr_response_formatter");
const CursorId testCursor(0);

TEST(MapReduceResponseFormatter, FormatInlineMapReduceResponse) {
    CursorResponse cr(testNss, testCursor, {BSON("a" << 1), BSON("b" << 1)});
    MapReduceResponseFormatter formatter(std::move(cr), boost::none, false);
    BSONObjBuilder builder;
    formatter.appendAsMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [{a: 1}, {b: 1}],"
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatEmptyInlineMapReduceResponse) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), boost::none, false);
    BSONObjBuilder builder;
    formatter.appendAsMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [],"
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatNonInlineMapReduceResponseWithoutDb) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), testNss, false);
    BSONObjBuilder builder;
    formatter.appendAsMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"mr_response_formatter\","
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatNonInlineMapReduceResponseWithDb) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), testNssWithDb, false);
    BSONObjBuilder builder;
    formatter.appendAsMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: {db: \"db\", collection: \"mr_response_formatter\"},"
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatVerboseMapReduceResponse) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), testNss, true);
    BSONObjBuilder builder;
    formatter.appendAsMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"mr_response_formatter\","
                               "timeMillis: 0,"
                               "timing: {mapTime: 0, emitLoop: 0, reduceTime: 0, total: 0},"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatInlineClusterMapReduceResponse) {
    CursorResponse cr(testNss, testCursor, {BSON("a" << 1), BSON("b" << 1)});
    MapReduceResponseFormatter formatter(std::move(cr), boost::none, false);
    BSONObjBuilder builder;
    formatter.appendAsClusterMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [{a: 1}, {b: 1}], "
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0}, "
                               "timeMillis: 0, "
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatEmptyInlineClusterMapReduceResponseWithoutDb) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), boost::none, false);
    BSONObjBuilder builder;
    formatter.appendAsClusterMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [],"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "timeMillis: 0,"
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatNonInlineClusterMapReduceResponseWithDb) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), testNssWithDb, false);
    BSONObjBuilder builder;
    formatter.appendAsClusterMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: {db: \"db\", collection: \"mr_response_formatter\"},"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "timeMillis: 0,"
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatNonInlineClusterMapReduceResponse) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), testNss, false);
    BSONObjBuilder builder;
    formatter.appendAsClusterMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"mr_response_formatter\","
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "timeMillis: 0,"
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceResponseFormatter, FormatVerboseClusterMapReduceResponse) {
    CursorResponse cr(testNss, testCursor, {});
    MapReduceResponseFormatter formatter(std::move(cr), testNss, true);
    BSONObjBuilder builder;
    formatter.appendAsClusterMapReduceResponse(&builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"mr_response_formatter\","
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "timeMillis: 0,"
                               "timing: {shardProcessing: 0, postProcessing: 0},"
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

}  // namespace
}  // namespace mongo
