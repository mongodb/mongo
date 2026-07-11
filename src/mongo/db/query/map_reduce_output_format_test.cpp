// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/map_reduce_output_format.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <boost/none.hpp>

using namespace std::literals::string_literals;

namespace mongo {
namespace {

TEST(MapReduceOutputFormat, FormatInlineMapReduceResponse) {
    BSONArrayBuilder documents;
    documents.append(BSON("a" << 1));
    documents.append(BSON("b" << 1));

    BSONObjBuilder builder;
    map_reduce_output_format::appendInlineResponse(documents.arr(), &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: [{a: 1}, {b: 1}]}"), builder.obj());
}

TEST(MapReduceOutputFormat, FormatEmptyInlineMapReduceResponse) {
    BSONArrayBuilder documents;
    BSONObjBuilder builder;
    map_reduce_output_format::appendInlineResponse(documents.arr(), &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{results: []}"), builder.obj());
}

TEST(MapReduceOutputFormat, FormatNonInlineMapReduceResponseWithoutDb) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse(boost::none, "c", &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: \"c\"}"), builder.obj());
}

TEST(MapReduceOutputFormat, FormatNonInlineMapReduceResponseWithDb) {
    BSONObjBuilder builder;
    map_reduce_output_format::appendOutResponse("db"s, "c", &builder);
    ASSERT_BSONOBJ_EQ(fromjson("{result: {db: \"db\", collection: \"c\"}}"), builder.obj());
}

}  // namespace
}  // namespace mongo
