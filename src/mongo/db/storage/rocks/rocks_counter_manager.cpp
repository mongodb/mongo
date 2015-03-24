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

#include "mongo/platform/basic.h"
#include "mongo/platform/endian.h"

#include "rocks_counter_manager.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>

// for invariant()
#include "mongo/util/assert_util.h"

#include <rocksdb/db.h>

#include "rocks_util.h"

namespace mongo {

    long long RocksCounterManager::loadCounter(const std::string& counterKey) {
        {
            boost::mutex::scoped_lock lk(_lock);
            auto itr = _counters.find(counterKey);
            if (itr != _counters.end()) {
                return itr->second;
            }
        }
        std::string value;
        auto s = _db->Get(rocksdb::ReadOptions(), counterKey, &value);
        if (s.IsNotFound()) {
            return 0;
        }
        invariantRocksOK(s);

        int64_t ret;
        invariant(sizeof(ret) == value.size());
        memcpy(&ret, value.data(), sizeof(ret));
        // we store counters in little endian
        return static_cast<long long>(endian::littleToNative(ret));
    }

    void RocksCounterManager::updateCounter(const std::string& counterKey, long long count,
                                            rocksdb::WriteBatch* writeBatch) {

        if (_crashSafe) {
            int64_t storage;
            writeBatch->Put(counterKey, _encodeCounter(count, &storage));
        } else {
            boost::mutex::scoped_lock lk(_lock);
            _counters[counterKey] = count;
            ++_syncCounter;
            if (!_syncing && _syncCounter >= kSyncEvery) {
                // let's sync this now. piggyback on writeBatch
                int64_t storage;
                for (const auto& counter : _counters) {
                    writeBatch->Put(counter.first, _encodeCounter(counter.second, &storage));
                }
                _counters.clear();
                _syncCounter = 0;
            }
        }
    }

    void RocksCounterManager::sync() {
        rocksdb::WriteBatch wb;
        {
            boost::mutex::scoped_lock lk(_lock);
            if (_syncing || _counters.size() == 0) {
                return;
            }
            int64_t storage;
            for (const auto& counter : _counters) {
                wb.Put(counter.first, _encodeCounter(counter.second, &storage));
            }
            _counters.clear();
            _syncCounter = 0;
            _syncing = true;
        }
        auto s = _db->Write(rocksdb::WriteOptions(), &wb);
        invariantRocksOK(s);
        {
            boost::mutex::scoped_lock lk(_lock);
            _syncing = false;
        }
    }

    rocksdb::Slice RocksCounterManager::_encodeCounter(long long counter, int64_t* storage) {
        *storage = static_cast<int64_t>(endian::littleToNative(counter));
        return rocksdb::Slice(reinterpret_cast<const char*>(storage), sizeof(*storage));
    }
}
