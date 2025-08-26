/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/write_ops_retryability.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <ostream>
#include <utility>
#include <variant>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using OplogBatch = ReshardingOplogBatchPreparer::OplogBatchToPrepare;

class ReshardingOplogBatchPreparerTest : public unittest::Test {
protected:
    repl::OplogEntry makeUpdateOp(BSONObj document) {
        return makeUpdateOp(std::move(document), boost::none, boost::none);
    }

    repl::OplogEntry makeUpdateOp(BSONObj document,
                                  boost::optional<LogicalSessionId> lsid,
                                  boost::optional<TxnNumber> txnNumber) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kUpdate);
        op.setObject2(document["_id"].wrap().getOwned());
        op.setObject(std::move(document));
        op.setSessionId(std::move(lsid));
        op.setTxnNumber(std::move(txnNumber));

        // These are unused by ReshardingOplogBatchPreparer but required by IDL parsing.
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    /**
     * Returns an applyOps oplog entry containing insert operations for the given documents. If the
     * session is an internal session for retryable writes, uses the "_id" of each document as its
     * statement id.
     */
    repl::OplogEntry makeApplyOpsForInsert(
        const std::vector<BSONObj> documents,
        boost::optional<LogicalSessionId> lsid = boost::none,
        boost::optional<TxnNumber> txnNumber = boost::none,
        boost::optional<bool> isPrepare = boost::none,
        boost::optional<bool> isPartial = boost::none,
        boost::optional<repl::MultiOplogEntryType> multiOplogEntryType = boost::none,
        bool useStmtIds = false) {
        std::vector<repl::DurableReplOperation> ops;
        for (const auto& document : documents) {
            auto insertOp = repl::DurableReplOperation(repl::OpTypeEnum::kInsert, {}, document);
            if ((lsid && isInternalSessionForRetryableWrite(*lsid)) || useStmtIds) {
                if (document.hasField("_id")) {
                    auto id = document.getIntField("_id");
                    insertOp.setStatementIds({id});
                }
            }
            ops.emplace_back(insertOp);
        }

        return makeApplyOpsOplogEntry(
            ops, lsid, txnNumber, isPrepare, isPartial, multiOplogEntryType);
    }

    repl::OplogEntry makeApplyOpsOplogEntry(
        std::vector<repl::DurableReplOperation> ops,
        boost::optional<LogicalSessionId> lsid = boost::none,
        boost::optional<TxnNumber> txnNumber = boost::none,
        boost::optional<bool> isPrepare = boost::none,
        boost::optional<bool> isPartial = boost::none,
        boost::optional<repl::MultiOplogEntryType> multiOplogEntryType = boost::none) {
        BSONObjBuilder applyOpsBuilder;

        BSONArrayBuilder opsArrayBuilder = applyOpsBuilder.subarrayStart("applyOps");
        for (const auto& op : ops) {
            opsArrayBuilder.append(op.toBSON());
        }
        opsArrayBuilder.done();

        if (isPrepare) {
            invariant(lsid);
            invariant(txnNumber);
            applyOpsBuilder.append(repl::ApplyOpsCommandInfoBase::kPrepareFieldName, *isPrepare);
        }

        if (isPartial) {
            invariant(lsid);
            invariant(txnNumber);
            applyOpsBuilder.append(repl::ApplyOpsCommandInfoBase::kPartialTxnFieldName, *isPartial);
        }

        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kCommand);
        op.setObject(applyOpsBuilder.obj());
        op.setSessionId(std::move(lsid));
        op.setTxnNumber(std::move(txnNumber));

        // These are unused by ReshardingOplogBatchPreparer but required by IDL parsing.
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        if (multiOplogEntryType) {
            op.setMultiOpType(*multiOplogEntryType);
        }

        return {op.toBSON()};
    }

    repl::OplogEntry makeCommandOp(BSONObj commandObj) {
        return makeCommandOp(std::move(commandObj), boost::none, boost::none);
    }

    repl::OplogEntry makeCommandOp(BSONObj commandObj,
                                   boost::optional<LogicalSessionId> lsid,
                                   boost::optional<TxnNumber> txnNumber) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kCommand);
        op.setObject(std::move(commandObj));
        op.setSessionId(std::move(lsid));
        op.setTxnNumber(std::move(txnNumber));

        // These are unused by ReshardingOplogBatchPreparer but required by IDL parsing.
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    repl::OplogEntry makeGenericNoopOplogEntry(const boost::optional<LogicalSessionId>& lsid,
                                               const boost::optional<TxnNumber>& txnNumber) {
        repl::MutableOplogEntry op;
        op.setSessionId(lsid);
        op.setTxnNumber(txnNumber);
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});
        return {op.toBSON()};
    }

    repl::OplogEntry makeProgressMarkNoopOplogEntry(bool createdAfterOplogApplicationStarted) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});

        ReshardProgressMarkO2Field o2Field;
        o2Field.setType(resharding::kReshardProgressMarkOpLogType);
        if (createdAfterOplogApplicationStarted) {
            o2Field.setCreatedAfterOplogApplicationStarted(true);
        }
        op.setObject2(o2Field.toBSON());
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    repl::OplogEntry makeFinalNoopOplogEntry() {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});

        ReshardBlockingWritesChangeEventO2Field o2Field;
        o2Field.setType(resharding::kReshardFinalOpLogType);
        o2Field.setReshardBlockingWrites({});
        o2Field.setReshardingUUID(UUID::gen());
        op.setObject2(o2Field.toBSON());

        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    ReshardingOplogBatchPreparer::OplogBatchToApply getNonEmptyWriterVector(
        ReshardingOplogBatchPreparer::WriterVectors writerVectors) {
        ReshardingOplogBatchPreparer::OplogBatchToApply nonempty;

        for (const auto& writer : writerVectors) {
            if (!writer.empty()) {
                ASSERT_TRUE(nonempty.empty())
                    << "Expected only one non-empty writer vector, but found multiple";
                nonempty = writer;
            }
        }

        ASSERT_FALSE(nonempty.empty()) << "Expected to find a non-empty writer vector, but didn't";
        return nonempty;
    }

    ReshardingOplogBatchPreparer _batchPreparer{kNumWriterVectors, nullptr};

    static constexpr size_t kNumWriterVectors = 2;
};

