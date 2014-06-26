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
        : _db( db ), _defaultCommit( defaultCommit ), _writeBatch(  ), _depth( 0 ) {
        _writeBatch.reset( new rocksdb::WriteBatch() );
    }

    RocksRecoveryUnit::~RocksRecoveryUnit() {
        if ( _defaultCommit ) {
            commitUnitOfWork();
        }
    }

    void RocksRecoveryUnit::beginUnitOfWork() {
        if ( !_writeBatch ) {
            _writeBatch.reset( new rocksdb::WriteBatch() );
        }
        _depth++;
    }
    void RocksRecoveryUnit::commitUnitOfWork() {
        invariant( _writeBatch );

        rocksdb::Status status = _db->Write( rocksdb::WriteOptions(), _writeBatch.get() );
        if ( !status.ok() ) {
            log() << "uh oh: " << status.ToString();
            invariant( !"rocks write batch commit failed" );
        }

        _writeBatch.reset( new rocksdb::WriteBatch() );
    }

    void RocksRecoveryUnit::endUnitOfWork() {
        _depth--;
        invariant( _depth >= 0 );
        if ( _depth == 0 )
            commitUnitOfWork();
    }

    bool RocksRecoveryUnit::commitIfNeeded(bool force ) {
        if ( !isCommitNeeded() )
            return false;
        commitUnitOfWork();
        return true;
    }

    bool RocksRecoveryUnit::awaitCommit() {
        // TODO
        return true;
    }

    bool RocksRecoveryUnit::isCommitNeeded() const {
        return
            _writeBatch &&
            ( _writeBatch->GetDataSize() > ( 1024 * 1024 * 50 ) ||
              _writeBatch->Count() > 1000 );
    }

    void* RocksRecoveryUnit::writingPtr(void* data, size_t len) {
        warning() << "RocksRecoveryUnit::writingPtr doesn't work";
        return data;
    }

    void RocksRecoveryUnit::syncDataAndTruncateJournal() {
        log() << "RocksRecoveryUnit::syncDataAndTruncateJournal() does nothing";
    }

    rocksdb::WriteBatch* RocksRecoveryUnit::writeBatch() {
        invariant( _writeBatch );
        return _writeBatch.get();
    }

}
