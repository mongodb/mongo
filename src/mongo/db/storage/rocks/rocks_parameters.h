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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/server_parameters.h"

#include "rocks_engine.h"

namespace mongo {

    // To dynamically configure RocksDB's rate limit, run
    // db.adminCommand({setParameter:1, rocksdbRuntimeConfigMaxWriteMBPerSec:30})
    class RocksRateLimiterServerParameter : public ServerParameter {
        MONGO_DISALLOW_COPYING(RocksRateLimiterServerParameter);

    public:
        RocksRateLimiterServerParameter(RocksEngine* engine);
        virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name);
        virtual Status set(const BSONElement& newValueElement);
        virtual Status setFromString(const std::string& str);

    private:
        Status _set(int newNum);
        RocksEngine* _engine;
    };

    // We use mongo's setParameter() API to issue a backup request to rocksdb.
    // To backup entire RocksDB instance, call:
    // db.adminCommand({setParameter:1, rocksdbBackup: "/var/lib/mongodb/backup/1"})
    // The directory needs to be an absolute path. It should not exist -- it will be created
    // automatically.
    class RocksBackupServerParameter : public ServerParameter {
        MONGO_DISALLOW_COPYING(RocksBackupServerParameter);

    public:
        RocksBackupServerParameter(RocksEngine* engine);
        virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name);
        virtual Status set(const BSONElement& newValueElement);
        virtual Status setFromString(const std::string& str);

    private:
        RocksEngine* _engine;
    };

    // We use mongo's setParameter() API to issue a compact request to rocksdb.
    // To compact entire RocksDB instance, call:
    // db.adminCommand({setParameter:1, rocksdbCompact: 1})
    class RocksCompactServerParameter : public ServerParameter {
        MONGO_DISALLOW_COPYING(RocksCompactServerParameter);

    public:
        RocksCompactServerParameter(RocksEngine* engine);
        virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name);
        virtual Status set(const BSONElement& newValueElement);
        virtual Status setFromString(const std::string& str);

    private:
        RocksEngine* _engine;
    };
}
