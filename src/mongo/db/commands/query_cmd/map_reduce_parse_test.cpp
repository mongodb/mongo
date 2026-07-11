// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/commands/query_cmd/map_reduce_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// The parser treats Javascript objects as black boxes so there's no need for realistic examples
// here.
constexpr auto initJavascript = "init!"sv;
constexpr auto mapJavascript = "map!"sv;
constexpr auto reduceJavascript = "reduce!"sv;
constexpr auto finalizeJavascript = "finalize!"sv;

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
