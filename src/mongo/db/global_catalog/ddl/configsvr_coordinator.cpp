// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

#include <string_view>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
using namespace std::literals::string_view_literals;

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

void ConfigsvrCoordinator::interrupt(Status status) noexcept {
    LOGV2_DEBUG(6347303,
                1,
                "ConfigsvrCoordinator received an interrupt",
                "coordinatorId"_attr = _coordId,
                "reason"_attr = redact(status));

    // Resolve any unresolved promises to avoid hanging.
    std::lock_guard<std::mutex> lg(_mutex);
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

            return AsyncTry([this, executor, token] {
                       // Perform a causality barrier to invalidate any retryable writes issued by
                       // previous executions (an earlier attempt of this instance, or a previous
                       // primary) before doing any work. Done here so individual coordinators don't
                       // have to. This is a no-op on the first execution, when no session has been
                       // persisted yet and thus there is nothing to invalidate.
                       _performCausalityBarrier(executor, token);

                       // Run the coordinator.
                       return _runImpl(executor, token);
                   })
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
                _onCleanup(opCtx);
            } catch (DBException& ex) {
                LOGV2_WARNING(6347302,
                              "Failed to remove ConfigsvrCoordinator state document",
                              "error"_attr = redact(ex));
                ex.addContext("Failed to remove ConfigsvrCoordinator state document"sv);
                std::lock_guard<std::mutex> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(ex.toStatus());
                }
                throw;
            }

            std::lock_guard<std::mutex> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.emplaceValue();
            }

            return status;
        })
        .semi();
}

}  // namespace mongo
