/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/change_stream_handle_topology_change_v2_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_builder_mock.h"
#include "mongo/db/pipeline/change_stream_shard_targeter_mock.h"
#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service_mock.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/clock_source_mock.h"

#include <deque>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace test {

using V2Stage = exec::agg::ChangeStreamHandleTopologyChangeV2Stage;

struct V2StageTestHelpers {
    static const ChangeStreamShardTargeterMock::ReaderContextCallback kEmptyShardTargeterCallback;
    static constexpr int kDefaultMinAllocationToShardsPollPeriodSecs = 1;
};

// Simple mock aggregation stage that supports an "undo" operation. Needed because the
// 'DocumentSourceMock' agg stage does not support undoing of already returned results.
class MockWithUndoStage : public exec::agg::Stage {
public:
    MockWithUndoStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      std::deque<exec::agg::GetNextResult> results);

    static boost::intrusive_ptr<MockWithUndoStage> createForTest(
        std::deque<exec::agg::GetNextResult> results,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    void undo();

protected:
    exec::agg::GetNextResult doGetNext() override;

private:
    std::deque<exec::agg::GetNextResult> _queue;
    boost::optional<exec::agg::GetNextResult> _undoResult;
};

class CursorManagerMock : public V2Stage::CursorManager {
public:
    CursorManagerMock(
        const ChangeStream& changeStream,
        ChangeStreamReaderBuilder* readerBuilder,
        boost::optional<boost::intrusive_ptr<MockWithUndoStage>> stageForUndo = boost::none);

    void initialize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    V2Stage* stage,
                    const ResumeTokenData& resumeTokenData) override;

    void openCursorsOnDataShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 OperationContext* opCtx,
                                 Timestamp atClusterTime,
                                 const stdx::unordered_set<ShardId>& shardIds) override;

    void openCursorOnConfigServer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  OperationContext* opCtx,
                                  Timestamp atClusterTime) override;

    void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardIds) override;

    void closeCursorOnConfigServer(OperationContext* opCtx) override;

    bool isCursorOnConfigServerOpen() const override;

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override;

    const ChangeStream& getChangeStream() const override;

    void enableUndoNextMode() override;

    void disableUndoNextMode() override;

    void undoGetNext() override;

    void setHighWaterMark(Timestamp highWaterMark) override;

    Timestamp getTimestampFromCurrentHighWaterMark() const override;

    bool undoGetNextCalled() const;

    boost::optional<bool> getUndoNextMode() const;

    boost::optional<Timestamp> getRestoredHighWaterMark() const;

    void setTimestampForCurrentHighWaterMark(Timestamp ts);

    bool isInitialized() const;

    ResumeToken getResumeToken() const;

    bool cursorOpenedOnConfigServer() const;

    void setThrowShardNotFoundExceptions(int value);

    void setThrowRetryChangeStreamExceptions(int value);

private:
    void throwShardNotFoundExceptionIfRequired();

    void throwRetryChangeStreamExceptionIfRequired();

    const ChangeStream _changeStream;
    ChangeStreamReaderBuilder* _readerBuilder;
    stdx::unordered_set<ShardId> _currentlyTargetedDataShards;

    // If set, this many attempts to open a cursor will throw a 'ShardNotFound' exception.
    int _throwShardNotFoundExceptions = 0;

    // If set, this many attempts to open a config server cursor will throw a 'RetryChangeStream'
    // exception.
    int _throwRetryChangeStreamExceptions = 0;

    // Will be set to true if a request was made to open a cursor on the config server.
    bool _cursorOpenedOnConfigServer = false;

    // If set, any attempt to open a cursor will throw a 'ShardNotFound' exception.
    bool _throwShardNotFoundException = false;

    // Will be set to true if 'undoGetNext()' was called.
    bool _undoGetNextCalled = false;

    // Resume token used when initializing the 'CursorManager'.
    boost::optional<ResumeToken> _resumeToken;

    boost::optional<Timestamp> _highWaterMarkTimestamp;

    // Calls to enable/disable undo mode will be recorded here.
    boost::optional<bool> _undoNextMode;

    // The timestamp used in a call to 'setHighWaterMark()' will be recorded here after overfetching
    // in degraded mode.
    boost::optional<Timestamp> _restoredHighWaterMark;

    // The aggregation stage that is used as input for the v2 stage. Necessary here so we can
    // perform an "undo" operation it if necessary.
    boost::optional<boost::intrusive_ptr<MockWithUndoStage>> _stageForUndo;
};

class DeadlineWaiterMock : public V2Stage::DeadlineWaiter {
public:
    void waitUntil(OperationContext* opCtx, Date_t deadline) override;

    Date_t getLastUsedDeadline() const;

    void setStatus(Status status);

private:
    Status _status = Status::OK();
    Date_t _lastUsedDeadline;
};

// Helper functions used in the tests.
// -----------------------------------

BSONObj buildHighWaterMarkToken(Timestamp ts);

DocumentSourceChangeStreamSpec buildChangeStreamSpec(Timestamp ts, ChangeStreamReadMode mode);

BSONObj getStageSpec();

std::shared_ptr<V2Stage::Parameters> buildParametersForTest(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    int minAllocationToShardsPollPeriodSecs,
    ChangeStreamReaderBuilder* changeStreamReaderBuilder,
    DataToShardsAllocationQueryService* dataToShardsAllocationQueryService,
    boost::optional<boost::intrusive_ptr<MockWithUndoStage>> stageForUndo = boost::none);

DataToShardsAllocationQueryServiceMock* getDataToShardsAllocationQueryServiceMock(
    std::shared_ptr<V2Stage::Parameters>& params);

ChangeStreamReaderBuilderMock* getChangeStreamReaderBuilderMock(
    std::shared_ptr<V2Stage::Parameters>& params);

ChangeStreamShardTargeterMock* getChangeStreamShardTargeterMock(
    std::shared_ptr<V2Stage::Parameters>& params);

CursorManagerMock* getCursorManagerMock(std::shared_ptr<V2Stage::Parameters>& params);

DeadlineWaiterMock* getDeadlineWaiterMock(std::shared_ptr<V2Stage::Parameters>& params);

ClockSourceMock* getPreciseClockSource(ServiceContext* serviceContext);

}  // namespace test
}  // namespace mongo
