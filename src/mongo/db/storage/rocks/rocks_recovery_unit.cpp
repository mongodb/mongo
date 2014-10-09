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

#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/write_batch_with_index.h>

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
            _destroyInternal();
        }
    }

    void RocksRecoveryUnit::beginUnitOfWork() {}

    void RocksRecoveryUnit::commitUnitOfWork() {
        invariant(!_destroyed);
        if ( !_writeBatch ) {
            // nothing to be committed
            return;
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
            _writeBatch.reset(new rocksdb::WriteBatchWithIndex(rocksdb::BytewiseComparator()));
        }

        return _writeBatch.get();
    }

    void RocksRecoveryUnit::registerChange(Change* change) { _changes.emplace_back(change); }

    void RocksRecoveryUnit::destroy() {
        _destroyed = true;
        _destroyInternal();
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

    void RocksRecoveryUnit::_destroyInternal() {
        if (_defaultCommit) {
            commitUnitOfWork();
        }

        for (auto& change : _changes) {
            change->rollback();
            delete change;
        }

        if (_snapshot) {
            _db->ReleaseSnapshot(_snapshot);
        }
    }
}
