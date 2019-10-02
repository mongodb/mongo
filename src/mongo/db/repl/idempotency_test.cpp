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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/idempotency_document_structure.h"
#include "mongo/db/repl/idempotency_test_fixture.h"
#include "mongo/db/repl/idempotency_update_sequence.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

class RandomizedIdempotencyTest : public IdempotencyTest {
protected:
    const int kDocId = 1;
    const BSONObj kDocIdQuery = BSON("_id" << kDocId);

    std::vector<OplogEntry> createUpdateSequence(const UpdateSequenceGenerator& generator,
                                                 const size_t length);

    BSONObj canonicalizeDocumentForDataHash(const BSONObj& obj) override;

    BSONObj getDoc();

    std::string getStatesString(const std::vector<CollectionState>& state1,
                                const std::vector<CollectionState>& state2,
                                const MultiApplier::OperationPtrs& opPtrs) override;

    Status resetState() override;

    void runIdempotencyTestCase();

    std::vector<OplogEntry> initOps;
    int64_t seed;
};

BSONObj RandomizedIdempotencyTest::canonicalizeDocumentForDataHash(const BSONObj& obj) {
    BSONObjBuilder objBuilder;
    BSONObjIteratorSorted iter(obj);
    while (iter.more()) {
        auto elem = iter.next();
        if (elem.isABSONObj()) {
            if (elem.type() == mongo::Array) {
                objBuilder.append(elem.fieldName(), obj);
            } else {
                // If it is a sub object, we'll have to sort it as well before we append it.
                auto sortedObj = canonicalizeDocumentForDataHash(elem.Obj());
                objBuilder.append(elem.fieldName(), sortedObj);
            }
        } else {
            // If it is not a sub object, just append it and move on.
            objBuilder.append(elem);
        }
    }

    return objBuilder.obj();
}

BSONObj RandomizedIdempotencyTest::getDoc() {
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);
    BSONObj doc;
    Helpers::findById(_opCtx.get(), autoColl.getDb(), nss.ns(), kDocIdQuery, doc);
    return doc.getOwned();
}

std::vector<OplogEntry> RandomizedIdempotencyTest::createUpdateSequence(
    const UpdateSequenceGenerator& generator, const size_t length) {
    // for each document enumerated & inserted generate a sequence of updates to apply to it.
    std::vector<OplogEntry> updateSequence;
    updateSequence.reserve(length);
    for (size_t i = 0; i < length; i++) {
        updateSequence.push_back(update(kDocId, generator.generateUpdate()));
    }

    return updateSequence;
}

std::string RandomizedIdempotencyTest::getStatesString(const std::vector<CollectionState>& state1,
                                                       const std::vector<CollectionState>& state2,
                                                       const MultiApplier::OperationPtrs& opPtrs) {
    unittest::log() << IdempotencyTest::getStatesString(state1, state2, opPtrs);
    StringBuilder sb;
    sb << "Ran update ops: ";
    sb << "[ ";
    bool firstIter = true;
    for (auto op : opPtrs) {
        if (!firstIter) {
            sb << ", ";
        } else {
            firstIter = false;
        }
        sb << op->toString();
    }
    sb << " ]\n";

    ASSERT_OK(resetState());

    sb << "Start: " << getDoc() << "\n";
    for (auto op : opPtrs) {
        ASSERT_OK(runOpInitialSync(*op));
        sb << "Apply: " << op->getObject() << "\n  ==> " << getDoc() << "\n";
    }

    sb << "Found from the seed: " << this->seed;

    return sb.str();
}

Status RandomizedIdempotencyTest::resetState() {
    Status dropStatus = runOpInitialSync(dropCollection());
    if (!dropStatus.isOK()) {
        return dropStatus;
    }

    return runOpsInitialSync(initOps);
}

void RandomizedIdempotencyTest::runIdempotencyTestCase() {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    std::set<StringData> fields{"a", "b"};
    size_t depth = 1;
    size_t length = 1;

    // Eliminate the chance of a sub array, because they cause theoretically valid sequences that
    // cause idempotency issues.
    const double kScalarProbability = 0.375;
    const double kDocProbability = 0.375;
    const double kArrProbability = 0.0;

    this->seed = SecureRandom().nextInt64();
    PseudoRandom seedGenerator(this->seed);
    RandomizedScalarGenerator scalarGenerator{PseudoRandom(seedGenerator.nextInt64())};
    UpdateSequenceGenerator updateGenerator(
        {fields, depth, length, kScalarProbability, kDocProbability, kArrProbability},
        PseudoRandom{seedGenerator.nextInt64()},
        &scalarGenerator);

    const bool kSkipDocs = kDocProbability == 0.0;
    const bool kSkipArrs = kArrProbability == 0.0;
    DocumentStructureEnumerator enumerator({fields, depth, length, kSkipDocs, kSkipArrs},
                                           &scalarGenerator);

    const size_t kUpdateSequenceLength = 5;
    // For the sake of keeping the speed of iteration sane and feasible.
    const size_t kNumUpdateSequencesPerDoc = 2;

    for (auto doc : enumerator) {
        BSONObj docWithId = (BSONObjBuilder(doc) << "_id" << kDocId).obj();
        for (size_t i = 0; i < kNumUpdateSequencesPerDoc; i++) {
            this->initOps = std::vector<OplogEntry>{createCollection(), insert(docWithId)};
            std::vector<OplogEntry> updateSequence =
                createUpdateSequence(updateGenerator, kUpdateSequenceLength);
            testOpsAreIdempotent(updateSequence, SequenceType::kAnyPrefixOrSuffix);
        }
    }
}

TEST_F(RandomizedIdempotencyTest, CheckUpdateSequencesAreIdempotent) {
    runIdempotencyTestCase();
}

TEST_F(IdempotencyTest, UpdateTwoFields) {
    ASSERT_OK(
        ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT_OK(runOpInitialSync(createCollection(kUuid)));
    ASSERT_OK(runOpInitialSync(insert(fromjson("{_id: 1, y: [0]}"))));

    auto updateOp1 = update(1, fromjson("{$set: {x: 1}}"));
    auto updateOp2 = update(1, fromjson("{$set: {x: 2, 'y.0': 2}}"));
    auto updateOp3 = update(1, fromjson("{$set: {y: 3}}"));

    auto ops = {updateOp1, updateOp2, updateOp3};
    testOpsAreIdempotent(ops);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
