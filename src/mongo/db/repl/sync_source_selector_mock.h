/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

/**
 * Mock implementation of SyncSourceSelector interface for testing.
 */
class SyncSourceSelectorMock : public SyncSourceSelector {
    MONGO_DISALLOW_COPYING(SyncSourceSelectorMock);

public:
    using ChooseNewSyncSourceHook = stdx::function<void()>;

    SyncSourceSelectorMock();
    virtual ~SyncSourceSelectorMock();

    void clearSyncSourceBlacklist() override;
    HostAndPort chooseNewSyncSource(const OpTime& ot) override;
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override;
    bool shouldChangeSyncSource(const HostAndPort&, const rpc::ReplSetMetadata&) override;

    /**
     * Sets a function that will be run every time chooseNewSyncSource() is called.
     */
    void setChooseNewSyncSourceHook_forTest(const ChooseNewSyncSourceHook& hook);

    /**
     * Sets the result for subsequent chooseNewSyncSource() invocations.
     */
    void setChooseNewSyncSourceResult_forTest(const HostAndPort&);

    /**
     * Returns most recent optime passed to chooseNewSyncSource().
     */
    OpTime getChooseNewSyncSourceOpTime_forTest() const;

    /**
     * Returns most recently blacklisted sync source.
     */
    HostAndPort getLastBlacklistedSyncSource_forTest() const;

    /**
     * Returns the expiration associated with the most recently blacklisted sync source.
     */
    Date_t getLastBlacklistExpiration_forTest() const;

private:
    // This is the sync source that chooseNewSyncSource returns.
    HostAndPort _chooseNewSyncSourceResult = HostAndPort("localhost", -1);

    // This is the most recent optime passed to chooseNewSyncSource().
    OpTime _chooseNewSyncSourceOpTime;

    // This is run every time chooseNewSyncSource() is called.
    ChooseNewSyncSourceHook _chooseNewSyncSourceHook = []() {};

    // This is the most recently blacklisted sync source passed to blacklistSyncSource().
    HostAndPort _lastBlacklistedSyncSource;

    // This is the most recent 'util' argument value passed to blacklistSyncSource().
    Date_t _lastBlacklistExpiration;
};

}  // namespace repl
}  // namespace mongo
