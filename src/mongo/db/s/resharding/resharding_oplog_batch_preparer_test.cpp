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

#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using OplogBatch = ReshardingOplogBatchPreparer::OplogBatchToPrepare;

class ScopedServerParameterChange {
public:
    ScopedServerParameterChange(int* param, int newValue) : _param(param), _originalValue(*_param) {
        *param = newValue;
    }

    ~ScopedServerParameterChange() {
        *_param = _originalValue;
    }

private:
    int* const _param;
    const int _originalValue;
};

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
        op.setOpTimeAndWallTimeBase({{}, {}});

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
        op.setOpTimeAndWallTimeBase({{}, {}});

        return {op.toBSON()};
    }

    ReshardingOplogBatchPreparer::OplogBatchToApply& getNonEmptyWriterVector(
        ReshardingOplogBatchPreparer::WriterVectors writerVectors) {
        ReshardingOplogBatchPreparer::OplogBatchToApply* nonempty = nullptr;

        for (auto& writer : writerVectors) {
            if (!writer.empty()) {
                ASSERT_FALSE(nonempty)
                    << "Expected only one non-empty writer vector, but found multiple";
                nonempty = &writer;
            }
        }

        ASSERT_TRUE(nonempty) << "Expected to find a non-empty writer vector, but didn't";
        return *nonempty;
    }

    ReshardingOplogBatchPreparer _batchPreparer{nullptr};

    static constexpr size_t kNumWriterVectors = 2;

private:
    ScopedServerParameterChange _numWriterVectors{&resharding::gReshardingWriterThreadCount,
                                                  int(kNumWriterVectors)};
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
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << i << "fromApplyOps" << false)));
    }

    BSONObjBuilder applyOpsBuilder;
    {
        BSONArrayBuilder opsArrayBuilder = applyOpsBuilder.subarrayStart("applyOps");
        for (int i = 0; i < numOps; ++i) {
            // We use OpTypeEnum::kInsert rather than OpTypeEnum::kUpdate here to avoid needing to
            // deal with setting the 'o2' field.
            opsArrayBuilder.append(
                repl::DurableReplOperation(
                    repl::OpTypeEnum::kInsert, {}, BSON("_id" << i << "fromApplyOps" << true))
                    .toBSON());
        }
    }

    batch.emplace_back(makeCommandOp(applyOpsBuilder.done()));

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
            batch.emplace_back(makeCommandOp(BSON(
                "applyOps" << BSON_ARRAY(repl::DurableReplOperation(
                                             repl::OpTypeEnum::kInsert,
                                             {},
                                             BSON("_id" << 0 << "n" << i << "fromApplyOps" << true))
                                             .toBSON()))));
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

    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);

    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), numOps);
    for (int i = 0; i < numOps; ++i) {
        ASSERT_BSONOBJ_BINARY_EQ(writer[i]->getObject(), BSON("_id" << i));
        ASSERT_EQ(writer[i]->getSessionId(), lsid);
        ASSERT_EQ(writer[i]->getTxnNumber(), TxnNumber{1});
        ASSERT_TRUE(writer[i]->isForReshardingSessionApplication());
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, DiscardsLowerTxnNumberSessionOps) {
    OplogBatch batch;
    auto lsid = makeLogicalSessionIdForTest();

    int numOps = 5;
    for (int i = 1; i <= numOps; ++i) {
        batch.emplace_back(makeUpdateOp(BSON("_id" << i), lsid, TxnNumber{i}));
    }

    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);

    auto writer = getNonEmptyWriterVector(writerVectors);
    ASSERT_EQ(writer.size(), 1U);
    ASSERT_BSONOBJ_BINARY_EQ(writer[0]->getObject(), BSON("_id" << numOps));
    ASSERT_EQ(writer[0]->getSessionId(), lsid);
    ASSERT_EQ(writer[0]->getTxnNumber(), TxnNumber{numOps});
    ASSERT_TRUE(writer[0]->isForReshardingSessionApplication());
}

TEST_F(ReshardingOplogBatchPreparerTest, DistributesSessionOpsToWriterVectorsFairly) {
    OplogBatch batch;

    int numOps = 100;
    for (int i = 0; i < numOps; ++i) {
        batch.emplace_back(
            makeUpdateOp(BSON("_id" << i), makeLogicalSessionIdForTest(), TxnNumber{1}));
    }

    auto writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch);
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
        ASSERT_THROWS_CODE(_batchPreparer.makeSessionOpWriterVectors(batch),
                           DBException,
                           ErrorCodes::OplogOperationUnsupported);
    }
}

TEST_F(ReshardingOplogBatchPreparerTest, DiscardsNoops) {
    OplogBatch batch;

    int numOps = 5;
    for (int i = 0; i < numOps; ++i) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});
        op.setNss({});
        op.setOpTimeAndWallTimeBase({{}, {}});
        batch.emplace_back(op.toBSON());
    }

    std::list<repl::OplogEntry> derivedOps;
    auto writerVectors = _batchPreparer.makeCrudOpWriterVectors(batch, derivedOps);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(derivedOps.size(), 0U);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);

    writerVectors = _batchPreparer.makeSessionOpWriterVectors(batch);
    ASSERT_EQ(writerVectors.size(), kNumWriterVectors);
    ASSERT_EQ(writerVectors[0].size(), 0U);
    ASSERT_EQ(writerVectors[1].size(), 0U);
}

}  // namespace
}  // namespace mongo
