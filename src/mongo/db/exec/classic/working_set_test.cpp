/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/working_set.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

class WorkingSetFixture : public mongo::unittest::Test {
protected:
    void setUp() override {
        ws.reset(new WorkingSet());
        id = ws->allocate();
        ASSERT(id != WorkingSet::INVALID_ID);
        member = ws->get(id);
        ASSERT(nullptr != member);
    }

    void tearDown() override {
        ws.reset();
        member = nullptr;
    }

    std::unique_ptr<WorkingSet> ws;
    WorkingSetID id;
    WorkingSetMember* member;
};

TEST_F(WorkingSetFixture, noFieldToGet) {
    BSONElement elt;

    // Make sure we're not getting anything out of an invalid WSM.
    ASSERT_EQUALS(WorkingSetMember::INVALID, member->getState());
    ASSERT_FALSE(member->getFieldDotted("foo", &elt));

    ws->transitionToRecordIdAndIdx(id);
    ASSERT_FALSE(member->getFieldDotted("foo", &elt));

    // Our state is that of a valid object.  The getFieldDotted shouldn't throw; there's
    // something to call getFieldDotted on, but there's no field there.
    ws->transitionToRecordIdAndObj(id);
    ASSERT_TRUE(member->getFieldDotted("foo", &elt));

    WorkingSetMember* member = ws->get(id);
    member->doc = {SnapshotId(), Document{BSON("fake" << "obj")}};
    ws->transitionToOwnedObj(id);
    ASSERT_TRUE(member->getFieldDotted("foo", &elt));
}

TEST_F(WorkingSetFixture, getFieldUnowned) {
    string fieldName = "x";

    BSONObj obj = BSON(fieldName << 5);
    // Not truthful since the RecordId is bogus, but the RecordId isn't accessed anyway...
    ws->transitionToRecordIdAndObj(id);
    member->doc = {SnapshotId(), Document{BSONObj(obj.objdata())}};
    ASSERT_TRUE(obj.isOwned());
    ASSERT_FALSE(member->doc.value().isOwned());

    // Get out the field we put in.
    BSONElement elt;
    ASSERT_TRUE(member->getFieldDotted(fieldName, &elt));
    ASSERT_EQUALS(elt.numberInt(), 5);
}

TEST_F(WorkingSetFixture, getFieldOwned) {
    string fieldName = "x";

    BSONObj obj = BSON(fieldName << 5);
    member->doc = {SnapshotId(), Document{obj}};
    ASSERT_TRUE(member->doc.value().isOwned());
    ws->transitionToOwnedObj(id);
    BSONElement elt;
    ASSERT_TRUE(member->getFieldDotted(fieldName, &elt));
    ASSERT_EQUALS(elt.numberInt(), 5);
}

TEST_F(WorkingSetFixture, getFieldFromIndex) {
    string firstName = "x";
    int firstValue = 5;

    string secondName = "y";
    int secondValue = 10;

    member->keyData.push_back(
        IndexKeyDatum(BSON(firstName << 1), BSON("" << firstValue), 0, SnapshotId{}));
    // Also a minor lie as RecordId is bogus.
    ws->transitionToRecordIdAndIdx(id);
    BSONElement elt;
    ASSERT_TRUE(member->getFieldDotted(firstName, &elt));
    ASSERT_EQUALS(elt.numberInt(), firstValue);
    // No foo field.
    ASSERT_FALSE(member->getFieldDotted("foo", &elt));

    // Add another index datum.
    member->keyData.push_back(
        IndexKeyDatum(BSON(secondName << 1), BSON("" << secondValue), 0, SnapshotId{}));
    ASSERT_TRUE(member->getFieldDotted(secondName, &elt));
    ASSERT_EQUALS(elt.numberInt(), secondValue);
    ASSERT_TRUE(member->getFieldDotted(firstName, &elt));
    ASSERT_EQUALS(elt.numberInt(), firstValue);
    // Still no foo.
    ASSERT_FALSE(member->getFieldDotted("foo", &elt));
}

TEST_F(WorkingSetFixture, getDottedFieldFromIndex) {
    string firstName = "x.y";
    int firstValue = 5;

    member->keyData.push_back(
        IndexKeyDatum(BSON(firstName << 1), BSON("" << firstValue), 0, SnapshotId{}));
    ws->transitionToRecordIdAndIdx(id);
    BSONElement elt;
    ASSERT_TRUE(member->getFieldDotted(firstName, &elt));
    ASSERT_EQUALS(elt.numberInt(), firstValue);
    ASSERT_FALSE(member->getFieldDotted("x", &elt));
    ASSERT_FALSE(member->getFieldDotted("y", &elt));
}

