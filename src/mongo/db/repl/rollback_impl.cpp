/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_impl.h"

#include <exception>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/rollback_impl_listener.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Creates an operation context using the current Client.
 */
ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

}  // namespace

RollbackImpl::RollbackImpl(executor::TaskExecutor* executor,
                           OplogInterface* localOplog,
                           const HostAndPort& syncSource,
                           const NamespaceString& remoteOplogNss,
                           std::size_t maxFetcherRestarts,
                           int requiredRollbackId,
                           ReplicationCoordinator* replicationCoordinator,
                           StorageInterface* storageInterface,
                           const OnCompletionFn& onCompletion)
    : AbstractAsyncComponent(executor, "rollback"),
      _localOplog(localOplog),
      _syncSource(syncSource),
      _remoteOplogNss(remoteOplogNss),
      _maxFetcherRestarts(maxFetcherRestarts),
      _requiredRollbackId(requiredRollbackId),
      _replicationCoordinator(replicationCoordinator),
      _storageInterface(storageInterface),
      _listener(stdx::make_unique<Listener>()),
      _commonPointResolver(stdx::make_unique<RollbackCommonPointResolver>(
          executor,
          syncSource,
          remoteOplogNss,
          maxFetcherRestarts,
          localOplog,
          _listener.get(),
          stdx::bind(&RollbackImpl::_commonPointResolverCallback, this, stdx::placeholders::_1))),
      _onCompletion(onCompletion) {
    // Task executor will be validated by AbstractAsyncComponent's constructor.
    invariant(localOplog);
    uassert(ErrorCodes::BadValue, "sync source must be valid", !syncSource.empty());
    invariant(replicationCoordinator);
    invariant(storageInterface);
    invariant(onCompletion);
}

RollbackImpl::~RollbackImpl() {
    shutdown();
    join();
}

// static
StatusWith<OpTime> RollbackImpl::readLocalRollbackInfoAndApplyUntilConsistentWithSyncSource(
    ReplicationCoordinator* replicationCoordinator, StorageInterface* storageInterface) {
    return {ErrorCodes::InternalError, "Method not implemented"};
}

Status RollbackImpl::_doStartup_inlock() noexcept {
    return _scheduleWorkAndSaveHandle_inlock(
        stdx::bind(&RollbackImpl::_transitionToRollbackCallback, this, stdx::placeholders::_1),
        &_transitionToRollbackHandle,
        str::stream() << "_transitionToRollbackCallback");
}

void RollbackImpl::_doShutdown_inlock() noexcept {
    _cancelHandle_inlock(_transitionToRollbackHandle);
    _shutdownComponent_inlock(_commonPointResolver);
}

stdx::mutex* RollbackImpl::_getMutex() noexcept {
    return &_mutex;
}

void RollbackImpl::_transitionToRollbackCallback(
    const executor::TaskExecutor::CallbackArgs& callbackArgs) {
    auto status = _checkForShutdownAndConvertStatus(
        callbackArgs, str::stream() << "error before transition to ROLLBACK");
    if (!status.isOK()) {
        _finishCallback(nullptr, status);
        return;
    }

    log() << "Rollback - transition to ROLLBACK";
    auto opCtx = makeOpCtx();
    {
        Lock::GlobalWrite globalWrite(opCtx.get());

        if (!_replicationCoordinator->setFollowerMode(MemberState::RS_ROLLBACK)) {
            std::string msg = str::stream()
                << "Cannot transition from " << _replicationCoordinator->getMemberState().toString()
                << " to " << MemberState(MemberState::RS_ROLLBACK).toString();
            log() << msg;
            status = Status(ErrorCodes::NotSecondary, msg);
        }
    }
    if (!status.isOK()) {
        _finishCallback(opCtx.get(), status);
        return;
    }

    // Schedule RollbackCommonPointResolver
    status = _startupComponent(_commonPointResolver);
    if (!status.isOK()) {
        _finishCallback(opCtx.get(), status);
        return;
    }
}

