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

    TEST(StreamingAPI, FromJSON) {
        mongo::mutablebson::BasicHeap myHeap;
        mongo::mutablebson::Document doc(&myHeap);

        const char* jsonSample =
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

        int len;
        mongo::BSONObj obj = mongo::fromjson(jsonSample, &len);
        mongo::mutablebson::Element e = doc.makeObjElement("root");
        mongo::mutablebson::ElementBuilder::parse(&e, obj); 
        mongo::mutablebson::SubtreeIterator it(e);

        ASSERT_EQUALS(it.done(), false);
        ASSERT_EQUALS("root", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("_id", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("query", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("owner", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("date", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("score", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("field1", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("field2", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("field3", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("users", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("0", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("uname", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("editid", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("date", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("yes_votes", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("no_votes", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("1", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("uname", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("editid", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("date", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("yes_votes", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("no_votes", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("pattern", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());
        ASSERT_EQUALS((++it).done(), false);
        ASSERT_EQUALS("lastfield", mongo::mutablebson::Element(&doc, it.getRep()).fieldName());

        ASSERT_EQUALS((++it).done(), true);
    }

} // unnamed namespace
