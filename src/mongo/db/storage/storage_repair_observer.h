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

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * A StorageRepairObserver is responsible for managing the state of the repair process. It
 * handles state transitions so that failed repairs are recoverable and so that replica set
 * corruption is not possible.
 * */
class StorageRepairObserver {
public:
    MONGO_DISALLOW_COPYING(StorageRepairObserver);

    explicit StorageRepairObserver(const std::string& dbpath);
    ~StorageRepairObserver() = default;

    static StorageRepairObserver* get(ServiceContext* service);
    static void set(ServiceContext* service, std::unique_ptr<StorageRepairObserver> repairObserver);

    /**
     * Notify the repair observer that a database repair operation is about to begin.
     *
     * Failure to call onRepairDone() afterward will leave the node in an incomplete repaired state,
     * and a subsequent restart will require to the node to run repair again.
     */
    void onRepairStarted();

    /**
     * Indicate that a data modification was made by repair. If even a single call is made, the
     * replica set configuration will be invalidated.
     *
     * Provide a 'description' of the modification that will added to a list of modifications by
     * getModifications();
     */
    void onModification(const std::string& description);

    /**
     * This must be called to notify the repair observer that a database repair operation completed
     * successfully. If any calls to onModification have been made, this invalidates the replica set
     * configuration so this node will be unable to rejoin a replica set.
     *
     * May only be called after a call to onRepairStarted().
     */
    void onRepairDone(OperationContext* opCtx);

    /**
     * Returns 'true' if this node is an incomplete repair state.
     */
    bool isIncomplete() const {
        return _repairState == RepairState::kIncomplete;
    }

    /**
     * Returns 'true' if this node is done with a repair operation.
     */
    bool isDone() const {
        return _repairState == RepairState::kDone;
    }

    /**
     * Returns 'true' if repair modified data.
     *
     * May only be called after a call to onRepairDone().
     */
    bool isDataModified() const {
        invariant(_repairState == RepairState::kIncomplete || _repairState == RepairState::kDone);
        return !_modifications.empty();
    }

    const std::vector<std::string>& getModifications() const {
        return _modifications;
    }


private:
    enum class RepairState {
        /**
         * No data has been modified, but the state of the replica set configuration is still
         * unknown. If the process were to exit in this state the server will be able to start up
         * normally, except if the replica set configuration is invalidated.
         */
        kPreStart,
        /**
         * Data is in the act of being repaired. Data may or may not have been modified by
         * repair, but if the process were to exit in this state, we do not know. The server should
         * require it not start normally without retrying a repair operation.
         */
        kIncomplete,
        /**
         * Repair has completed. The server can be started normally unless data was modified and the
         * server is started as a replica set.
         */
        kDone
    };

    void _touchRepairIncompleteFile();
    void _removeRepairIncompleteFile();
    void _invalidateReplConfigIfNeeded(OperationContext* opCtx);

    boost::filesystem::path _repairIncompleteFilePath;
    RepairState _repairState;
    std::vector<std::string> _modifications;
};

}  // namespace mongo
