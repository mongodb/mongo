// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/sync_source_selector_mock.h"

namespace mongo {
namespace repl {

SyncSourceSelectorMock::SyncSourceSelectorMock() {}

SyncSourceSelectorMock::~SyncSourceSelectorMock() {}

void SyncSourceSelectorMock::clearSyncSourceDenylist() {}

HostAndPort SyncSourceSelectorMock::chooseNewSyncSource(const OpTime& ot) {
    _chooseNewSyncSourceHook();
    _chooseNewSyncSourceOpTime = ot;
    return _chooseNewSyncSourceResult;
}

void SyncSourceSelectorMock::denylistSyncSource(const HostAndPort& host, Date_t until) {
    _lastDenylistedSyncSource = host;
    _lastDenylistExpiration = until;
}

void SyncSourceSelectorMock::setChooseNewSyncSourceHook_forTest(
    const ChooseNewSyncSourceHook& hook) {
    _chooseNewSyncSourceHook = hook;
}

ChangeSyncSourceAction SyncSourceSelectorMock::shouldChangeSyncSource(
    const HostAndPort&,
    const rpc::ReplSetMetadata&,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {
    return ChangeSyncSourceAction::kContinueSyncing;
}

ChangeSyncSourceAction SyncSourceSelectorMock::shouldChangeSyncSourceOnError(
    const HostAndPort&, const OpTime& lastOpTimeFetched) const {
    return ChangeSyncSourceAction::kContinueSyncing;
}

void SyncSourceSelectorMock::setChooseNewSyncSourceResult_forTest(const HostAndPort& syncSource) {
    _chooseNewSyncSourceResult = syncSource;
}

OpTime SyncSourceSelectorMock::getChooseNewSyncSourceOpTime_forTest() const {
    return _chooseNewSyncSourceOpTime;
}

HostAndPort SyncSourceSelectorMock::getLastDenylistedSyncSource_forTest() const {
    return _lastDenylistedSyncSource;
}

Date_t SyncSourceSelectorMock::getLastDenylistExpiration_forTest() const {
    return _lastDenylistExpiration;
}

}  // namespace repl
}  // namespace mongo