TEST_F(WorkingSetFixture, MetadataCanBeCorrectlyTransferredBackAndForthFromDocument) {
    // Add some metadata to the WSM.
    member->metadata().setTextScore(1.2);
    member->metadata().setSearchScore(3.4);

    // Test that the metadata can be extracted from the WSM.
    auto releasedMetadata = member->releaseMetadata();
    ASSERT_FALSE(member->metadata());
    ASSERT_FALSE(member->metadata().hasTextScore());
    ASSERT_FALSE(member->metadata().hasSearchScore());
    ASSERT_TRUE(releasedMetadata);
    ASSERT_TRUE(releasedMetadata.hasTextScore());
    ASSERT_TRUE(releasedMetadata.hasSearchScore());

    // Test that the extracted metadata can be added to a Document.
    Document document;
    MutableDocument md{std::move(document)};
    md.setMetadata(std::move(releasedMetadata));
    document = md.freeze();
    ASSERT_FALSE(releasedMetadata);                   // NOLINT(bugprone-use-after-move)
    ASSERT_FALSE(releasedMetadata.hasTextScore());    // NOLINT(bugprone-use-after-move)
    ASSERT_FALSE(releasedMetadata.hasSearchScore());  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(document.metadata());
    ASSERT_TRUE(document.metadata().hasTextScore());
    ASSERT_TRUE(document.metadata().hasSearchScore());

    // Test that metadata can be transferred back to the WSM.
    MutableDocument md2{std::move(document)};
    member->setMetadata(md2.releaseMetadata());
    document = md2.freeze();
    ASSERT_FALSE(document.metadata());
    ASSERT_FALSE(document.metadata().hasTextScore());
    ASSERT_FALSE(document.metadata().hasSearchScore());
    ASSERT_TRUE(member->metadata());
    ASSERT_TRUE(member->metadata().hasTextScore());
    ASSERT_TRUE(member->metadata().hasSearchScore());
}

namespace {
// Serializes the given working set member to a buffer, then returns a working set member resulting
// from deserializing this buffer.
WorkingSetMember roundtripWsmThroughSerialization(WorkingSetMember wsm) {
    SortableWorkingSetMember sortableWsm{std::move(wsm)};
    BufBuilder builder{};
    sortableWsm.serializeForSorter(builder);
    BufReader reader{builder.buf(), static_cast<unsigned>(builder.len())};
    auto deserializedSortableWsm = SortableWorkingSetMember::deserializeForSorter(
        reader, SortableWorkingSetMember::SorterDeserializeSettings{});
    return deserializedSortableWsm.extract();
}
}  // namespace

TEST_F(WorkingSetFixture, RecordIdLongAndObjStateCanRoundtripThroughSerialization) {
    Document doc{{"foo", Value{"bar"_sd}}};
    member->doc.setValue(doc);
    member->doc.setSnapshotId(SnapshotId{42u});
    member->recordId = RecordId{43};
    ws->transitionToRecordIdAndObj(id);
    auto roundtripped = roundtripWsmThroughSerialization(*member);
    ASSERT_EQ(WorkingSetMember::RID_AND_OBJ, roundtripped.getState());
    ASSERT_DOCUMENT_EQ(roundtripped.doc.value(), doc);
    ASSERT_EQ(roundtripped.doc.snapshotId().toNumber(), 42u);
    ASSERT_EQ(roundtripped.recordId.getLong(), 43);
    ASSERT_FALSE(roundtripped.metadata());
}

TEST_F(WorkingSetFixture, RecordIdStrAndObjStateCanRoundtripThroughSerialization) {
    Document doc{{"foo", Value{"bar"_sd}}};
    member->doc.setValue(doc);
    member->doc.setSnapshotId(SnapshotId{42u});
    const OID oid = OID::gen();

    member->recordId = record_id_helpers::keyForOID(oid);
    ws->transitionToRecordIdAndObj(id);
    auto roundtripped = roundtripWsmThroughSerialization(*member);
    ASSERT_EQ(WorkingSetMember::RID_AND_OBJ, roundtripped.getState());
    ASSERT_DOCUMENT_EQ(roundtripped.doc.value(), doc);
    ASSERT_EQ(roundtripped.doc.snapshotId().toNumber(), 42u);
    ASSERT_EQ(record_id_helpers::toBSONAs(roundtripped.recordId, "").firstElement().OID(), oid);
    ASSERT_FALSE(roundtripped.metadata());
}

TEST_F(WorkingSetFixture, OwnedObjStateCanRoundtripThroughSerialization) {
    Document doc{{"foo", Value{"bar"_sd}}};
    member->doc.setValue(doc);
    member->doc.setSnapshotId(SnapshotId{42u});
    ws->transitionToOwnedObj(id);
    auto roundtripped = roundtripWsmThroughSerialization(*member);
    ASSERT_EQ(WorkingSetMember::OWNED_OBJ, roundtripped.getState());
    ASSERT_DOCUMENT_EQ(roundtripped.doc.value(), doc);
    ASSERT_EQ(roundtripped.doc.snapshotId().toNumber(), 42u);
    ASSERT(roundtripped.recordId.isNull());
    ASSERT_FALSE(roundtripped.metadata());
}

