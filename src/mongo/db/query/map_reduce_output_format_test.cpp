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
#include "mongo/db/query/map_reduce_output_format.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(MapReduceOutputFormat, FormatInlineMapReduceResponse) {
    BSONArrayBuilder documents;
    documents.append(BSON("a" << 1));
    documents.append(BSON("b" << 1));
    BSONObjBuilder builder;
    map_reduce_output_format::appendInlineResponse(documents.arr(), false, false, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [{a: 1}, {b: 1}],"
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatEmptyInlineMapReduceResponse) {
    BSONArrayBuilder documents;
    BSONObjBuilder builder;
    map_reduce_output_format::appendInlineResponse(documents.arr(), false, false, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [],"
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatNonInlineMapReduceResponseWithoutDb) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(NamespaceString("", "c"), false, false, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"c\","
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatNonInlineMapReduceResponseWithDb) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(NamespaceString("db", "c"), false, false, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: {db: \"db\", collection: \"c\"},"
                               "timeMillis: 0,"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatVerboseMapReduceResponse) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(NamespaceString("", "c"), true, false, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"c\","
                               "timeMillis: 0,"
                               "timing: {mapTime: 0, emitLoop: 0, reduceTime: 0, total: 0},"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatInlineClusterMapReduceResponse) {
    BSONArrayBuilder documents;
    documents.append(BSON("a" << 1));
    documents.append(BSON("b" << 1));
    BSONObjBuilder builder;
    map_reduce_output_format::appendInlineResponse(documents.arr(), false, true, &builder);
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

TEST(MapReduceOutputFormat, FormatEmptyInlineClusterMapReduceResponse) {
    BSONArrayBuilder documents;
    BSONObjBuilder builder;
    map_reduce_output_format::appendInlineResponse(documents.arr(), false, true, &builder);
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

TEST(MapReduceOutputFormat, FormatNonInlineClusterMapReduceResponseWithoutDb) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(NamespaceString("", "c"), false, true, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"c\","
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "timeMillis: 0,"
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatNonInlineClusterMapReduceResponseWithDb) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(NamespaceString("db", "c"), false, true, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: {db: \"db\", collection: \"c\"},"
                               "counts: {input: 0, emit: 0, reduce: 0, output: 0},"
                               "timeMillis: 0,"
                               "shardCounts: {"
                               "\"shard-conn-string\": {input: 0, emit: 0, reduce: 0, output: 0}}, "
                               "postProcessCounts: {"
                               "\"merging-shard-conn-string\": {input: 0, reduce: 0, output: 0}},"
                               "ok: 1}"),
                      builder.obj());
}

TEST(MapReduceOutputFormat, FormatVerboseClusterMapReduceResponse) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(NamespaceString("", "c"), true, true, &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"c\","
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
