// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/repl/initial_sync/base_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <string_view>

namespace mongo {
namespace repl {

class InitialSyncBaseCloner : public BaseCloner {
public:
    InitialSyncBaseCloner(std::string_view clonerName,
                          InitialSyncSharedData* sharedData,
                          const HostAndPort& source,
                          DBClientConnection* client,
                          StorageInterface* storageInterface,
                          ThreadPool* dbPool);
    ~InitialSyncBaseCloner() override = default;

    [[MONGO_MOD_PRIVATE]] int getRetryableOperationCount_forTest();

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
