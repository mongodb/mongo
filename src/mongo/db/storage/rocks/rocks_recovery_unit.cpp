// rocks_recovery_unit.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

namespace mongo {

    RocksRecoveryUnit::RocksRecoveryUnit(rocksdb::DB* db, bool defaultCommit)
        : _db(db),
          _defaultCommit(defaultCommit),
          _writeBatch(),
          _snapshot(NULL),
          _destroyed(false) {}

    RocksRecoveryUnit::~RocksRecoveryUnit() {
        if (!_destroyed) {
            destroy();
        }
    }

    void RocksRecoveryUnit::beginUnitOfWork() {}

    void RocksRecoveryUnit::commitUnitOfWork() {
        invariant(!_destroyed);
        if ( !_writeBatch ) {
            // nothing to be committed
            return;
        }

        for (auto pair : _deltaCounters) {
            auto& counter = pair.second;
            counter._value->fetch_add(counter._delta, std::memory_order::memory_order_relaxed);
            long long newValue = counter._value->load(std::memory_order::memory_order_relaxed);

            // TODO: make the encoding platform indepdent.
            const char* nr_ptr = reinterpret_cast<char*>(&newValue);
            writeBatch()->Put(pair.first, rocksdb::Slice(nr_ptr, sizeof(long long)));
        }

        rocksdb::Status status = _db->Write(rocksdb::WriteOptions(), _writeBatch->GetWriteBatch());
        if ( !status.ok() ) {
            log() << "uh oh: " << status.ToString();
            invariant( !"rocks write batch commit failed" );
        }

        for (auto& change : _changes) {
            change->commit();
            delete change;
        }
        _changes.clear();
        _deltaCounters.clear();
        _writeBatch.reset();

        if ( _snapshot ) {
            _db->ReleaseSnapshot( _snapshot );
            _snapshot = _db->GetSnapshot();
        }
    }

    void RocksRecoveryUnit::endUnitOfWork() {}

    bool RocksRecoveryUnit::awaitCommit() {
        // TODO
        return true;
    }

    void* RocksRecoveryUnit::writingPtr(void* data, size_t len) {
        warning() << "RocksRecoveryUnit::writingPtr doesn't work";
        return data;
    }

    void RocksRecoveryUnit::syncDataAndTruncateJournal() {
        log() << "RocksRecoveryUnit::syncDataAndTruncateJournal() does nothing";
    }

    // lazily initialized because Recovery Units are sometimes initialized just for reading,
    // which does not require write batches
    rocksdb::WriteBatchWithIndex* RocksRecoveryUnit::writeBatch() {
        if (!_writeBatch) {
            // this assumes that default column family uses default comparator. change this if you
            // change default column family's comparator
            _writeBatch.reset(
                new rocksdb::WriteBatchWithIndex(rocksdb::BytewiseComparator(), 0, true));
        }

        return _writeBatch.get();
    }

    void RocksRecoveryUnit::registerChange(Change* change) { _changes.emplace_back(change); }

    void RocksRecoveryUnit::destroy() {
        if (_defaultCommit) {
            commitUnitOfWork();
        } else {
            _deltaCounters.clear();
        }

        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            for (auto& change : _changes) {
                change->rollback();
                delete change;
            }
        }

        releaseSnapshot();
        _destroyed = true;
    }

    // XXX lazily initialized for now
    // This is lazily initialized for simplicity so long as we still
    // have database-level locking. If a method needs to access the snapshot,
    // and it has not been initialized, then it knows it is the first
    // method to access the snapshot, and can initialize it before using it.
    const rocksdb::Snapshot* RocksRecoveryUnit::snapshot() {
        if ( !_snapshot ) {
            _snapshot = _db->GetSnapshot();
        }

        return _snapshot;
    }

    void RocksRecoveryUnit::releaseSnapshot() {
        if (_snapshot) {
            _db->ReleaseSnapshot(_snapshot);
            _snapshot = nullptr;
        }
    }

    rocksdb::Status RocksRecoveryUnit::Get(rocksdb::ColumnFamilyHandle* columnFamily,
                                           const rocksdb::Slice& key, std::string* value) {
        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            boost::scoped_ptr<rocksdb::WBWIIterator> wb_iterator(
                _writeBatch->NewIterator(columnFamily));
            wb_iterator->Seek(key);
            if (wb_iterator->Valid() && wb_iterator->Entry().key == key) {
                const auto& entry = wb_iterator->Entry();
                if (entry.type == rocksdb::WriteType::kDeleteRecord) {
                    return rocksdb::Status::NotFound();
                }
                // TODO avoid double copy
                *value = std::string(entry.value.data(), entry.value.size());
                return rocksdb::Status::OK();
            }
        }
        rocksdb::ReadOptions options;
        options.snapshot = snapshot();
        return _db->Get(options, columnFamily, key, value);
    }

    rocksdb::Iterator* RocksRecoveryUnit::NewIterator(rocksdb::ColumnFamilyHandle* columnFamily) {
        invariant(columnFamily != _db->DefaultColumnFamily());

        rocksdb::ReadOptions options;
        options.snapshot = snapshot();
        auto iterator = _db->NewIterator(options, columnFamily);
        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            iterator = _writeBatch->NewIteratorWithBase(columnFamily, iterator);
        }
        return iterator;
    }

    void RocksRecoveryUnit::incrementCounter(const rocksdb::Slice& counterKey,
                                             std::atomic<long long>* counter, long long delta) {
        if (delta == 0) {
            return;
        }

        auto pair = _deltaCounters.find(counterKey.ToString());
        if (pair == _deltaCounters.end()) {
            _deltaCounters[counterKey.ToString()] =
                mongo::RocksRecoveryUnit::Counter(counter, delta);
        } else {
            pair->second._delta += delta;
        }
    }

    long long RocksRecoveryUnit::getDeltaCounter(const rocksdb::Slice& counterKey) {
        auto counter = _deltaCounters.find(counterKey.ToString());
        if (counter == _deltaCounters.end()) {
            return 0;
        } else {
            return counter->second._delta;
        }
    }

    RocksRecoveryUnit* RocksRecoveryUnit::getRocksRecoveryUnit(OperationContext* opCtx) {
        return dynamic_cast<RocksRecoveryUnit*>(opCtx->recoveryUnit());
    }

}