TEST_F(ReshardingOplogBatchPreparerTest, AssignsCrudOpsToWriterVectorsById) {
    OplogBatch batch;

    int numOps = 10;
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << 0 << "n" << i)));
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), 0U);

    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), numOps);
    for (int i = 0; i < numOps; ++i) {
        ASSERT_BSONOBJ_BINARY_EQ(writer[i]->getObject(), BSON("_id" << 0 << "n" << i));
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, DistributesCrudOpsToWriterVectorsFairly) {
    OplogBatch batch;

    int numOps = 100;
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << i)));
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), 0U);

    // Use `numOps / 5` as a generous definition for "fair". There's no guarantee for how the _id
    // values will be hashed but can at least assert the writer vector sizes won't wildly differ
    // from each other.
    ASSERT_GTE(writerVectors[0].size(), numOps / 5);
    ASSERT_GTE(writerVectors[1].size(), numOps / 5);
    ASSERT_EQ(writerVectors[0].size() + writerVectors[1].size(), numOps);
}

TEST_F(ReshardingOplogBatchPreparerTest, CreatesDerivedCrudOpsForApplyOps) {
    OplogBatch batch;

    // We use the "fromApplyOps" field in the document to distinguish between the regular oplog
    // entries from the derived ones later on.
    int numOps = 20;
    std::vector<BSONObj> docsForApplyOps;
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << i << "fromApplyOps" << false)));
        docsForApplyOps.push_back(BSON("_id" << i << "fromApplyOps" << true));
    }

    batch.emplace_back(makeApplyOpsForInsert(docsForApplyOps));

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), numOps);

    ASSERT_EQ(writerVectors[0].size() + writerVectors[1].size(), numOps * 2);

    for (const auto& writer : writerVectors) {
        for (size_t i = 0; i < writer.size(); ++i) {
            if (writer[i]->getObject()["fromApplyOps"].Bool()) {
                continue;
            }

            int docId = writer[i]->getObject()["_id"].Int();

            bool found = false;
            for (size_t j = i + 1; j < writer.size(); ++j) {
                if (writer[j]->getObject()["fromApplyOps"].Bool() &&
                    writer[j]->getObject()["_id"].Int() == docId) {
                    found = true;
                }
            }
            ASSERT_TRUE(found) << "Expected to find normal op and unrolled applyOps for _id="
                               << docId << " in same writer vector, but didn't";
        }
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, InterleavesDerivedCrudOpsForApplyOps) {
    OplogBatch batch;

    int numOps = 10;
    for (int i = 0; i < numOps; ++i) {
        // We use the "fromApplyOps" field in the document to distinguish between the regular oplog
        // entries from the derived ones later on.
        if (i % 2 == 0) {
            batch.emplace_back(
                makeUpdateOp(BSON("_id" << 0 << "n" << i << "fromApplyOps" << false)));
        } else {
            // We use OpTypeEnum::kInsert rather than OpTypeEnum::kUpdate here to avoid needing to
            // deal with setting the 'o2' field.
            batch.emplace_back(
                makeApplyOpsForInsert({BSON("_id" << 0 << "n" << i << "fromApplyOps" << true)}));
        }
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), numOps / 2);

    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), numOps);
    for (int i = 0; i < numOps; ++i) {
        if (i % 2 == 0) {
            ASSERT_BSONOBJ_BINARY_EQ(writer[i]->getObject(),
                                     BSON("_id" << 0 << "n" << i << "fromApplyOps" << false));
        } else {
            ASSERT_BSONOBJ_BINARY_EQ(writer[i]->getObject(),
                                     BSON("_id" << 0 << "n" << i << "fromApplyOps" << true));
        }
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, AssignsSessionOpsToWriterVectorsByLsid) {
    OplogBatch batch;
    auto lsid = makeLogicalSessionIdForTest();

    int numOps = 10;
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << i), lsid, TxnNumber{1}));
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), 0U);

    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), numOps);
    for (int i = 0; i < numOps; ++i) {
        ASSERT_BSONOBJ_BINARY_EQ(writer[i]->getObject(), BSON("_id" << i));
        ASSERT_EQ(writer[i]->getSessionId(), lsid);
        ASSERT_EQ(writer[i]->getTxnNumber(), TxnNumber{1});
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, DiscardsLowerTxnNumberSessionOps) {
    OplogBatch batch;
    auto lsid = makeLogicalSessionIdForTest();

    int numOps = 5;
    for (int i = 1; i <= numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << i), lsid, TxnNumber{i}));
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), 0U);

    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), 1U);
    ASSERT_BSONOBJ_BINARY_EQ(writer[0]->getObject(), BSON("_id" << numOps));
    ASSERT_EQ(writer[0]->getSessionId(), lsid);
    ASSERT_EQ(writer[0]->getTxnNumber(), TxnNumber{numOps});
}

