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
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo::repl {
// Runs on tenant migration recipient primary and secondaries. Copies and imports donor files.
class TenantFileImporterService : public ReplicaSetAwareService<TenantFileImporterService> {
public:
    static constexpr StringData kTenantFileImporterServiceName = "TenantFileImporterService"_sd;
    static TenantFileImporterService* get(ServiceContext* serviceContext);
    TenantFileImporterService() = default;
    void startMigration(const UUID& migrationId);
    void learnedFilename(const UUID& migrationId, const BSONObj& metadataDoc);
    void learnedAllFilenames(const UUID& migrationId);
    void reset();

private:
    void onStartup(OperationContext* opCtx) final;

    void onShutdown() final {
        stdx::lock_guard lk(_mutex);
        _reset(lk);
    }

    void onStepUpBegin(OperationContext* opCtx, long long term) final {
        stdx::lock_guard lk(_mutex);
        _reset(lk);
    }

    void onStepUpComplete(OperationContext* opCtx, long long term) final {
        stdx::lock_guard lk(_mutex);
        _reset(lk);
    }

    void onStepDown() final {
        stdx::lock_guard lk(_mutex);
        _reset(lk);
    }

    void onBecomeArbiter() final {
        stdx::lock_guard lk(_mutex);
        _reset(lk);
    }

    void _voteImportedFiles();

    void _reset(WithLock lk);

    // Lasts for the lifetime of the process.
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    // Wraps _executor. Created by learnedFilename and destroyed by _reset.
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;
    boost::optional<UUID> _migrationId;
    Mutex _mutex = MONGO_MAKE_LATCH("TenantFileImporterService::_mutex");

    class ImporterState {
    public:
        enum class State { kUninitialized, kCopyingFiles, kCopiedFiles, kImportedFiles };

        void setState(State nextState) {
            tassert(6114403,
                    str::stream() << "current state: " << toString(_state)
                                  << ", new state: " << toString(nextState),
                    isValidTransition(nextState));
            _state = nextState;
        }

        bool is(State state) const {
            return _state == state;
        }

        StringData toString() const {
            return toString(_state);
        }

    private:
        static StringData toString(State value) {
            switch (value) {
                case State::kUninitialized:
                    return "uninitialized";
                case State::kCopyingFiles:
                    return "copying files";
                case State::kCopiedFiles:
                    return "copied files";
                case State::kImportedFiles:
                    return "imported files";
            }
            MONGO_UNREACHABLE;
            return StringData();
        }

        bool isValidTransition(State newState) {
            if (_state == newState) {
                return true;
            }

            switch (_state) {
                case State::kUninitialized:
                    return newState == State::kCopyingFiles;
                case State::kCopyingFiles:
                    return newState == State::kCopiedFiles || newState == State::kUninitialized;
                case State::kCopiedFiles:
                    return newState == State::kImportedFiles || newState == State::kUninitialized;
                case State::kImportedFiles:
                    return newState == State::kUninitialized;
            }
            MONGO_UNREACHABLE;
        }

        State _state = State::kUninitialized;
    };

    ImporterState _state;
};
}  // namespace mongo::repl
