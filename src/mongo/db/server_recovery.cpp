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

#include "mongo/db/server_recovery.h"

#include "mongo/base/string_data.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

#include <absl/container/node_hash_set.h>

namespace mongo {
namespace {
const auto getInReplicationRecovery = ServiceContext::declareDecoration<AtomicWord<int32_t>>();
const auto getSizeRecoveryState = ServiceContext::declareDecoration<SizeRecoveryState>();
}  // namespace

bool SizeRecoveryState::collectionNeedsSizeAdjustment(StringData ident) const {
    if (!InReplicationRecovery::isSet(getGlobalServiceContext())) {
        return true;
    }

    return collectionAlwaysNeedsSizeAdjustment(ident);
}

bool SizeRecoveryState::collectionAlwaysNeedsSizeAdjustment(StringData ident) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _collectionsAlwaysNeedingSizeAdjustment.count(ident) > 0;
}

void SizeRecoveryState::markCollectionAsAlwaysNeedsSizeAdjustment(StringData ident) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _collectionsAlwaysNeedingSizeAdjustment.insert(std::string{ident});
}

void SizeRecoveryState::clearStateBeforeRecovery() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _collectionsAlwaysNeedingSizeAdjustment.clear();
}

void SizeRecoveryState::setRecordStoresShouldAlwaysCheckSize(bool shouldAlwayCheckSize) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _recordStoresShouldAlwayCheckSize = shouldAlwayCheckSize;
}

bool SizeRecoveryState::shouldRecordStoresAlwaysCheckSize() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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
