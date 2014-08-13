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

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include "mongo/util/log.h"

namespace mongo {

    RocksRecoveryUnit::RocksRecoveryUnit( rocksdb::DB* db, bool defaultCommit )
                                       : _db( db ),
                                       _defaultCommit( defaultCommit ),
                                       _writeBatch(  ),
                                       _depth( 0 ),
                                       _snapshot( NULL ) { }


    RocksRecoveryUnit::~RocksRecoveryUnit() {
        if ( _defaultCommit ) {
            commitUnitOfWork();
        }

        if ( _snapshot ) {
            _db->ReleaseSnapshot( _snapshot );
        }
    }

    void RocksRecoveryUnit::beginUnitOfWork() {
        _depth++;
    }

    void RocksRecoveryUnit::commitUnitOfWork() {
        if ( !_writeBatch ) {
            // nothing to be committed
            return;
        }

        rocksdb::Status status = _db->Write( rocksdb::WriteOptions(), _writeBatch.get() );
        if ( !status.ok() ) {
            log() << "uh oh: " << status.ToString();
            invariant( !"rocks write batch commit failed" );
        }

        _writeBatch->Clear();

        if ( _snapshot ) {
            _db->ReleaseSnapshot( _snapshot );
            _snapshot = _db->GetSnapshot();
        }
    }

    void RocksRecoveryUnit::endUnitOfWork() {
        _depth--;
        invariant( _depth >= 0 );
        if ( _depth == 0 )
            commitUnitOfWork();
    }

    bool RocksRecoveryUnit::commitIfNeeded(bool force ) {
        commitUnitOfWork();
        return true;
    }

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
    rocksdb::WriteBatch* RocksRecoveryUnit::writeBatch() {
        if ( !_writeBatch ) {
            _writeBatch.reset( new rocksdb::WriteBatch() );
        }

        return _writeBatch.get();
    }

    void RocksRecoveryUnit::registerChange(Change* change) {
        // without rollbacks enabled, this is fine.
        change->commit();
        delete change;
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

}
