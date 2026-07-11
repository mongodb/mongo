// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/server_recovery.h"

#include "mongo/util/decorable.h"

#include <mutex>
#include <string_view>
#include <utility>

#include <absl/container/node_hash_set.h>

namespace mongo {
namespace {
const auto getInReplicationRecovery = ServiceContext::declareDecoration<Atomic<int32_t>>();
const auto getSizeRecoveryState = ServiceContext::declareDecoration<SizeRecoveryState>();
}  // namespace

bool SizeRecoveryState::collectionNeedsSizeAdjustment(std::string_view ident) const {
    if (!InReplicationRecovery::isSet(getGlobalServiceContext())) {
        return true;
    }

    return collectionAlwaysNeedsSizeAdjustment(ident);
}

bool SizeRecoveryState::collectionAlwaysNeedsSizeAdjustment(std::string_view ident) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _collectionsAlwaysNeedingSizeAdjustment.count(ident) > 0;
}

void SizeRecoveryState::markCollectionAsAlwaysNeedsSizeAdjustment(std::string_view ident) {
    std::lock_guard<std::mutex> lock(_mutex);
    _collectionsAlwaysNeedingSizeAdjustment.insert(std::string{ident});
}

void SizeRecoveryState::clearStateBeforeRecovery() {
    std::lock_guard<std::mutex> lock(_mutex);
    _collectionsAlwaysNeedingSizeAdjustment.clear();
}

void SizeRecoveryState::setRecordStoresShouldAlwaysCheckSize(bool shouldAlwayCheckSize) {
    std::lock_guard<std::mutex> lock(_mutex);
    _recordStoresShouldAlwayCheckSize = shouldAlwayCheckSize;
}

bool SizeRecoveryState::shouldRecordStoresAlwaysCheckSize() const {
    std::lock_guard<std::mutex> lock(_mutex);
    // Regardless of whether the _recordStoresShouldAlwayCheckSize flag is set, if we are in
    // replication recovery then sizes should always be checked. This is in case the size storer
    // information is no longer accurate. This may be necessary if a collection creation was not
    // part of a stable checkpoint.
    return _recordStoresShouldAlwayCheckSize ||
        InReplicationRecovery::isSet(getGlobalServiceContext());
}

InReplicationRecovery::InReplicationRecovery(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {
    getInReplicationRecovery(_serviceContext).fetchAndAdd(1);
}

InReplicationRecovery::~InReplicationRecovery() {
    getInReplicationRecovery(_serviceContext).fetchAndSubtract(1);
}

bool InReplicationRecovery::isSet(ServiceContext* serviceContext) {
    return getInReplicationRecovery(serviceContext).load();
}
}  // namespace mongo

mongo::SizeRecoveryState& mongo::sizeRecoveryState(ServiceContext* serviceCtx) {
    return getSizeRecoveryState(serviceCtx);
}
