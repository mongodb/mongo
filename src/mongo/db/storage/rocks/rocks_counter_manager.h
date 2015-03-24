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

#include <atomic>
#include <set>
#include <unordered_map>
#include <memory>
#include <string>
#include <list>

#include <boost/thread/mutex.hpp>

#include <rocksdb/db.h>
#include <rocksdb/slice.h>

#include "mongo/base/string_data.h"

namespace mongo {

    class RocksCounterManager {
    public:
        RocksCounterManager(rocksdb::DB* db, bool crashSafe)
            : _db(db), _crashSafe(crashSafe), _syncing(false), _syncCounter(0) {}

        long long loadCounter(const std::string& counterKey);

        void updateCounter(const std::string& counterKey, long long count,
                           rocksdb::WriteBatch* writeBatch);

        void sync();

        bool crashSafe() const { return _crashSafe; }

    private:
        static rocksdb::Slice _encodeCounter(long long counter, int64_t* storage);

        rocksdb::DB* _db; // not owned
        const bool _crashSafe;
        boost::mutex _lock;
        // protected by _lock
        bool _syncing;
        // protected by _lock
        std::unordered_map<std::string, long long> _counters;
        // protected by _lock
        int _syncCounter;

        static const int kSyncEvery = 10000;
    };

}