TEST_F(ReshardingOplogBatchPreparerTest, DistributesSessionOpsToWriterVectorsFairly) {
    OplogBatch batch;

    int numOps = 100;
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(
            makeUpdateOp(BSON("_id" << i), makeLogicalSessionIdForTest(), TxnNumber{1}));
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);

    // Use `numOps / 5` as a generous definition for "fair". There's no guarantee for how the lsid
    // values will be hashed but can at least assert the writer vector sizes won't wildly differ
    // from each other.
    ASSERT_GTE(writerVectors[0].size(), numOps / 5);
    ASSERT_GTE(writerVectors[1].size(), numOps / 5);
    ASSERT_EQ(writerVectors[0].size() + writerVectors[1].size(), numOps);
}

TEST_F(ReshardingOplogBatchPreparerTest, ThrowsForUnsupportedCommandOps) {
    {
        OplogBatch batch;
        batch.emplace_back(makeCommandOp(BSON("drop" << 1)));

        std::list<repl::OplogEntry> derivedOps;
        ASSERT_THROWS_CODE(_batchPreparer.makeCrudOpWriterVectors(batch, derivedOps),
                           DBException,
                           ErrorCodes::OplogOperationUnsupported);
    }

    {
        OplogBatch batch;
        batch.emplace_back(makeCommandOp(BSON("commitIndexBuild" << 1)));

        std::list<repl::OplogEntry> derivedOps;
        ASSERT_THROWS_CODE(_batchPreparer.makeSessionOpWriterVectors(batch, derivedOps),
                           DBException,
                           ErrorCodes::OplogOperationUnsupported);
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, DiscardsGenericNoops) {
    auto runTest = [&](const boost::optional<LogicalSessionId>& lsid,
                       const boost::optional<TxnNumber>& txnNumber) {
        OplogBatch batch;

        int numOps = 5;
        for (int i = 0; i < numOps; ++i) {
            auto op = makeGenericNoopOplogEntry(lsid, txnNumber);
            batch.emplace_back(std::move(op));
        }

        std::list<repl::OplogEntry> derivedOpsForCrudWriters;
        auto writerVectors =
            _batchPreparer.makeCrudOpWriterVectors(batch, derivedOpsForCrudWriters);
        ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
        ASSERT_EQ(derivedOpsForCrudWriters.size(), 0U);
        ASSERT_EQ(writerVectors[0].size(), 0U);
        ASSERT_EQ(writerVectors[1].size(), 0U);

        std::list<repl::OplogEntry> derivedOpsForSessionWriters;
        writerVectors =
            _batchPreparer.makeSessionOpWriterVectors(batch, derivedOpsForSessionWriters);
        ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
        ASSERT_EQ(derivedOpsForSessionWriters.size(), 0U);
        ASSERT_EQ(writerVectors[0].size(), 0U);
        ASSERT_EQ(writerVectors[1].size(), 0U);
    };

    runTest(boost::none, boost::none);

    TxnNumber txnNumber{1};
    runTest(makeLogicalSessionIdForTest(), txnNumber);
    runTest(makeLogicalSessionIdWithTxnUUIDForTest(), txnNumber);
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest(), txnNumber);
}

