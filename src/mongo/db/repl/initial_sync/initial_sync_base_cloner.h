/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/repl/initial_sync/base_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <string>

namespace mongo {
namespace repl {

class InitialSyncBaseCloner : public BaseCloner {
public:
    InitialSyncBaseCloner(StringData clonerName,
                          InitialSyncSharedData* sharedData,
                          const HostAndPort& source,
                          DBClientConnection* client,
                          StorageInterface* storageInterface,
                          ThreadPool* dbPool);
    ~InitialSyncBaseCloner() override = default;

    MONGO_MOD_PRIVATE int getRetryableOperationCount_forTest();

protected:
    InitialSyncSharedData* getSharedData() const final {
        return checked_cast<InitialSyncSharedData*>(BaseCloner::getSharedData());
    }

    /**
     * Clears _retryableOp.
     */
    void clearRetryingState() final;

    /**
     * Returns true if we should use raw data operations during cloning.
     * If available, they must be used in order to clone viewless timeseries collections.
     * TODO(SERVER-101595): This method always returns true once 9.0 becomes lastLTS.
     */
    bool shouldUseRawDataOperations();

private:
    /**
     * Make sure the initial sync ID on the sync source has not changed.  Throws an exception
     * if it has.  Returns a not-OK status if a network error occurs.
     */
    Status checkInitialSyncIdIsUnchanged();

    /**
     * Make sure the rollback ID has not changed.  Throws an exception if it has.  Returns
     * a not-OK status if a network error occurs.
     */
    Status checkRollBackIdIsUnchanged();

    /**
     * Does validity checks on the sync source.  If the sync source is now no longer usable,
     * throws an exception. Returns a not-OK status if a network error occurs or if the sync
     * source is temporarily unusable (e.g. restarting).
     */
    Status checkSyncSourceIsStillValid();

    /**
     * Checks to see if we are still within our allowed outage duration.
     * Also probes the sync source for clone-fatal conditions, such as rollback.
     */
    void handleStageAttemptFailed(BaseClonerStage* stage, Status lastError) override;

    /**
     * Allows the initial sync fuzzer to pause cloner execution at specific points.
     */
    void pauseForFuzzer(BaseClonerStage* stage) final;

    /**
     * Provides part of a log message for the initial sync describing the namespace the
     * cloner is operating on.  It must start with the database name, followed by the
     * string ' db: { ', followed by the stage name, followed by ': ' and the collection UUID
     * if known.
     */
    std::string describeForFuzzer(BaseClonerStage*) const override = 0;

    /**
     * Overriden to allow the BaseCloner to use the initial sync log component.
     */
    logv2::LogComponent getLogComponent() final;

    // Operation that may currently be retrying.
    InitialSyncSharedData::RetryableOperation _retryableOp;
};

}  // namespace repl
}  // namespace mongo
