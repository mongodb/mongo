// rocks_transaction.cpp

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

#include "mongo/db/storage/rocks/rocks_transaction.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>

// for invariant()
#include "mongo/util/assert_util.h"

namespace mongo {
    RocksTransactionEngine::RocksTransactionEngine() : _latestSeqId(1), _nextTransactionId(1) {}

    void RocksTransaction::commit() {
        if (_writeShards.empty()) {
            return;
        }
        uint64_t newSeqId = 0;
        {
            boost::mutex::scoped_lock lk(_transactionEngine->_commitLock);
            for (const auto& writeShard : _writeShards) {
                invariant(_transactionEngine->_seqId[writeShard] <= _snapshotSeqId);
                invariant(_transactionEngine->_uncommittedTransactionId[writeShard] ==
                          _transactionId);
                _transactionEngine->_uncommittedTransactionId.erase(writeShard);
            }
            newSeqId =
                _transactionEngine->_latestSeqId.load(std::memory_order::memory_order_relaxed) + 1;
            for (const auto& writeShard : _writeShards) {
                _transactionEngine->_seqId[writeShard] = newSeqId;
            }
            _transactionEngine->_latestSeqId.store(newSeqId);
        }
        // cleanup
        _snapshotSeqId = newSeqId;
        _writeShards.clear();
    }

    bool RocksTransaction::registerWrite(const std::string& id) {
        boost::mutex::scoped_lock lk(_transactionEngine->_commitLock);
        auto seqIdIter = _transactionEngine->_seqId.find(id);
        if (seqIdIter != _transactionEngine->_seqId.end() && seqIdIter->second > _snapshotSeqId) {
            // write-committed write conflict
            return false;
        }
        auto uncommittedTransactionIter = _transactionEngine->_uncommittedTransactionId.find(id);
        if (uncommittedTransactionIter != _transactionEngine->_uncommittedTransactionId.end() &&
            uncommittedTransactionIter->second != _transactionId) {
            // write-uncommitted write conflict
            return false;
        }
        _writeShards.insert(id);
        _transactionEngine->_uncommittedTransactionId[id] = _transactionId;
        return true;
    }

    void RocksTransaction::abort() {
        if (_writeShards.empty()) {
            return;
        }
        {
            boost::mutex::scoped_lock lk(_transactionEngine->_commitLock);
            for (const auto& writeShard : _writeShards) {
                _transactionEngine->_uncommittedTransactionId.erase(writeShard);
            }
        }
        _writeShards.clear();
    }

    void RocksTransaction::recordSnapshotId() {
        _snapshotSeqId = _transactionEngine->getLatestSeqId();
    }
}
