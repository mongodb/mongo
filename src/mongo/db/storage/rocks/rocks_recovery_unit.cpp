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

#include "mongo/platform/basic.h"
#include "mongo/platform/endian.h"

#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/base/checked_cast.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_transaction.h"
#include "mongo/util/log.h"

namespace mongo {
    namespace {
        class PrefixStrippingIterator : public rocksdb::Iterator {
        public:
            // baseIterator is consumed
            PrefixStrippingIterator(std::string prefix, Iterator* baseIterator)
                : _prefix(std::move(prefix)),
                  _prefixSlice(_prefix.data(), _prefix.size()),
                  _baseIterator(baseIterator) {}

            virtual bool Valid() const {
                return _baseIterator->Valid() && _baseIterator->key().starts_with(_prefixSlice);
            }

            virtual void SeekToFirst() { _baseIterator->Seek(_prefixSlice); }
            virtual void SeekToLast() {
                // next prefix lexicographically, assume same length
                std::string nextPrefix(_prefix);
                invariant(nextPrefix.size() > 0);
                for (size_t i = nextPrefix.size() - 1; i >= 0; ++i) {
                    nextPrefix[i]++;
                    // if it's == 0, that means we've overflowed, so need to keep adding
                    if (nextPrefix[i] != 0) {
                        break;
                    }
                }

                _baseIterator->Seek(nextPrefix);
                if (!_baseIterator->Valid()) {
                    _baseIterator->SeekToLast();
                }
                if (_baseIterator->Valid() && !_baseIterator->key().starts_with(_prefixSlice)) {
                    _baseIterator->Prev();
                }
            }

            virtual void Seek(const rocksdb::Slice& target) {
                std::unique_ptr<char[]> buffer(new char[_prefix.size() + target.size()]);
                memcpy(buffer.get(), _prefix.data(), _prefix.size());
                memcpy(buffer.get() + _prefix.size(), target.data(), target.size());
                _baseIterator->Seek(rocksdb::Slice(buffer.get(), _prefix.size() + target.size()));
            }

            virtual void Next() { _baseIterator->Next(); }
            virtual void Prev() { _baseIterator->Prev(); }

            virtual rocksdb::Slice key() const {
                rocksdb::Slice strippedKey = _baseIterator->key();
                strippedKey.remove_prefix(_prefix.size());
                return strippedKey;
            }
            virtual rocksdb::Slice value() const { return _baseIterator->value(); }
            virtual rocksdb::Status status() const { return _baseIterator->status(); }

        private:
            std::string _prefix;
            rocksdb::Slice _prefixSlice;
            std::unique_ptr<Iterator> _baseIterator;
        };

    }  // anonymous namespace

    RocksRecoveryUnit::RocksRecoveryUnit(RocksTransactionEngine* transactionEngine, rocksdb::DB* db,
                                         bool durable)
        : _transactionEngine(transactionEngine),
          _db(db),
          _durable(durable),
          _transaction(transactionEngine),
          _writeBatch(),
          _snapshot(NULL),
          _depth(0),
          _myTransactionCount(1) {}

    RocksRecoveryUnit::~RocksRecoveryUnit() {
        _abort();
    }

