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

#include "mongo/util/timer.h"

namespace mongo {

    class RocksCompactionScheduler {
    public:
        RocksCompactionScheduler(rocksdb::DB* db) : _db(db) { _timer.reset(); }

        static int getSkippedDeletionsThreshold() { return kSkippedDeletionsThreshold; }

        void reportSkippedDeletionsAboveThreshold(const std::string& prefix);

    private:
        rocksdb::DB* _db;  // not owned

        boost::mutex _lock;
        // protected by _lock
        Timer _timer;

        // Don't trigger compactions more often than every 10min
        static const int kMinCompactionIntervalMins = 10;
        // We'll compact the prefix if any operation on the prefix reports more than 50.000
        // deletions it had to skip over (this is about 10ms extra overhead)
        static const int kSkippedDeletionsThreshold = 50000;
    };
}
