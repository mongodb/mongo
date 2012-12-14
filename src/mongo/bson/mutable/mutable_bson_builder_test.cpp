/* Copyright 2010 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_builder.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/mutable_bson.h"
#include "mongo/bson/mutable/mutable_bson_heap.h"
#include "mongo/bson/mutable/mutable_bson_internal.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

    // TODO: This object should contain a representative for every
    // BSON type, including deprecated values.
    static const char jsonSample[] =
        "{_id:ObjectId(\"47cc67093475061e3d95369d\"),"
        "query:\"kate hudson\","
        "owner:1234567887654321,"
        "date:\"2011-05-13T14:22:46.777Z\","
        "score:123.456,"
        "field1:Infinity,"
        "\"field2\":-Infinity,"
        "\"field3\":NaN,"
        "users:["
        "{uname:\"@aaaa\",editid:\"1234\",date:1303959350,yes_votes:0,no_votes:0},"
        "{uname:\"@bbbb\",editid:\"5678\",date:1303959350,yes_votes:0,no_votes:0}],"
        "pattern:/match.*this/,"
        "lastfield:\"last\"}";

    TEST(BuilderAPI, RoundTrip) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        int len;
        mongo::BSONObj obj = mongo::fromjson(jsonSample, &len);
        mongo::mutablebson::ElementBuilder::parse(obj, &doc);

        mongo::BSONObjBuilder builder;
        mongo::mutablebson::BSONBuilder::build(doc.root(), &builder);
        mongo::BSONObj built = builder.done();

        // TODO: When both builders are feature complete, add a an assert
        // that the round tripped objects are equivalent.
    }

} // unnamed namespace
