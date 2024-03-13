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

#include "mongo/db/pipeline/change_stream_split_event_helpers.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace change_stream_split_event;

class ChangeStreamSplitEventHelpersTest : public unittest::Test {
public:
    ChangeStreamSplitEventHelpersTest() {
        ResumeTokenData tokenData(
            Timestamp(1000, 1), 2, 0, UUID::gen(), Value(Document{{kIdField, 1}}));
        doc.metadata().setSortKey(Value(ResumeToken(tokenData).toDocument()), true);

        // Approximate the size of the fragment with no data.
        // This size may vary because of the variable serialization of the token - fragments with
        // 'splitEvent.fragment' == 1 are 4 bytes shorter, because the token with
        // 'tokenData.fragmentNum' == 0 are 2 bytes shorter.
        MutableDocument fragment;
        tokenData.fragmentNum = 1UL;
        fragment.metadata().setSortKey(Value(ResumeToken(tokenData).toDocument()), true);
        fragment.addField(kIdField, fragment.metadata().getSortKey());
        fragment.addField(kSplitEventField,
                          Value(Document{{kFragmentNumberField, 1}, {kTotalFragmentsField, 1}}));
        minFragmentSize = static_cast<size_t>(fragment.peek().toBsonWithMetaData().objsize());
    }

    size_t getFieldBsonSize(const Document& doc, StringData key) {
        return static_cast<size_t>(doc.toBson<BSONObj::LargeSizeTrait>().getField(key).size());
    }

    MutableDocument doc;
    size_t minFragmentSize;
    FieldPath fragmentNumberPath =
        FieldPath::getFullyQualifiedPath(kSplitEventField, kFragmentNumberField);
    FieldPath totalFragmentsPath =
        FieldPath::getFullyQualifiedPath(kSplitEventField, kTotalFragmentsField);
};

TEST_F(ChangeStreamSplitEventHelpersTest, EmptyDocThrows) {
    ASSERT_THROWS_CODE(splitChangeEvent(doc.freeze(), minFragmentSize, 0), DBException, 7182502);
}

TEST_F(ChangeStreamSplitEventHelpersTest, DocWithSolelyIdThrows) {
    doc.addField("_id", Value(1));
    ASSERT_THROWS_CODE(splitChangeEvent(doc.freeze(), minFragmentSize, 0), DBException, 7182502);
}

TEST_F(ChangeStreamSplitEventHelpersTest, BasicSplitWithSingleFragment) {
    doc.addField("a", Value(123));
    doc.addField("b", Value(321));
    auto fieldSize = getFieldBsonSize(doc.peek(), "a");
    auto fragments = splitChangeEvent(doc.freeze(), minFragmentSize + fieldSize + fieldSize, 0);
    ASSERT_EQ(1UL, fragments.size());
    auto& fragment = fragments.front();
    ASSERT_EQ(1, fragment.getNestedField(fragmentNumberPath).getInt());
    ASSERT_EQ(1, fragment.getNestedField(totalFragmentsPath).getInt());
    ASSERT_EQ(123, fragment.getField("a").getInt());
    ASSERT_EQ(321, fragment.getField("b").getInt());
}

TEST_F(ChangeStreamSplitEventHelpersTest, ReplacesIdWithFragmentResumeToken) {
    // Replace the test doc's _id with a numeric value. This will be overwritten by the fragment's
    // resume token when we split the event.
    doc.addField("_id", Value(1));
    doc.addField("a", Value(123));
    auto fieldSize = getFieldBsonSize(doc.peek(), "a");
    auto fragments = splitChangeEvent(doc.freeze(), minFragmentSize + fieldSize, 0);
    ASSERT_EQ(1ULL, fragments.size());
    auto& fragment = fragments.front();
    ASSERT_EQ(123, fragment.getField("a").getInt());
    auto tokenData = ResumeToken::parse(fragment.getField(kIdField).getDocument()).getData();
    ASSERT_EQ(0ULL, *tokenData.fragmentNum);
    ASSERT_EQ(tokenData,
              ResumeToken::parse(fragment.metadata().getSortKey().getDocument()).getData());
}

TEST_F(ChangeStreamSplitEventHelpersTest, OversizedFragmentThrows) {
    doc.addField("a", Value("very_long_string"_sd));
    auto fieldSize = getFieldBsonSize(doc.peek(), "a");
    ASSERT_THROWS_CODE(
        splitChangeEvent(doc.freeze(), minFragmentSize + fieldSize - 5, 0), DBException, 7182500);
}

TEST_F(ChangeStreamSplitEventHelpersTest, SplitEventAtMaxSizeBoundary) {
    // Add two fields of equal size. The first fragment will contain the field with the name
    // preceeding in the lexicographic order.
    doc.addField("b", Value(321));
    doc.addField("a", Value(123));
    auto fieldSize = getFieldBsonSize(doc.peek(), "b");
    auto fragments = splitChangeEvent(doc.freeze(), minFragmentSize + fieldSize, 0);
    ASSERT_EQ(2ULL, fragments.size());
    auto &fragment1 = fragments.front(), fragment2 = fragments.back();
    ASSERT_EQ(123, fragment1.getField("a").getInt());
    ASSERT_EQ(321, fragment2.getField("b").getInt());
}

TEST_F(ChangeStreamSplitEventHelpersTest, SplitEventFieldsOrderedInAscendingSize) {
    doc.addField("a", Value("unittesting"_sd));
    doc.addField("b", Value("hello"_sd));
    auto fieldSize = getFieldBsonSize(doc.peek(), "a");
    auto fragments = splitChangeEvent(doc.freeze(), minFragmentSize + fieldSize, 0);
    ASSERT_EQ(2ULL, fragments.size());
    auto &fragment1 = fragments.front(), fragment2 = fragments.back();
    ASSERT_EQ(1, fragment1.getNestedField(fragmentNumberPath).getInt());
    ASSERT_EQ(2, fragment1.getNestedField(totalFragmentsPath).getInt());
    ASSERT_EQ("hello", fragment1.getField("b").getString());
    ASSERT_EQ(2, fragment2.getNestedField(fragmentNumberPath).getInt());
    ASSERT_EQ(2, fragment2.getNestedField(totalFragmentsPath).getInt());
    ASSERT_EQ("unittesting", fragment2.getField("a").getString());
}

TEST_F(ChangeStreamSplitEventHelpersTest, CanSkipFirstNFragments) {
    doc.addField("a", Value("unittesting"_sd));
    doc.addField("b", Value("hello"_sd));
    auto fieldSize = getFieldBsonSize(doc.peek(), "a");
    auto fragmentsSkip1 = splitChangeEvent(doc.peek(), minFragmentSize + fieldSize, 1);
    ASSERT_EQ(1ULL, fragmentsSkip1.size());
    ASSERT_EQ(2, fragmentsSkip1.front().getNestedField(fragmentNumberPath).getInt());
    ASSERT_EQ(2, fragmentsSkip1.front().getNestedField(totalFragmentsPath).getInt());
    auto fragmentsSkip2 = splitChangeEvent(doc.peek(), minFragmentSize + fieldSize, 2);
    ASSERT_EQ(0ULL, fragmentsSkip2.size());
}

}  // namespace
}  // namespace mongo