void RollbackImpl::_commonPointResolverCallback(const Status& commonPointResolverStatus) {
    auto status = _checkForShutdownAndConvertStatus(
        commonPointResolverStatus,
        str::stream() << "failed to find common point between local and remote oplogs");
    if (!status.isOK()) {
        _finishCallback(nullptr, status);
        return;
    }

    // Success! For now....
    _finishCallback(nullptr, OpTime());
}

void RollbackImpl::_checkShardIdentityRollback(OperationContext* opCtx) {
    invariant(opCtx);

    if (ShardIdentityRollbackNotifier::get(opCtx)->didRollbackHappen()) {
        severe() << "shardIdentity document rollback detected.  Shutting down to clear "
                    "in-memory sharding state.  Restarting this process should safely return it "
                    "to a healthy state";
        fassertFailedNoTrace(40407);
    }
}

void RollbackImpl::_transitionFromRollbackToSecondary(OperationContext* opCtx) {
    invariant(opCtx);

    Lock::GlobalWrite globalWrite(opCtx);

    // If the current member state is not ROLLBACK, this means that the
    // ReplicationCoordinator::setFollowerMode(ROLLBACK) call in _transitionToRollbackCallback()
    // failed. In that case, there's nothing to do in this function.
    if (MemberState(MemberState::RS_ROLLBACK) != _replicationCoordinator->getMemberState()) {
        return;
    }

    if (!_replicationCoordinator->setFollowerMode(MemberState::RS_SECONDARY)) {
        severe() << "Failed to transition into " << MemberState(MemberState::RS_SECONDARY)
                 << "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK)
                 << " but found self in " << _replicationCoordinator->getMemberState();
        fassertFailedNoTrace(40408);
    }
}

void RollbackImpl::_tearDown(OperationContext* opCtx) {
    invariant(opCtx);

    _checkShardIdentityRollback(opCtx);
    _transitionFromRollbackToSecondary(opCtx);
}

void RollbackImpl::_finishCallback(OperationContext* opCtx, StatusWith<OpTime> lastApplied) {
    // Abort only when we are in a unrecoverable state.
    // WARNING: these statuses sometimes have location codes which are lost with uassertStatusOK
    // so we need to check here first.
    if (ErrorCodes::UnrecoverableRollbackError == lastApplied.getStatus().code()) {
        severe() << "Unable to complete rollback. A full resync may be needed: "
                 << redact(lastApplied.getStatus());
        fassertFailedNoTrace(40435);
    }

    // After running callback function '_onCompletion', clear '_onCompletion' to release any
    // resources that might be held by this function object.
    // '_onCompletion' must be moved to a temporary copy and destroyed outside the lock in case
    // there is any logic that's invoked at the function object's destruction that might call into
    // this RollbackImpl. 'onCompletion' must be destroyed outside the lock and this should happen
    // before we transition the state to Complete.
    decltype(_onCompletion) onCompletion;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_onCompletion);
        std::swap(_onCompletion, onCompletion);
    }

    // If 'opCtx' is null, lazily create OperationContext using makeOpCtx() that will last for the
    // duration of this function call.
    _tearDown(opCtx ? opCtx : makeOpCtx().get());

    // Completion callback must be invoked outside mutex.
    try {
        onCompletion(lastApplied);
    } catch (...) {
        severe() << "rollback finish callback threw exception: " << redact(exceptionToStatus());
        // This exception handling block should be unreachable because OnCompletionFn is declared
        // noexcept. This is purely a defensive mechanism to guard against C++ runtime
        // implementations that have less than ideal support for noexcept.
        MONGO_UNREACHABLE;
    }

    // Destroy the remaining reference to the completion callback before we transition the state to
    // Complete so that callers can expect any resources bound to '_onCompletion' to be released
    // before RollbackImpl::join() returns.
    onCompletion = {};

    _transitionToComplete();
}

}  // namespace repl
}  // namespace mongo
