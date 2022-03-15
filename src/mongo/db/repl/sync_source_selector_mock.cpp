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