TEST_F(ReshardingOplogBatchPreparerTest,
       DiscardsProgressMarkOplogCreatedBeforeOplogApplicationStarted) {
    OplogBatch batch;

    auto op = makeProgressMarkNoopOplogEntry(false /* createdAfterOplogApplicationStarted */);
    batch.emplace_back(std::move(op));

    std::list<repl::OplogEntry> derivedOpsForCrudWriters;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOpsForCrudWriters);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForCrudWriters.size(), 0U);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);

    std::list<repl::OplogEntry> derivedOpsForSessionWriters;
    writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOpsForSessionWriters);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForSessionWriters.size(), 0U);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);
}

TEST_F(ReshardingOplogBatchPreparerTest,
       CrudWriterDoesNotDiscardProgressMarkOplogCreatedAfterOplogApplicationStarted) {
    OplogBatch batch;

    auto op = makeProgressMarkNoopOplogEntry(true /* createdAfterOplogApplicationStarted */);
    batch.emplace_back(std::move(op));

    std::list<repl::OplogEntry> derivedOpsForCrudWriters;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOpsForCrudWriters);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForCrudWriters.size(), 0U);
    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), 1U);
    ASSERT(resharding::isProgressMarkOplogAfterOplogApplicationStarted(*writer[0]));

    // The 'reshardProgressMark' oplog entry should not get added to session writers.
    std::list<repl::OplogEntry> derivedOpsForSessionWriters;
    writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOpsForSessionWriters);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForSessionWriters.size(), 0U);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);
}

TEST_F(ReshardingOplogBatchPreparerTest, DiscardsFinalOplog) {
    OplogBatch batch;

    auto op = makeFinalNoopOplogEntry();
    batch.emplace_back(std::move(op));

    std::list<repl::OplogEntry> derivedOpsForCrudWriters;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOpsForCrudWriters);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForCrudWriters.size(), 0U);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);

    std::list<repl::OplogEntry> derivedOpsForSessionWriters;
    writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOpsForSessionWriters);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForSessionWriters.size(), 0U);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);
}

