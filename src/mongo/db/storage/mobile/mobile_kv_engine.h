/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <memory>

#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mobile/mobile_session_pool.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {

class JournalListener;

class MobileKVEngine : public KVEngine {
public:
    MobileKVEngine(const std::string& path);

    RecoveryUnit* newRecoveryUnit() override;

    Status createRecordStore(OperationContext* opCtx,
                             StringData ns,
                             StringData ident,
                             const CollectionOptions& options) override;

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                StringData ns,
                                                StringData ident,
                                                const CollectionOptions& options) override;

    Status createSortedDataInterface(OperationContext* opCtx,
                                     StringData ident,
                                     const IndexDescriptor* desc) override;

    SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                StringData ident,
                                                const IndexDescriptor* desc) override;

    Status beginBackup(OperationContext* opCtx) override {
        return Status::OK();
    }

    void endBackup(OperationContext* opCtx) override {}

    Status dropIdent(OperationContext* opCtx, StringData ident) override;

    bool supportsDocLocking() const override {
        return false;
    }

    bool supportsDBLocking() const override {
        return false;
    }

    bool supportsDirectoryPerDB() const override {
        return false;
    }

    bool isDurable() const override {
        return true;
    }

    /**
     * Flush is a no-op since SQLite transactions are durable by default after each commit.
     */
    int flushAllFiles(OperationContext* opCtx, bool sync) override {
        return 0;
    }

    bool isEphemeral() const override {
        return false;
    }

    int64_t getIdentSize(OperationContext* opCtx, StringData ident) override;

    Status repairIdent(OperationContext* opCtx, StringData ident) override {
        return Status::OK();
    }

    void cleanShutdown() override{};

    bool hasIdent(OperationContext* opCtx, StringData ident) const override;

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const override;

    void setJournalListener(JournalListener* jl) override {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _journalListener = jl;
    }

    virtual Timestamp getAllCommittedTimestamp() const override {
        MONGO_UNREACHABLE;
    }

private:
    mutable stdx::mutex _mutex;
    void _initDBPath(const std::string& path);

    std::unique_ptr<MobileSessionPool> _sessionPool;

    // Notified when we write as everything is considered "journalled" since repl depends on it.
    JournalListener* _journalListener = &NoOpJournalListener::instance;

    std::string _path;
};
}  // namespace mongo
