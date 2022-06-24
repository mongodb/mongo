/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <memory>
#include <vector>

#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/repl/tenant_base_cloner.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/util/progress_meter.h"

namespace mongo::repl {

class TenantFileCloner final : public TenantBaseCloner {
public:
    struct Stats {
        std::string filePath;
        size_t fileSize;
        Date_t start;
        Date_t end;
        size_t receivedBatches{0};
        size_t writtenBatches{0};
        size_t bytesCopied{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    /**
     * Type of function to schedule file system tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleFsWorkFn = unique_function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        executor::TaskExecutor::CallbackFn)>;

    /**
     * Constructor for TenantFileCloner.
     *
     * remoteFileName: Path of file to copy on remote system.
     * remoteFileSize: Size of remote file in bytes, used for progress messages and stats only.
     * relativePath: Path of file relative to dbpath on the remote system, as a
     *               boost::filesystem::path generic path.
     */
    TenantFileCloner(const UUID& backupId,
                     const UUID& migrationId,
                     const std::string& remoteFileName,
                     size_t remoteFileSize,
                     const std::string& relativePath,
                     TenantMigrationSharedData* sharedData,
                     const HostAndPort& source,
                     DBClientConnection* client,
                     StorageInterface* storageInterface,
                     ThreadPool* dbPool);

    virtual ~TenantFileCloner() = default;

    /**
     * Waits for any file system work to finish or fail.
     */
    void waitForFilesystemWorkToComplete();

    Stats getStats() const;

    std::string toString() const;

protected:
    ClonerStages getStages() final;

    bool isMyFailPoint(const BSONObj& data) const final;

private:
    friend class TenantFileClonerTest;

    class TenantFileClonerQueryStage : public ClonerStage<TenantFileCloner> {
    public:
        TenantFileClonerQueryStage(std::string name,
                                   TenantFileCloner* cloner,
                                   ClonerRunFn stageFunc)
            : ClonerStage<TenantFileCloner>(name, cloner, stageFunc) {}

        bool checkSyncSourceValidityOnRetry() override {
            // Sync source validity is assured by the backup ID not existing if the sync source
            // is restarted or otherwise becomes invalid.
            return false;
        }

        bool isTransientError(const Status& status) override {
            if (isCursorError(status)) {
                return true;
            }
            return ErrorCodes::isRetriableError(status);
        }

        static bool isCursorError(const Status& status) {
            // Our cursor was killed on the sync source.
            if ((status == ErrorCodes::CursorNotFound) || (status == ErrorCodes::OperationFailed) ||
                (status == ErrorCodes::QueryPlanKilled)) {
                return true;
            }
            return false;
        }
    };

    std::string describeForFuzzer(BaseClonerStage* stage) const final {
        // We do not have a fuzzer for tenant backup file cloner.
        MONGO_UNREACHABLE;
    }

    /**
     * The preStage sets the begin time in _stats and makes sure the destination file
     * can be created.
     */
    void preStage() final;

    /**
     * The postStage sets the end time in _stats.
     */
    void postStage() final;

    /**
     * Stage function that executes a query to retrieve the file data.  For each
     * batch returned by the upstream node, handleNextBatch will be called with the data.  This
     * stage will finish when the entire query is finished or failed.
     */
    AfterStageBehavior queryStage();

    /**
     * Put all results from a query batch into a buffer, and schedule it to be written to disk.
     */
    void handleNextBatch(DBClientCursor& cursor);

    /**
     * Called whenever there is a new batch of documents ready from the DBClientConnection.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void writeDataToFilesystemCallback(const executor::TaskExecutor::CallbackArgs& cbd);

    /**
     * Sends an (aggregation) query command to the source. That query command with be parameterized
     * based on copy progress.
     */
    void runQuery();

    /**
     * Convenience call to get the file offset under a lock.
     */
    size_t getFileOffset();

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    const UUID _backupId;                    // (R)
    const UUID _migrationId;                 // (R)
    const std::string _remoteFileName;       // (R)
    size_t _remoteFileSize;                  // (R)
    const std::string _relativePathString;   // (R)
    boost::filesystem::path _localFilePath;  // (X)

    TenantFileClonerQueryStage _queryStage;  // (R)

    std::ofstream _localFile;  // (M)
    // File offset we will request from the remote side in the next query.
    off_t _fileOffset = 0;  // (M)
    bool _sawEof = false;   // (X)

    // Data read from source to insert.
    std::vector<BSONObj> _dataToWrite;  // (M)
    // Putting _fsWorkTaskRunner last ensures anything the database work threads depend on
    // like _dataToWrite, is destroyed after those threads exit.
    TaskRunner _fsWorkTaskRunner;  // (R)
    //  Function for scheduling filesystem work using the executor.
    ScheduleFsWorkFn _scheduleFsWorkFn;  // (R)

    ProgressMeter _progressMeter;  // (X) progress meter for this instance.
    Stats _stats;                  // (M)

    static constexpr int kProgressMeterSecondsBetween = 60;
    static constexpr int kProgressMeterCheckInterval = 128;
};

}  // namespace mongo::repl