TEST_F(ReshardingOplogBatchPreparerTest,
       SessionWriterDoesNotDiscardWouldChangeOwningShardNoopForRetryableInternalTransaction) {

    const auto lsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    const TxnNumber txnNumber{1};

    OplogBatch batch;

    auto op =
        repl::DurableReplOperation(repl::OpTypeEnum::kNoop, {}, kWouldChangeOwningShardSentinel);
    op.setObject2(BSONObj());
    op.setStatementIds({0});
    batch.emplace_back(makeApplyOpsOplogEntry(
        {op}, lsid, txnNumber, false /* isPrepare */, false /* isPartial */));

    std::list<repl::OplogEntry> derivedOpsForCrudWriters;
    auto crudWriterVectors =
        _batchPreparer.makeCrudOpWriterVectors(batch, derivedOpsForCrudWriters);
    ASSERT_EQ(crudWriterVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOpsForCrudWriters.size(), 0U);
    ASSERT_EQ(crudWriterVectors[0].size(), 0U);
    ASSERT_EQ(crudWriterVectors[1].size(), 0U);

    std::list<repl::OplogEntry> derivedOpsForSessionWriters;
    auto sessionWriterVectors =
        _batchPreparer.makeSessionOpWriterVectors(batch, derivedOpsForSessionWriters);
    ASSERT_EQ(sessionWriterVectors.size(), kNumWriterVectors);
    auto writer = getNonEmptyWriterVector(sessionWriterVectors);
    ASSERT_EQ(writer.size(), 1U);
    ASSERT_EQ(derivedOpsForSessionWriters.size(), 1U);
    ASSERT_EQ(writer[0]->getSessionId(), *getParentSessionId(lsid));
    ASSERT_EQ(*writer[0]->getTxnNumber(), *lsid.getTxnNumber());
    ASSERT(isWouldChangeOwningShardSentinelOplogEntry(*writer[0]));
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForApplyOpsWithoutTxnNumber) {
    OplogBatch batch;
    batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0)}));

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(derivedOps.size(), 0U);

    for (const auto& writer : writerVectors) {
        ASSERT_TRUE(writer.empty());
    }
}


