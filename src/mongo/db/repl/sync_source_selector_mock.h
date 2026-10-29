// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

/**
 * Mock implementation of SyncSourceSelector interface for testing.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] SyncSourceSelectorMock : public SyncSourceSelector {
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
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void setChooseNewSyncSourceHook_forTest(
        const ChooseNewSyncSourceHook& hook);

    /**
     * Sets the result for subsequent chooseNewSyncSource() invocations.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void setChooseNewSyncSourceResult_forTest(const HostAndPort&);

    /**
     * Returns most recent optime passed to chooseNewSyncSource().
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] OpTime getChooseNewSyncSourceOpTime_forTest() const;

    /**
     * Returns most recently denylisted sync source.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] HostAndPort getLastDenylistedSyncSource_forTest() const;

    /**
     * Returns the expiration associated with the most recently denylisted sync source.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] Date_t getLastDenylistExpiration_forTest() const;

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
}  // namespace mongo
