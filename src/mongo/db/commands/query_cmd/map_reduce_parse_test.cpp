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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/commands/query_cmd/map_reduce_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {
namespace {

// The parser treats Javascript objects as black boxes so there's no need for realistic examples
// here.
constexpr auto initJavascript = "init!"_sd;
constexpr auto mapJavascript = "map!"_sd;
constexpr auto reduceJavascript = "reduce!"_sd;
constexpr auto finalizeJavascript = "finalize!"_sd;

TEST(MapReduceParseTest, failedParse) {
    auto ctx = IDLParserContext("mapReduce");
    // Missing fields.
    ASSERT_THROWS(MapReduceCommandRequest::parse(BSON("" << ""
                                                         << "$db"
                                                         << "db"),
                                                 ctx),
                  DBException);
    ASSERT_THROWS(MapReduceCommandRequest::parse(BSON("mapReduce" << "foo"
                                                                  << "$db"
                                                                  << "db"),
                                                 ctx),
                  DBException);
    ASSERT_THROWS(
        MapReduceCommandRequest::parse(BSON("map" << mapJavascript << "reduce" << reduceJavascript
                                                  << "out" << BSON("inline" << 1) << "$db"
                                                  << "db"),
                                       ctx),
        DBException);

    // Extra fields.
    ASSERT_THROWS(MapReduceCommandRequest::parse(
                      BSON("mapReduce" << "theSource"
                                       << "map" << mapJavascript << "reduce" << reduceJavascript
                                       << "out" << BSON("inline" << 1) << "alloy"
                                       << "chromium steel"
                                       << "$db"
                                       << "db"),
                      ctx),
                  DBException);
    ASSERT_THROWS(MapReduceCommandRequest::parse(
                      BSON("mapReduce" << "theSource"
                                       << "map" << mapJavascript << "reduce" << reduceJavascript
                                       << "out" << BSON("inline" << 1 << "notinline" << 0) << "$db"
                                       << "db"),
                      ctx),
                  DBException);
}

TEST(MapReduceParseTest, failsToParseCodeWithScope) {
    auto ctx = IDLParserContext("mapReduce");

    ASSERT_THROWS(
        MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                        << "map" << mapJavascript << "reduce"
                                                        << BSONCodeWScope("var x = 3", BSONObj())
                                                        << "out" << BSON("inline" << 1) << "$db"
                                                        << "db"),
                                       ctx),
        DBException);
    ASSERT_THROWS(
        MapReduceCommandRequest::parse(
            BSON("mapReduce" << "theSource"
                             << "map" << BSONCodeWScope("var x = 3", BSONObj()) << "reduce"
                             << reduceJavascript << "out" << BSON("inline" << 1) << "$db"
                             << "db"),
            ctx),
        DBException);
}

TEST(MapReduceParseTest, parseOutputTypes) {
    auto ctx = IDLParserContext("mapReduce");

    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("inline" << 1) << "$db"
                                                    << "db"),
                                   ctx);
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << "theSink"
                                                    << "$db"
                                                    << "db"),
                                   ctx);
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("replace" << "theSink"
                                                                      << "db"
                                                                      << "myDb")
                                                    << "$db"
                                                    << "db"),
                                   ctx);
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("merge" << "theSink") << "$db"
                                                    << "db"),
                                   ctx);
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("reduce" << "theSink"
                                                                     << "db"
                                                                     << "myDb"
                                                                     << "sharded" << true)
                                                    << "$db"
                                                    << "db"),
                                   ctx);
    ASSERT(true);
}

TEST(MapReduceParseTest, parseAllOptionalFields) {
    auto ctx = IDLParserContext("mapReduce");

    MapReduceCommandRequest::parse(
        BSON("mapReduce" << "theSource"
                         << "map" << mapJavascript << "reduce" << reduceJavascript << "out"
                         << BSON("inline" << 1) << "query" << BSON("author" << "dave") << "sort"
                         << BSON("bottlecaps" << 1) << "collation"
                         << BSON("locale" << "zh@collation=pinyin") << "limit" << 86 << "finalize"
                         << finalizeJavascript << "scope" << BSON("global" << initJavascript)
                         << "verbose" << false << "bypassDocumentValidation" << true
                         << "writeConcern" << BSON("w" << 1 << "j" << false << "wtimeout" << 1498)
                         << "$db"
                         << "db"),
        ctx);
}

TEST(MapReduceParseTest, deprecatedOptions) {
    auto ctx = IDLParserContext("mapReduce");
    // jsMode can be true or false
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("inline" << 1) << "$db"
                                                    << "db"
                                                    << "jsMode" << true),
                                   ctx);
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("inline" << 1) << "$db"
                                                    << "db"
                                                    << "jsMode" << false),
                                   ctx);
    // nonAtomic can be true but not false
    MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                    << "map" << mapJavascript << "reduce"
                                                    << reduceJavascript << "out"
                                                    << BSON("reduce" << "theSink"
                                                                     << "db"
                                                                     << "myDb"
                                                                     << "nonAtomic" << true)
                                                    << "$db"
                                                    << "db"),
                                   ctx);
    ASSERT_THROWS(
        MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                        << "map" << mapJavascript << "reduce"
                                                        << reduceJavascript << "out"
                                                        << BSON("reduce" << "theSink"
                                                                         << "db"
                                                                         << "myDb"
                                                                         << "nonAtomic" << false)
                                                        << "$db"
                                                        << "db"),
                                       ctx),
        DBException);
    ASSERT_THROWS(
        MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                        << "map" << mapJavascript << "reduce"
                                                        << reduceJavascript << "out"
                                                        << BSON("reduce" << "theSink"
                                                                         << "db"
                                                                         << "myDb"
                                                                         << "nonAtomic" << false)
                                                        << "$db"
                                                        << "db"),
                                       ctx),
        DBException);
    // out.sharded cannot be false
    ASSERT_THROWS(
        MapReduceCommandRequest::parse(BSON("mapReduce" << "theSource"
                                                        << "map" << mapJavascript << "reduce"
                                                        << reduceJavascript << "out"
                                                        << BSON("reduce" << "theSink"
                                                                         << "db"
                                                                         << "myDb"
                                                                         << "sharded" << false)
                                                        << "$db"
                                                        << "db"),
                                       ctx),
        DBException);
}

}  // namespace
}  // namespace mongo
