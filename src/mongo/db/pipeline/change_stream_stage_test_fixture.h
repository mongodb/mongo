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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ExecutableStubMongoProcessInterface : public StubMongoProcessInterface {
    bool isExpectedToExecuteQueries() override {
        return true;
    }
};

class ChangeStreamStageTestNoSetup : public AggregationContextFixture {
public:
    ChangeStreamStageTestNoSetup();
    explicit ChangeStreamStageTestNoSetup(NamespaceString nsString);
};

struct MockMongoInterface final : public ExecutableStubMongoProcessInterface {
    // Used by operations which need to obtain the oplog's UUID.
    static const UUID& oplogUuid();

    MockMongoInterface(std::vector<repl::OplogEntry> transactionEntries = {},
                       std::vector<Document> documentsForLookup = {});

    // For tests of transactions that involve multiple oplog entries.
    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const override;

    // Called by DocumentSourceAddPreImage to obtain the UUID of the oplog. Since that's the only
    // piece of collection info we need for now, just return a BSONObj with the mock oplog UUID.
    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) override;

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        boost::optional<UUID> collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final;

    // Stores oplog entries associated with a commit operation, including the oplog entries that a
    // real DocumentSourceChangeStream would not see, because they are marked with a "prepare" or
    // "partialTxn" flag. When the DocumentSourceChangeStream sees the commit for the transaction,
    // either an explicit "commitCommand" or an implicit commit represented by an "applyOps" that is
    // not marked with the "prepare" or "partialTxn" flag, it uses a TransactionHistoryIterator to
    // go back and look up these entries.
    //
    // These entries are stored in the order they would be returned by the
    // TransactionHistoryIterator, which is the _reverse_ of the order they appear in the oplog.
    std::vector<repl::OplogEntry> _transactionEntries;

    // These documents are used to feed the 'lookupSingleDocument' method.
    std::vector<Document> _documentsForLookup;
};

class ChangeStreamStageTest : public ChangeStreamStageTestNoSetup {
public:
    ChangeStreamStageTest();

    explicit ChangeStreamStageTest(NamespaceString nsString);

    void checkTransformation(const repl::OplogEntry& entry,
                             const boost::optional<Document>& expectedDoc,
                             const BSONObj& spec = change_stream_test_helper::kDefaultSpec,
                             const boost::optional<Document>& expectedInvalidate = {},
                             const std::vector<repl::OplogEntry>& transactionEntries = {},
                             std::vector<Document> documentsForLookup = {},
                             const boost::optional<std::int32_t>& expectedErrorCode = {});

    /**
     * Returns a list of stages expanded from a $changStream specification, starting with a
     * DocumentSourceMock which contains a single document representing 'entry'.
     *
     * Stages such as DSEnsureResumeTokenPresent which can swallow results are removed from the
     * returned list.
     */
    std::unique_ptr<exec::agg::Pipeline> makeExecPipeline(BSONObj entry, const BSONObj& spec);

    /**
     * Returns a list of the stages expanded from a $changStream specification, starting with a
     * DocumentSourceMock which contains a list of document representing 'entries'.
     */
    std::unique_ptr<exec::agg::Pipeline> makeExecPipeline(
        std::vector<BSONObj> entries,
        const BSONObj& spec,
        bool removeEnsureResumeTokenStage = false);

    std::unique_ptr<exec::agg::Pipeline> makeExecPipeline(
        const repl::OplogEntry& entry,
        const BSONObj& spec = change_stream_test_helper::kDefaultSpec);

    repl::OplogEntry createCommand(const BSONObj& oField,
                                   boost::optional<UUID> uuid = boost::none,
                                   boost::optional<bool> fromMigrate = boost::none,
                                   boost::optional<repl::OpTime> opTime = boost::none);
    /**
     * Helper for running an applyOps through the pipeline, and getting all of the results.
     */
    std::vector<Document> getApplyOpsResults(const Document& applyOpsDoc,
                                             const LogicalSessionFromClient& lsid,
                                             BSONObj spec = change_stream_test_helper::kDefaultSpec,
                                             bool hasTxnNumber = true);

    Document makeExpectedUpdateEvent(Timestamp ts,
                                     const NamespaceString& nss,
                                     BSONObj documentKey,
                                     Document updateDescription,
                                     bool expandedEvents = false);

    /**
     * Helper function to do a $v:2 delta oplog test.
     */
    void runUpdateV2OplogTest(BSONObj diff, Document updateModificationEntry);

    /**
     * Helper to create change stream pipeline for testing.
     */
    std::unique_ptr<Pipeline> buildTestPipeline(const std::vector<BSONObj>& rawPipeline);

    /**
     * Helper to verify if the change stream pipeline contains expected stages.
     */
    void assertStagesNameOrder(std::unique_ptr<Pipeline> pipeline,
                               const std::vector<std::string>& expectedStages);
};

}  // namespace mongo