TEST_F(ReshardingOplogBatchPreparerTest,
       SessionWriteVectorsDeriveCrudOpsForApplyOpsForRetryableInternalTransaction) {
    const auto lsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    const TxnNumber txnNumber{1};

    OplogBatch batch;
    // 'makeApplyOpsForInsert' uses the "_id" of each document as the "stmtId" for its insert
    // operation. The insert operation without a stmtId should not have a derived operation.
    batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0), BSONObj(), BSON("_id" << 1)},
                                             lsid,
                                             txnNumber,
                                             false /* isPrepare */,
                                             false /* isPartial */));

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
    ASSERT_FALSE(writerVectors.empty());

    auto writer = getNonEmptyWriterVector(writerVectors);

    ASSERT_EQ(writer.size(), 2U);
    ASSERT_EQ(derivedOps.size(), 2U);
    for (size_t i = 0; i < writer.size(); ++i) {
        ASSERT_EQ(writer[i]->getSessionId(), *getParentSessionId(lsid));
        ASSERT_EQ(*writer[i]->getTxnNumber(), *lsid.getTxnNumber());
        ASSERT(writer[i]->getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT_BSONOBJ_EQ(writer[i]->getObject(), (BSON("_id" << static_cast<int>(i))));
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForSmallUnpreparedTxn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        const TxnNumber txnNumber{1};

        OplogBatch batch;
        batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0), BSON("_id" << 1)},
                                                 lsid,
                                                 txnNumber,
                                                 false /* isPrepare */,
                                                 false /* isPartial */));

        std::list<repl::OplogEntry> derivedOps;
        auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
        ASSERT_FALSE(writerVectors.empty());

        auto writer = getNonEmptyWriterVector(writerVectors);

        if (isInternalSessionForRetryableWrite(lsid)) {
            ASSERT_EQ(writer.size(), 2U);
            ASSERT_EQ(derivedOps.size(), 2U);
            for (size_t i = 0; i < writer.size(); ++i) {
                ASSERT_EQ(writer[i]->getSessionId(), *getParentSessionId(lsid));
                ASSERT_EQ(*writer[i]->getTxnNumber(), *lsid.getTxnNumber());
                ASSERT(writer[i]->getOpType() == repl::OpTypeEnum::kInsert);
                ASSERT_BSONOBJ_EQ(writer[i]->getObject(), (BSON("_id" << static_cast<int>(i))));
            }
        } else {
            ASSERT_EQ(writer.size(), 1U);
            ASSERT_EQ(derivedOps.size(), 0U);
            ASSERT_EQ(writer[0]->getSessionId(), lsid);
            ASSERT_EQ(*writer[0]->getTxnNumber(), txnNumber);
            ASSERT(writer[0]->getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        }
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForLargeUnpreparedTxn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        const TxnNumber txnNumber{1};

        OplogBatch batch;
        batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0), BSON("_id" << 1)},
                                                 lsid,
                                                 txnNumber,
                                                 false /* isPrepare */,
                                                 true /* isPartial */));
        batch.emplace_back(makeApplyOpsForInsert(
            {BSON("_id" << 2)}, lsid, txnNumber, false /* isPrepare */, false /* isPartial */));

        std::list<repl::OplogEntry> derivedOps;
        auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
        ASSERT_FALSE(writerVectors.empty());

        auto writer = getNonEmptyWriterVector(writerVectors);

        if (isInternalSessionForRetryableWrite(lsid)) {
            ASSERT_EQ(writer.size(), 3U);
            ASSERT_EQ(derivedOps.size(), 3U);
            for (size_t i = 0; i < writer.size(); ++i) {
                ASSERT_EQ(writer[i]->getSessionId(), *getParentSessionId(lsid));
                ASSERT_EQ(*writer[i]->getTxnNumber(), *lsid.getTxnNumber());
                ASSERT(writer[i]->getOpType() == repl::OpTypeEnum::kInsert);
                ASSERT_BSONOBJ_EQ(writer[i]->getObject(), (BSON("_id" << static_cast<int>(i))));
            }
        } else {
            ASSERT_EQ(writer.size(), 1U);
            ASSERT_EQ(derivedOps.size(), 0U);
            ASSERT_EQ(writer[0]->getSessionId(), lsid);
            ASSERT_EQ(*writer[0]->getTxnNumber(), txnNumber);
            ASSERT(writer[0]->getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        }
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForSmallCommittedPreparedTxn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        const TxnNumber txnNumber{1};

        OplogBatch batch;
        batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0), BSON("_id" << 1)},
                                                 lsid,
                                                 txnNumber,
                                                 true /* isPrepare */,
                                                 false /* isPartial */));
        batch.emplace_back(makeCommandOp(BSON("commitTransaction" << 1), lsid, txnNumber));

        std::list<repl::OplogEntry> derivedOps;
        auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
        ASSERT_FALSE(writerVectors.empty());

        auto writer = getNonEmptyWriterVector(writerVectors);

        if (isInternalSessionForRetryableWrite(lsid)) {
            ASSERT_EQ(writer.size(), 2U);
            ASSERT_EQ(derivedOps.size(), 2U);
            for (size_t i = 0; i < writer.size(); ++i) {
                ASSERT_EQ(writer[i]->getSessionId(), *getParentSessionId(lsid));
                ASSERT_EQ(*writer[i]->getTxnNumber(), *lsid.getTxnNumber());
                ASSERT(writer[i]->getOpType() == repl::OpTypeEnum::kInsert);
                ASSERT_BSONOBJ_EQ(writer[i]->getObject(), (BSON("_id" << static_cast<int>(i))));
            }
        } else {
            ASSERT_EQ(writer.size(), 1U);
            ASSERT_EQ(derivedOps.size(), 0U);
            ASSERT_EQ(writer[0]->getSessionId(), lsid);
            ASSERT_EQ(*writer[0]->getTxnNumber(), txnNumber);
            ASSERT(writer[0]->getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        }
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForLargeCommittedPreparedTxn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        const TxnNumber txnNumber{1};

        OplogBatch batch;
        batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0), BSON("_id" << 1)},
                                                 lsid,
                                                 txnNumber,
                                                 false /* isPrepare */,
                                                 true /* isPartial */));
        batch.emplace_back(makeApplyOpsForInsert(
            {BSON("_id" << 2)}, lsid, txnNumber, true /* isPrepare */, false /* isPartial */));
        batch.emplace_back(makeCommandOp(BSON("commitTransaction" << 1), lsid, txnNumber));

        std::list<repl::OplogEntry> derivedOps;
        auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
        ASSERT_FALSE(writerVectors.empty());

        auto writer = getNonEmptyWriterVector(writerVectors);

        if (isInternalSessionForRetryableWrite(lsid)) {
            ASSERT_EQ(writer.size(), 3U);
            ASSERT_EQ(derivedOps.size(), 3U);
            for (size_t i = 0; i < writer.size(); ++i) {
                ASSERT_EQ(writer[i]->getSessionId(), *getParentSessionId(lsid));
                ASSERT_EQ(*writer[i]->getTxnNumber(), *lsid.getTxnNumber());
                ASSERT(writer[i]->getOpType() == repl::OpTypeEnum::kInsert);
                ASSERT_BSONOBJ_EQ(writer[i]->getObject(), (BSON("_id" << static_cast<int>(i))));
            }
        } else {
            ASSERT_EQ(writer.size(), 1U);
            ASSERT_EQ(derivedOps.size(), 0U);
            ASSERT_EQ(writer[0]->getSessionId(), lsid);
            ASSERT_EQ(*writer[0]->getTxnNumber(), txnNumber);
            ASSERT(writer[0]->getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        }
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForAbortedPreparedTxn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        const TxnNumber txnNumber{1};

        OplogBatch batch;
        batch.emplace_back(makeCommandOp(BSON("abortTransaction" << 1), lsid, txnNumber));

        std::list<repl::OplogEntry> derivedOps;
        auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
        ASSERT_FALSE(writerVectors.empty());

        auto writer = getNonEmptyWriterVector(writerVectors);
        ASSERT_EQ(writer.size(), 1U);
        ASSERT_EQ(derivedOps.size(), 0U);
        ASSERT_EQ(writer[0]->getSessionId(), lsid);
        ASSERT_EQ(*writer[0]->getTxnNumber(), txnNumber);
        ASSERT(writer[0]->getCommandType() == repl::OplogEntry::CommandType::kAbortTransaction);
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsForPartialUnpreparedTxn) {
    auto runTest = [&](const LogicalSessionId& lsid) {
        const TxnNumber txnNumber{1};

        OplogBatch batch;
        batch.emplace_back(makeApplyOpsForInsert(
            {BSON("_id" << 0)}, lsid, txnNumber, false /* isPrepare */, true /* isPartial */));

        std::list<repl::OplogEntry> derivedOps;
        auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
        if (isInternalSessionForRetryableWrite(lsid)) {
            ASSERT_FALSE(writerVectors.empty());
            auto writer = getNonEmptyWriterVector(writerVectors);
            ASSERT_EQ(writer.size(), 1U);
            ASSERT_EQ(derivedOps.size(), 1U);
            ASSERT_EQ(writer[0]->getSessionId(), *getParentSessionId(lsid));
            ASSERT_EQ(*writer[0]->getTxnNumber(), *lsid.getTxnNumber());
            ASSERT(writer[0]->getOpType() == repl::OpTypeEnum::kInsert);
            ASSERT_BSONOBJ_EQ(writer[0]->getObject(), (BSON("_id" << 0)));
        } else {
            ASSERT_EQ(derivedOps.size(), 0U);
            for (const auto& writer : writerVectors) {
                ASSERT_TRUE(writer.empty());
            }
        }
    };

    runTest(makeLogicalSessionIdForTest());
    runTest(makeLogicalSessionIdWithTxnUUIDForTest());
    runTest(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
}

TEST_F(ReshardingOplogBatchPreparerTest, SessionWriteVectorsDeriveCrudOpsForMultiApplyOpsBasic) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNumber{1};

    OplogBatch batch;
    // 'makeApplyOpsForInsert' uses the "_id" of each document as the "stmtId"
    batch.emplace_back(makeApplyOpsForInsert({BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2)},
                                             lsid,
                                             txnNumber,
                                             false /* isPrepare */,
                                             false /* isPartial */,
                                             repl::MultiOplogEntryType::kApplyOpsAppliedSeparately,
                                             true /* useStmtIds */));

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch, derivedOps);
    ASSERT_FALSE(writerVectors.empty());

    auto writer = getNonEmptyWriterVector(writerVectors);

    ASSERT_EQ(writer.size(), 3U);
    ASSERT_EQ(derivedOps.size(), 3U);
    for (size_t i = 0; i < writer.size(); ++i) {
        ASSERT_EQ(writer[i]->getSessionId(), lsid);
        ASSERT_EQ(*writer[i]->getTxnNumber(), txnNumber);
        ASSERT(writer[i]->getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT_BSONOBJ_EQ(writer[i]->getObject(), (BSON("_id" << static_cast<int>(i))));
        ASSERT_FALSE(writer[i]->getMultiOpType());
    }
}

}  // namespace
}  // namespace mongo
