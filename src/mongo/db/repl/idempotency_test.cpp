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


#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/document_diff_test_helpers.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {
namespace {

class RandomizedIdempotencyTest : public IdempotencyTest {
protected:
    const int kDocId = 1;
    const BSONObj kDocIdQuery = BSON("_id" << kDocId);
    BSONObj canonicalizeDocumentForDataHash(const BSONObj& obj) override;

    BSONObj getDoc();

    std::string getStatesString(const std::vector<CollectionState>& state1,
                                const std::vector<CollectionState>& state2,
                                const std::vector<OplogEntry>& state1Ops,
                                const std::vector<OplogEntry>& state2Ops) override;

    Status resetState() override;

    void runUpdateV2IdempotencyTestCase();

    std::vector<OplogEntry> initOps;
    int64_t seed;
};

BSONObj canonicalizeBSONObjForDataHash(const BSONObj& obj);

BSONArray canonicalizeArrayForDataHash(const BSONObj& arr) {
    BSONArrayBuilder arrBuilder;
    for (auto&& elem : arr) {
        if (elem.type() == BSONType::array) {
            arrBuilder.append(canonicalizeArrayForDataHash(elem.embeddedObject()));
        } else if (elem.type() == BSONType::object) {
            arrBuilder.append(canonicalizeBSONObjForDataHash(elem.embeddedObject()));
        } else {
            arrBuilder.append(elem);
        }
    }
    return BSONArray(arrBuilder.obj());
}

BSONObj canonicalizeBSONObjForDataHash(const BSONObj& obj) {
    BSONObjBuilder objBuilder;
    BSONObjIteratorSorted iter(obj);
    while (iter.more()) {
        auto elem = iter.next();
        if (elem.isABSONObj()) {
            if (elem.type() == BSONType::array) {
                objBuilder.append(elem.fieldName(),
                                  canonicalizeArrayForDataHash(elem.embeddedObject()));
            } else {
                // If it is a sub object, we'll have to sort it as well before we append it.
                auto sortedObj = canonicalizeBSONObjForDataHash(elem.Obj());
                objBuilder.append(elem.fieldName(), sortedObj);
            }
        } else {
            // If it is not a sub object, just append it and move on.
            objBuilder.append(elem);
        }
    }
    return objBuilder.obj();
}

BSONObj RandomizedIdempotencyTest::canonicalizeDocumentForDataHash(const BSONObj& obj) {
    return canonicalizeBSONObjForDataHash(obj);
}
BSONObj RandomizedIdempotencyTest::getDoc() {
    auto coll = acquireCollection(
        _opCtx.get(),
        CollectionAcquisitionRequest(_nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(_opCtx.get()),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
    BSONObj doc;
    Helpers::findById(_opCtx.get(), _nss, kDocIdQuery, doc);
    return doc.getOwned();
}

std::string RandomizedIdempotencyTest::getStatesString(const std::vector<CollectionState>& state1,
                                                       const std::vector<CollectionState>& state2,
                                                       const std::vector<OplogEntry>& state1Ops,
                                                       const std::vector<OplogEntry>& state2Ops) {
    LOGV2(21157,
          "{IdempotencyTest_getStatesString_state1_state2_ops}",
          "IdempotencyTest_getStatesString_state1_state2_ops"_attr =
              IdempotencyTest::getStatesString(state1, state2, state1Ops, state2Ops));
    StringBuilder sb;
    sb << "Ran update ops: ";
    sb << "[ ";
    bool firstIter = true;
    for (const auto& op : state2Ops) {
        if (!firstIter) {
            sb << ", ";
        } else {
            firstIter = false;
        }
        sb << op.toStringForLogging();
    }
    sb << " ]\n";

    ASSERT_OK(resetState());
    ASSERT_OK(runOpsInitialSync(state1Ops));
    sb << "Document at the end of state1: " << getDoc() << "\n";

    ASSERT_OK(resetState());

    sb << "Start document for state2: " << getDoc() << "\n";
    for (const auto& op : state2Ops) {
        ASSERT_OK(runOpInitialSync(op));
        sb << "Apply: " << op.getObject() << "\n  ==> " << getDoc() << "\n";
    }

    sb << "Found from the seed: " << this->seed;

    return sb.str();
}

Status RandomizedIdempotencyTest::resetState() {
    // Remove the collection without a drop OpTime. As we are re-applying the initOps the create
    // OpTime would be less than the previous drop. By removing the collection without an OpTime we
    // completely clear the history for the namespace.
    {
        Lock::GlobalLock lk{_opCtx.get(), MODE_IX};
        CollectionCatalog::write(_opCtx.get(), [&](CollectionCatalog& catalog) {
            // Lookup the UUID from the namespace, it will not exist if we're 'resetting' the state
            // before creating it.
            auto uuid = catalog.lookupUUIDByNSS(_opCtx.get(), _nss);
            if (uuid) {
                catalog.deregisterCollection(_opCtx.get(), *uuid, /*commitTime=*/boost::none);
            }
        });
    }
    return runOpsInitialSync(initOps);
}

void RandomizedIdempotencyTest::runUpdateV2IdempotencyTestCase() {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    this->seed = SecureRandom().nextInt64();
    PseudoRandom seedGenerator(this->seed);
    std::set<StringData> fields{"f00", "f10", "f01", "f11", "f02", "f20"};

    auto generateDocWithId = [&seedGenerator](int id) {
        MutableDocument doc;
        doc.addField("_id", Value(id));
        PseudoRandom rng(seedGenerator.nextInt64());
        return doc_diff::generateDoc(&rng, &doc, 0);
    };

    PseudoRandom rng(seedGenerator.nextInt64());
    for (auto simulation = 0; simulation < 10; ++simulation) {
        // Initialize the collection with a single document, which would later be updated.
        auto inputObj = generateDocWithId(kDocId);
        this->initOps = std::vector<OplogEntry>{createCollection(), insert(inputObj)};
        ASSERT_OK(resetState());
        ASSERT_BSONOBJ_BINARY_EQ(inputObj, getDoc());

        auto oldDoc = inputObj;
        const size_t kUpdateSequenceLength = 15;
        std::vector<OplogEntry> updateSequence;
        for (size_t i = 0; i < kUpdateSequenceLength; i++) {
            BSONObj oplogDiff;
            boost::optional<BSONObj> generatedDoc;
            // With delta based updates, we cannot just generate any random diff since certain diffs
            // when applied to an unrelated object (which would never have produced by computing the
            // input objects) would break idempotency. So we do a dry run of what the collection
            // state would look like and compute diffs based on that.
            generatedDoc = generateDocWithId(kDocId);
            auto diffOutput = doc_diff::computeOplogDiff(
                oldDoc, *generatedDoc, update_oplog_entry::kSizeOfDeltaOplogEntryMetadata);
            ASSERT(diffOutput);
            if (diffOutput->isEmpty()) {
                // The computeOplogDiff function returns an empty diff object when the size of the
                // computed diff is larger than the 'post' image. Retrying because the diff field
                // can't be empty in an update oplog.
                LOGV2(9873500, "Retrying because the diff field can't be empty.");
                i--;
                continue;
            }
            oplogDiff = BSON("$v" << 2 << "diff" << *diffOutput);
            auto op = update(kDocId, oplogDiff);
            ASSERT_OK(runOpInitialSync(op));
            if (generatedDoc) {
                ASSERT_BSONOBJ_BINARY_EQ(*generatedDoc, getDoc());
            }
            oldDoc = getDoc();
            updateSequence.push_back(std::move(op));
        }
        testOpsAreIdempotent(updateSequence, SequenceType::kAnyPrefixOrSuffix);
    }
}

TEST_F(RandomizedIdempotencyTest, CheckUpdateSequencesAreIdempotentV2) {
    runUpdateV2IdempotencyTestCase();
    runUpdateV2IdempotencyTestCase();
    runUpdateV2IdempotencyTestCase();
    runUpdateV2IdempotencyTestCase();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
