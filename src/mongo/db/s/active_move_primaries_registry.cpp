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

#include "mongo/db/s/active_move_primaries_registry.h"

#include "mongo/db/service_context.h"

namespace mongo {
namespace {

const auto getRegistry = ServiceContext::declareDecoration<ActiveMovePrimariesRegistry>();

}  // namespace

ActiveMovePrimariesRegistry::ActiveMovePrimariesRegistry() = default;

ActiveMovePrimariesRegistry::~ActiveMovePrimariesRegistry() {
    invariant(!_activeMovePrimaryState);
}

ActiveMovePrimariesRegistry& ActiveMovePrimariesRegistry::get(ServiceContext* service) {
    return getRegistry(service);
}

ActiveMovePrimariesRegistry& ActiveMovePrimariesRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

StatusWith<ScopedMovePrimary> ActiveMovePrimariesRegistry::registerMovePrimary(
    const ShardMovePrimary& requestArgs) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_activeMovePrimaryState) {
        if (_activeMovePrimaryState->requestArgs == requestArgs) {
            return {ScopedMovePrimary(nullptr, false, _activeMovePrimaryState->notification)};
        }

        return _activeMovePrimaryState->constructErrorStatus();
    }

    _activeMovePrimaryState.emplace(requestArgs);

    return {ScopedMovePrimary(this, true, _activeMovePrimaryState->notification)};
}

boost::optional<NamespaceString> ActiveMovePrimariesRegistry::getActiveMovePrimaryNss() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_activeMovePrimaryState) {
        return _activeMovePrimaryState->requestArgs.get_shardsvrMovePrimary();
    }

    return boost::none;
}

void ActiveMovePrimariesRegistry::_clearMovePrimary() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_activeMovePrimaryState);
    _activeMovePrimaryState.reset();
}

Status ActiveMovePrimariesRegistry::ActiveMovePrimaryState::constructErrorStatus() const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream()
                << "Unable to start new movePrimary operation because this shard is currently "
                   "moving its primary for namespace "
                << requestArgs.get_shardsvrMovePrimary()->ns() << " to " << requestArgs.getTo()};
}

ScopedMovePrimary::ScopedMovePrimary(ActiveMovePrimariesRegistry* registry,
                                     bool shouldExecute,
                                     std::shared_ptr<Notification<Status>> completionNotification)
    : _registry(registry),
      _shouldExecute(shouldExecute),
      _completionNotification(std::move(completionNotification)) {}

ScopedMovePrimary::~ScopedMovePrimary() {
    if (_registry) {
        // If this is a movePrimary that didn't join an existing movePrimary, the caller must
        // signal on completion.
        invariant(_shouldExecute);
        invariant(*_completionNotification);
        _registry->_clearMovePrimary();
    }
}

ScopedMovePrimary::ScopedMovePrimary(ScopedMovePrimary&& other) {
    *this = std::move(other);
}

ScopedMovePrimary& ScopedMovePrimary::operator=(ScopedMovePrimary&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
        _shouldExecute = other._shouldExecute;
        _completionNotification = std::move(other._completionNotification);
    }

    return *this;
}

void ScopedMovePrimary::signalComplete(Status status) {
    invariant(_shouldExecute);
    _completionNotification->set(status);
}

Status ScopedMovePrimary::waitForCompletion(OperationContext* opCtx) {
    invariant(!_shouldExecute);
    return _completionNotification->get(opCtx);
}

}  // namespace mongo
