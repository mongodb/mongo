/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "boost/optional/optional.hpp"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo::repl {
// Runs on tenant migration recipient primary and secondaries. Copies and imports donor files.
class TenantFileImporterService : public ReplicaSetAwareService<TenantFileImporterService> {
public:
    static constexpr StringData kTenantFileImporterServiceName = "TenantFileImporterService"_sd;
    static TenantFileImporterService* get(ServiceContext* serviceContext);
    TenantFileImporterService() = default;
    void startMigration(const UUID& migrationId, const StringData& donorConnectionString);
    void learnedFilename(const UUID& migrationId, const BSONObj& metadataDoc);
    void learnedAllFilenames(const UUID& migrationId);
    void interrupt(const UUID& migrationId);
    void interruptAll();

private:
    void onInitialDataAvailable(OperationContext*, bool) final {}

    void onShutdown() final {
        stdx::unique_lock<Latch> lk(_mutex);
        _interrupt(lk);
        _reset(lk);
    }

    void onStartup(OperationContext*) final {}

    void onStepUpBegin(OperationContext*, long long) final {}

    void onStepUpComplete(OperationContext*, long long) final {}

    void onStepDown() final {}

    void onBecomeArbiter() final {}

    void _handleEvents(OperationContext* opCtx);

    void _voteImportedFiles(OperationContext* opCtx);

    void _interrupt(WithLock);

    void _reset(WithLock);

    std::unique_ptr<stdx::thread> _thread;
    boost::optional<UUID> _migrationId;
    std::string _donorConnectionString;
    Mutex _mutex = MONGO_MAKE_LATCH("TenantFileImporterService::_mutex");

    // Explicit State enum ordering defined here because we rely on comparison
    // operators for state checking in various TenantFileImporterService methods.
    enum class State {
        kUninitialized = 0,
        kStarted = 1,
        kLearnedFilename = 2,
        kLearnedAllFilenames = 3,
        kInterrupted = 4
    };

    static StringData stateToString(State state) {
        switch (state) {
            case State::kUninitialized:
                return "uninitialized";
            case State::kStarted:
                return "started";
            case State::kLearnedFilename:
                return "learned filename";
            case State::kLearnedAllFilenames:
                return "learned all filenames";
            case State::kInterrupted:
                return "interrupted";
        }
        MONGO_UNREACHABLE;
        return StringData();
    }

    State _state;

    struct ImporterEvent {
        enum class Type { kNone, kLearnedFileName, kLearnedAllFilenames };
        Type type;
        UUID migrationId;
        BSONObj metadataDoc;

        ImporterEvent(Type _type, const UUID& _migrationId)
            : type(_type), migrationId(_migrationId) {}
    };

    using Queue =
        MultiProducerSingleConsumerQueue<ImporterEvent,
                                         producer_consumer_queue_detail::DefaultCostFunction>;

    std::shared_ptr<Queue> _eventQueue;
};
}  // namespace mongo::repl
