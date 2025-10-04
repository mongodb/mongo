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


#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeRunningConfigsvrCoordinatorInstance);
MONGO_FAIL_POINT_DEFINE(hangAndEndBeforeRunningConfigsvrCoordinatorInstance);

namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

ConfigsvrCoordinatorMetadata extractConfigsvrCoordinatorMetadata(const BSONObj& stateDoc) {
    return ConfigsvrCoordinatorMetadata::parse(stateDoc,
                                               IDLParserContext("ConfigsvrCoordinatorMetadata"));
}

ConfigsvrCoordinator::ConfigsvrCoordinator(const BSONObj& stateDoc)
    : _coordId(extractConfigsvrCoordinatorMetadata(stateDoc).getId()) {}

ConfigsvrCoordinator::~ConfigsvrCoordinator() {
    tassert(10644543,
            "Expected _completionPromise to be ready",
            _completionPromise.getFuture().isReady());
}

void ConfigsvrCoordinator::_removeStateDocument(OperationContext* opCtx) {
    LOGV2_DEBUG(6347304,
                2,
                "Removing state document for ConfigsvrCoordinator instance",
                "coordId"_attr = _coordId);

    PersistentTaskStore<ConfigsvrCoordinatorMetadata> store(
        NamespaceString::kConfigsvrCoordinatorsNamespace);
    store.remove(opCtx,
                 BSON(ConfigsvrCoordinatorMetadata::kIdFieldName << _coordId.toBSON()),
                 defaultMajorityWriteConcern());
}

OperationSessionInfo ConfigsvrCoordinator::_getCurrentSession() const {
    const auto& coordinatorMetadata = metadata();
    tassert(10644544,
            "Expected session to be set on the coordinator document metadata",
            coordinatorMetadata.getSession());
    ConfigsvrCoordinatorSession coordinatorSession = *coordinatorMetadata.getSession();

    OperationSessionInfo osi;
    osi.setSessionId(coordinatorSession.getLsid());
    osi.setTxnNumber(coordinatorSession.getTxnNumber());
    return osi;
}

void ConfigsvrCoordinator::interrupt(Status status) noexcept {
    LOGV2_DEBUG(6347303,
                1,
                "ConfigsvrCoordinator received an interrupt",
                "coordinatorId"_attr = _coordId,
                "reason"_attr = redact(status));

    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

SemiFuture<void> ConfigsvrCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                           const CancellationToken& token) noexcept {
    if (hangAndEndBeforeRunningConfigsvrCoordinatorInstance.shouldFail()) {
        hangAndEndBeforeRunningConfigsvrCoordinatorInstance.pauseWhileSet();
        _completionPromise.emplaceValue();
        return Status::OK();
    }
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            hangBeforeRunningConfigsvrCoordinatorInstance.pauseWhileSet();

            return AsyncTry([this, executor, token] { return _runImpl(executor, token); })
                .until([this, token](Status status) { return status.isOK() || token.isCanceled(); })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, CancellationToken::uncancelable());
        })
        .onCompletion([this, executor, token, anchor = shared_from_this()](const Status& status) {
            if (!status.isOK()) {
                LOGV2_ERROR(
                    6347301, "Error executing ConfigsvrCoordinator", "error"_attr = redact(status));

                // Nothing else to do, the _completionPromise will be cancelled once the coordinator
                // is interrupted, because the only reasons to stop forward progress in this node is
                // because of a stepdown happened or the coordinator was canceled.
                tassert(10644545,
                        "Execution of the ConfigsvrCoordinator failed unexpectedly",
                        (token.isCanceled() && status.isA<ErrorCategory::CancellationError>()) ||
                            status.isA<ErrorCategory::NotPrimaryError>());
                return status;
            }

            try {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _removeStateDocument(opCtx);

                const auto session = metadata().getSession();
                if (session && status.isOK()) {
                    // Return lsid to the InternalSessionPool. If status is not OK, let the session
                    // be discarded.
                    InternalSessionPool::get(opCtx)->release(
                        {session->getLsid(), session->getTxnNumber()});
                }

            } catch (DBException& ex) {
                LOGV2_WARNING(6347302,
                              "Failed to remove ConfigsvrCoordinator state document",
                              "error"_attr = redact(ex));
                ex.addContext("Failed to remove ConfigsvrCoordinator state document"_sd);
                stdx::lock_guard<stdx::mutex> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(ex.toStatus());
                }
                throw;
            }

            stdx::lock_guard<stdx::mutex> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.emplaceValue();
            }

            return status;
        })
        .semi();
}

}  // namespace mongo
