/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/active_rename_collection_registry.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"


namespace mongo {
namespace {

const auto getRegistry = ServiceContext::declareDecoration<ActiveRenameCollectionRegistry>();

bool checkDuplicateActiveRequest(const ShardsvrRenameCollection& activeRequest,
                                 const ShardsvrRenameCollection& newRequest) {
    if (activeRequest.getRenameCollection() != newRequest.getRenameCollection())
        return false;
    if (activeRequest.getTo() != newRequest.getTo())
        return false;
    if (activeRequest.getDropTarget() != newRequest.getDropTarget())
        return false;
    if (activeRequest.getStayTemp() != newRequest.getStayTemp())
        return false;
    return true;
}

}  // namespace

ActiveRenameCollectionRegistry::ActiveRenameCollectionRegistry() = default;

ActiveRenameCollectionRegistry::~ActiveRenameCollectionRegistry() {
    invariant(_activeRenameCollectionMap.empty());
}

ActiveRenameCollectionRegistry& ActiveRenameCollectionRegistry::get(ServiceContext* service) {
    return getRegistry(service);
}

ActiveRenameCollectionRegistry& ActiveRenameCollectionRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

StatusWith<ScopedRenameCollection> ActiveRenameCollectionRegistry::registerRenameCollection(
    const ShardsvrRenameCollection& request) {
    std::string nss = request.getRenameCollection().ns();

    auto iter = _activeRenameCollectionMap.find(nss);
    if (iter == _activeRenameCollectionMap.end()) {
        auto activeRenameCollectionState = std::make_shared<ActiveRenameCollectionState>(request);
        _activeRenameCollectionMap.try_emplace(nss, activeRenameCollectionState);

        return {ScopedRenameCollection(
            nss, this, true, activeRenameCollectionState->_promise.getFuture())};

    } else {
        auto activeRenameCollectionState = iter->second;

        if (checkDuplicateActiveRequest(activeRenameCollectionState->activeRequest, request)) {
            return {ScopedRenameCollection(
                nss, nullptr, false, activeRenameCollectionState->_promise.getFuture())};
        }

        return activeRenameCollectionState->constructErrorStatus(request);
    }
}

void ActiveRenameCollectionRegistry::_clearRenameCollection(std::string nss) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto iter = _activeRenameCollectionMap.find(nss);
    invariant(iter != _activeRenameCollectionMap.end());
    _activeRenameCollectionMap.erase(nss);
}

void ActiveRenameCollectionRegistry::_setEmptyOrError(std::string nss, Status status) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto iter = _activeRenameCollectionMap.find(nss);
    invariant(iter != _activeRenameCollectionMap.end());
    auto activeRenameCollectionState = iter->second;
    if (status.isOK()) {
        activeRenameCollectionState->_promise.emplaceValue();
    } else {
        activeRenameCollectionState->_promise.setError(status);
    }
}

Status ActiveRenameCollectionRegistry::ActiveRenameCollectionState::constructErrorStatus(
    const ShardsvrRenameCollection& request) const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Unable to rename collection " << request.getRenameCollection()};
}

ScopedRenameCollection::ScopedRenameCollection(std::string nss,
                                               ActiveRenameCollectionRegistry* registry,
                                               bool shouldExecute,
                                               SharedSemiFuture<void> future)
    : _nss(nss), _registry(registry), _shouldExecute(shouldExecute), _future(std::move(future)) {}

ScopedRenameCollection::~ScopedRenameCollection() {
    if (_registry && _shouldExecute) {
        _registry->_clearRenameCollection(_nss);
    }
}

ScopedRenameCollection::ScopedRenameCollection(ScopedRenameCollection&& other) {
    *this = std::move(other);
}

ScopedRenameCollection& ScopedRenameCollection::operator=(ScopedRenameCollection&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
        _shouldExecute = other._shouldExecute;
        _future = std::move(other._future);
        _nss = std::move(other._nss);
    }

    return *this;
}

void ScopedRenameCollection::emplaceStatus(Status status) {
    invariant(_shouldExecute);
    _registry->_setEmptyOrError(_nss, status);
}

SharedSemiFuture<void> ScopedRenameCollection::awaitExecution() {
    invariant(!_shouldExecute);
    return _future;
}

}  // namespace mongo
