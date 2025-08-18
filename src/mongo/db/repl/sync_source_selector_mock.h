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

#pragma once

#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>

namespace MONGO_MOD_PUB mongo {
namespace repl {

/**
 * Mock implementation of SyncSourceSelector interface for testing.
 */
class SyncSourceSelectorMock : public SyncSourceSelector {
    SyncSourceSelectorMock(const SyncSourceSelectorMock&) = delete;
    SyncSourceSelectorMock& operator=(const SyncSourceSelectorMock&) = delete;

public:
    using ChooseNewSyncSourceHook = std::function<void()>;

    SyncSourceSelectorMock();
    ~SyncSourceSelectorMock() override;

    void clearSyncSourceDenylist() override;
    HostAndPort chooseNewSyncSource(const OpTime& ot) override;
    void denylistSyncSource(const HostAndPort& host, Date_t until) override;
    ChangeSyncSourceAction shouldChangeSyncSource(const HostAndPort&,
                                                  const rpc::ReplSetMetadata&,
                                                  const rpc::OplogQueryMetadata& oqMetadata,
                                                  const OpTime& previousOpTimeFetched,
                                                  const OpTime& lastOpTimeFetched) const override;

    ChangeSyncSourceAction shouldChangeSyncSourceOnError(
        const HostAndPort&, const OpTime& lastOpTimeFetched) const override;

    /**
     * Sets a function that will be run every time chooseNewSyncSource() is called.
     */
    MONGO_MOD_NEEDS_REPLACEMENT void setChooseNewSyncSourceHook_forTest(
        const ChooseNewSyncSourceHook& hook);

    /**
     * Sets the result for subsequent chooseNewSyncSource() invocations.
     */
    MONGO_MOD_NEEDS_REPLACEMENT void setChooseNewSyncSourceResult_forTest(const HostAndPort&);

    /**
     * Returns most recent optime passed to chooseNewSyncSource().
     */
    MONGO_MOD_NEEDS_REPLACEMENT OpTime getChooseNewSyncSourceOpTime_forTest() const;

    /**
     * Returns most recently denylisted sync source.
     */
    MONGO_MOD_NEEDS_REPLACEMENT HostAndPort getLastDenylistedSyncSource_forTest() const;

    /**
     * Returns the expiration associated with the most recently denylisted sync source.
     */
    MONGO_MOD_NEEDS_REPLACEMENT Date_t getLastDenylistExpiration_forTest() const;

private:
    // This is the sync source that chooseNewSyncSource returns.
    HostAndPort _chooseNewSyncSourceResult = HostAndPort("localhost", -1);

    // This is the most recent optime passed to chooseNewSyncSource().
    OpTime _chooseNewSyncSourceOpTime;

    // This is run every time chooseNewSyncSource() is called.
    ChooseNewSyncSourceHook _chooseNewSyncSourceHook = []() {
    };

    // This is the most recently denylisted sync source passed to denylistSyncSource().
    HostAndPort _lastDenylistedSyncSource;

    // This is the most recent 'util' argument value passed to denylistSyncSource().
    Date_t _lastDenylistExpiration;
};

}  // namespace repl
}  // namespace MONGO_MOD_PUB mongo