    void RocksRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
        _depth++;
    }

    void RocksRecoveryUnit::commitUnitOfWork() {
        if (_depth > 1) {
            return; // only outermost gets committed.
        }

        if (_writeBatch) {
            _commit();
        }

        try {
            for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
                (*it)->commit();
            }
            _changes.clear();
        }
        catch (...) {
            std::terminate();
        }

        _releaseSnapshot();
    }

    void RocksRecoveryUnit::endUnitOfWork() {
        _depth--;
        if (_depth == 0) {
            _abort();
        }
    }

    bool RocksRecoveryUnit::awaitCommit() {
        // TODO
        return true;
    }

    void RocksRecoveryUnit::commitAndRestart() {
        invariant( _depth == 0 );
        commitUnitOfWork();
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

    void RocksRecoveryUnit::setOplogReadTill(const RecordId& record) { _oplogReadTill = record; }

    void RocksRecoveryUnit::registerChange(Change* change) { _changes.push_back(change); }

    SnapshotId RocksRecoveryUnit::getSnapshotId() const { return SnapshotId(_myTransactionCount); }

    void RocksRecoveryUnit::_releaseSnapshot() {
        if (_snapshot) {
            _db->ReleaseSnapshot(_snapshot);
            _snapshot = nullptr;
        }
        _myTransactionCount++;
    }

    void RocksRecoveryUnit::_commit() {
        invariant(_writeBatch);
        for (auto pair : _deltaCounters) {
            auto& counter = pair.second;
            counter._value->fetch_add(counter._delta, std::memory_order::memory_order_relaxed);
            long long newValue = counter._value->load(std::memory_order::memory_order_relaxed);
            int64_t littleEndian = static_cast<int64_t>(endian::littleToNative(newValue));
            const char* nr_ptr = reinterpret_cast<const char*>(&littleEndian);
            writeBatch()->Put(pair.first, rocksdb::Slice(nr_ptr, sizeof(littleEndian)));
        }

        if (_writeBatch->GetWriteBatch()->Count() != 0) {
            // Order of operations here is important. It needs to be synchronized with
            // _transaction.recordSnapshotId() and _db->GetSnapshot() and
            rocksdb::WriteOptions writeOptions;
            writeOptions.disableWAL = !_durable;
            auto status = _db->Write(rocksdb::WriteOptions(), _writeBatch->GetWriteBatch());
            if (!status.ok()) {
                log() << "uh oh: " << status.ToString();
                invariant(!"rocks write batch commit failed");
            }
            _transaction.commit();
        }
        _deltaCounters.clear();
        _writeBatch.reset();
    }

    void RocksRecoveryUnit::_abort() {
        try {
            for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
                    it != end; ++it) {
                (*it)->rollback();
            }
            _changes.clear();
        }
        catch (...) {
            std::terminate();
        }

        _transaction.abort();
        _deltaCounters.clear();
        _writeBatch.reset();

        _releaseSnapshot();
    }

    const rocksdb::Snapshot* RocksRecoveryUnit::snapshot() {
        if ( !_snapshot ) {
            // Order of operations here is important. It needs to be synchronized with
            // _db->Write() and _transaction.commit()
            _transaction.recordSnapshotId();
            _snapshot = _db->GetSnapshot();
        }

        return _snapshot;
    }

    rocksdb::Status RocksRecoveryUnit::Get(const rocksdb::Slice& key, std::string* value) {
        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            boost::scoped_ptr<rocksdb::WBWIIterator> wb_iterator(_writeBatch->NewIterator());
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
        return _db->Get(options, key, value);
    }

    rocksdb::Iterator* RocksRecoveryUnit::NewIterator(std::string prefix) {
        rocksdb::ReadOptions options;
        options.snapshot = snapshot();
        auto iterator = _db->NewIterator(options);
        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            iterator = _writeBatch->NewIteratorWithBase(iterator);
        }
        return new PrefixStrippingIterator(std::move(prefix), iterator);
    }

    rocksdb::Iterator* RocksRecoveryUnit::NewIteratorNoSnapshot(rocksdb::DB* db,
                                                                std::string prefix) {
        auto iterator = db->NewIterator(rocksdb::ReadOptions());
        return new PrefixStrippingIterator(std::move(prefix), iterator);
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

    long long RocksRecoveryUnit::getCounterValue(rocksdb::DB* db, const rocksdb::Slice counterKey) {
        std::string value;
        auto s = db->Get(rocksdb::ReadOptions(), counterKey, &value);
        if (s.IsNotFound()) {
            return 0;
        } else if (!s.ok()) {
            log() << "Counter get failed " << s.ToString();
            invariant(!"Counter get failed");
        }

        int64_t ret;
        invariant(sizeof(ret) == value.size());
        memcpy(&ret, value.data(), sizeof(ret));
        // we store counters in little endian
        return static_cast<long long>(endian::littleToNative(ret));
    }

    RocksRecoveryUnit* RocksRecoveryUnit::getRocksRecoveryUnit(OperationContext* opCtx) {
        return checked_cast<RocksRecoveryUnit*>(opCtx->recoveryUnit());
    }

}
