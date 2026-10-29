// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/rss/replicated_storage_service.h"

namespace mongo::rss {
namespace {
const auto getReplicatedStorageService =
    ServiceContext::declareDecoration<ReplicatedStorageService>();
}  // namespace

ReplicatedStorageService& ReplicatedStorageService::get(ServiceContext* svcCtx) {
    return getReplicatedStorageService(svcCtx);
}

ReplicatedStorageService& ReplicatedStorageService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

PersistenceProvider& ReplicatedStorageService::getPersistenceProvider() {
    invariant(_provider);
    return *_provider;
}

const PersistenceProvider& ReplicatedStorageService::getPersistenceProvider() const {
    invariant(_provider);
    return *_provider;
}

void ReplicatedStorageService::setPersistenceProvider(std::unique_ptr<PersistenceProvider>&& p) {
    _provider = std::move(p);
}

PersistenceProvider& ReplicatedStorageService::getSpillPersistenceProvider() {
    invariant(_spillProvider);
    return *_spillProvider;
}
void ReplicatedStorageService::setSpillPersistenceProvider(
    std::unique_ptr<PersistenceProvider>&& p) {
    _spillProvider = std::move(p);
}

ServiceLifecycle& ReplicatedStorageService::getServiceLifecycle() {
    invariant(_lifecycle);
    return *_lifecycle;
}

void ReplicatedStorageService::setServiceLifecycle(std::unique_ptr<ServiceLifecycle>&& l) {
    _lifecycle = std::move(l);
}

}  // namespace mongo::rss