TEST_F(WorkingSetFixture, RecordIdAndIdxStateCanRoundtripThroughSerialization) {
    member->recordId = RecordId{43};
    member->keyData.emplace_back(
        BSON("a" << 1 << "b" << 1), BSON("" << 3 << "" << 4), 8u, SnapshotId{11u});
    member->keyData.emplace_back(BSON("c" << -1), BSON("" << 5), 9u, SnapshotId{12u});
    ws->transitionToRecordIdAndIdx(id);

    auto roundtripped = roundtripWsmThroughSerialization(*member);
    ASSERT_EQ(WorkingSetMember::RID_AND_IDX, roundtripped.getState());
    ASSERT_EQ(roundtripped.recordId.getLong(), 43);
    ASSERT_EQ(roundtripped.keyData.size(), 2u);

    ASSERT_BSONOBJ_EQ(roundtripped.keyData[0].indexKeyPattern, BSON("a" << 1 << "b" << 1));
    ASSERT_BSONOBJ_EQ(roundtripped.keyData[0].keyData, BSON("" << 3 << "" << 4));
    ASSERT_EQ(roundtripped.keyData[0].indexId, 8u);
    ASSERT_EQ(roundtripped.keyData[0].snapshotId.toNumber(), 11u);

    ASSERT_BSONOBJ_EQ(roundtripped.keyData[1].indexKeyPattern, BSON("c" << -1));
    ASSERT_BSONOBJ_EQ(roundtripped.keyData[1].keyData, BSON("" << 5));
    ASSERT_EQ(roundtripped.keyData[1].indexId, 9u);
    ASSERT_EQ(roundtripped.keyData[1].snapshotId.toNumber(), 12u);

    ASSERT_FALSE(roundtripped.metadata());
}

TEST_F(WorkingSetFixture, WsmWithMetadataCanRoundtripThroughSerialization) {
    Document doc{{"foo", Value{"bar"_sd}}};
    member->doc.setValue(doc);
    member->metadata().setTextScore(42.0);
    member->metadata().setSearchScore(43.0);
    ws->transitionToOwnedObj(id);
    auto roundtripped = roundtripWsmThroughSerialization(*member);

    ASSERT_EQ(WorkingSetMember::OWNED_OBJ, roundtripped.getState());
    ASSERT_DOCUMENT_EQ(roundtripped.doc.value(), doc);
    ASSERT_FALSE(roundtripped.doc.value().metadata());
    ASSERT_TRUE(roundtripped.doc.snapshotId().isNull());
    ASSERT_TRUE(roundtripped.recordId.isNull());

    ASSERT_TRUE(roundtripped.metadata());
    ASSERT_TRUE(roundtripped.metadata().hasTextScore());
    ASSERT_EQ(roundtripped.metadata().getTextScore(), 42.0);
    ASSERT_TRUE(roundtripped.metadata().hasSearchScore());
    ASSERT_EQ(roundtripped.metadata().getSearchScore(), 43.0);
    ASSERT_FALSE(roundtripped.metadata().hasGeoNearPoint());
    ASSERT_FALSE(roundtripped.metadata().hasGeoNearDistance());
}

TEST_F(WorkingSetFixture, WsmCanBeExtractedAndReinserted) {
    Document doc{{"foo", Value{"bar"_sd}}};
    member->doc.setValue(doc);
    member->doc.setSnapshotId(SnapshotId{42u});
    member->recordId = RecordId{43};
    ws->transitionToRecordIdAndObj(id);
    member = nullptr;
    ASSERT_FALSE(ws->isFree(id));

    auto extractedWsm = ws->extract(id);
    ASSERT_TRUE(ws->isFree(id));
    ASSERT_TRUE(extractedWsm.hasObj());
    ASSERT_EQ(extractedWsm.getState(), WorkingSetMember::RID_AND_OBJ);
    ASSERT_DOCUMENT_EQ(extractedWsm.doc.value(), doc);
    ASSERT_EQ(extractedWsm.doc.snapshotId().toNumber(), 42u);
    ASSERT_EQ(extractedWsm.recordId.getLong(), 43);
    ASSERT_FALSE(extractedWsm.metadata());

    auto emplacedId = ws->emplace(std::move(extractedWsm));
    ASSERT_FALSE(ws->isFree(emplacedId));

    auto emplacedWsm = ws->get(emplacedId);
    ASSERT_TRUE(emplacedWsm->hasObj());
    ASSERT_EQ(emplacedWsm->getState(), WorkingSetMember::RID_AND_OBJ);
    ASSERT_DOCUMENT_EQ(emplacedWsm->doc.value(), doc);
    ASSERT_EQ(emplacedWsm->doc.snapshotId().toNumber(), 42u);
    ASSERT_EQ(emplacedWsm->recordId.getLong(), 43);
    ASSERT_FALSE(emplacedWsm->metadata());
}

}  // namespace mongo
