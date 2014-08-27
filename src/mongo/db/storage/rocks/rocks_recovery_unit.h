// rocks_recovery_unit.h

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

#pragma once

#include <map>
#include <stack>
#include <string>

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/recovery_unit.h"

namespace rocksdb {
    class DB;
    class Snapshot;
    class WriteBatch;
}

namespace mongo {

    class RocksRecoveryUnit : public RecoveryUnit {
        MONGO_DISALLOW_COPYING(RocksRecoveryUnit);
    public:
        RocksRecoveryUnit( rocksdb::DB* db, bool defaultCommit );
        virtual ~RocksRecoveryUnit();

        virtual void beginUnitOfWork();
        virtual void commitUnitOfWork();

        virtual void endUnitOfWork();

        virtual bool commitIfNeeded(bool force = false);

        virtual bool awaitCommit();

        virtual void* writingPtr(void* data, size_t len);

        virtual void syncDataAndTruncateJournal();

        virtual void registerChange(Change* change);

        // local api

        rocksdb::WriteBatch* writeBatch();

        const rocksdb::Snapshot* snapshot();

    private:
        rocksdb::DB* _db; // not owned
        bool _defaultCommit;

        boost::scoped_ptr<rocksdb::WriteBatch> _writeBatch; // owned
        int _depth;

        // bare because we need to call ReleaseSnapshot when we're done with this
        const rocksdb::Snapshot* _snapshot; // owned
    };

}
