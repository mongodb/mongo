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

#include "mongo/db/storage/rocks/rocks_transaction.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>

// for invariant()
#include "mongo/util/assert_util.h"

namespace mongo {
    RocksTransactionEngine::RocksTransactionEngine() : _latestSnapshotId(1), _nextTransactionId(1) {}

    std::list<uint64_t>::iterator RocksTransactionEngine::_getLatestSnapshotId_inlock() {
        return _activeSnapshots.insert(_activeSnapshots.end(), _latestSnapshotId);
    }

    bool RocksTransactionEngine::_isKeyCommittedAfterSnapshot_inlock(const std::string& key,
                                                                     uint64_t snapshotId) {
        auto iter = _keyInfo.find(key);
        return iter != _keyInfo.end() && iter->second.first > snapshotId;
    }

    void RocksTransactionEngine::_registerCommittedKey_inlock(const std::string& key,
                                                              uint64_t newSnapshotId) {
        auto iter = _keyInfo.find(key);
        if (iter != _keyInfo.end()) {
            _keysSortedBySnapshot.erase(iter->second.second);
            _keyInfo.erase(iter);
        }

        auto listIter = _keysSortedBySnapshot.insert(_keysSortedBySnapshot.end(), {key, newSnapshotId});
        _keyInfo.insert({key, {newSnapshotId, listIter}});
    }

    void RocksTransactionEngine::_cleanUpKeysCommittedBeforeSnapshot_inlock(uint64_t snapshotId) {
        while (!_keysSortedBySnapshot.empty() && _keysSortedBySnapshot.begin()->second <= snapshotId) {
            auto keyInfoIter = _keyInfo.find(_keysSortedBySnapshot.begin()->first);
            invariant(keyInfoIter != _keyInfo.end());
            _keyInfo.erase(keyInfoIter);
            _keysSortedBySnapshot.pop_front();
        }
    }

    void RocksTransactionEngine::_cleanupSnapshot_inlock(
        const std::list<uint64_t>::iterator& snapshotIter) {
        bool needCleanup = _activeSnapshots.begin() == snapshotIter;
        _activeSnapshots.erase(snapshotIter);
        if (needCleanup) {
            _cleanUpKeysCommittedBeforeSnapshot_inlock(_activeSnapshots.empty()
                                                           ? std::numeric_limits<uint64_t>::max()
                                                           : *_activeSnapshots.begin());
        }
    }

    void RocksTransaction::commit() {
        if (_writtenKeys.empty()) {
            return;
        }
        uint64_t newSnapshotId = 0;
        {
            boost::lock_guard<boost::mutex> lk(_transactionEngine->_lock);
            for (const auto& key : _writtenKeys) {
                invariant(
                    !_transactionEngine->_isKeyCommittedAfterSnapshot_inlock(key, _snapshotId));
                invariant(_transactionEngine->_uncommittedTransactionId[key] == _transactionId);
                _transactionEngine->_uncommittedTransactionId.erase(key);
            }
            newSnapshotId = _transactionEngine->_latestSnapshotId + 1;
            for (const auto& key : _writtenKeys) {
                _transactionEngine->_registerCommittedKey_inlock(key, newSnapshotId);
            }
            _cleanup_inlock();
            _transactionEngine->_latestSnapshotId = newSnapshotId;
        }
        // cleanup
        _writtenKeys.clear();
    }

    bool RocksTransaction::registerWrite(const std::string& key) {
        boost::lock_guard<boost::mutex> lk(_transactionEngine->_lock);
        if (_transactionEngine->_isKeyCommittedAfterSnapshot_inlock(key, _snapshotId)) {
            // write-committed write conflict
            return false;
        }
        auto uncommittedTransactionIter = _transactionEngine->_uncommittedTransactionId.find(key);
        if (uncommittedTransactionIter != _transactionEngine->_uncommittedTransactionId.end() &&
            uncommittedTransactionIter->second != _transactionId) {
            // write-uncommitted write conflict
            return false;
        }
        _writtenKeys.insert(key);
        _transactionEngine->_uncommittedTransactionId[key] = _transactionId;
        return true;
    }

    void RocksTransaction::abort() {
        if (_writtenKeys.empty() && !_snapshotInitialized) {
            return;
        }
        {
            boost::lock_guard<boost::mutex> lk(_transactionEngine->_lock);
            for (const auto& key : _writtenKeys) {
                _transactionEngine->_uncommittedTransactionId.erase(key);
            }
            _cleanup_inlock();
        }
        _writtenKeys.clear();
    }

    void RocksTransaction::recordSnapshotId() {
        {
            boost::lock_guard<boost::mutex> lk(_transactionEngine->_lock);
            _cleanup_inlock();
            _activeSnapshotsIter = _transactionEngine->_getLatestSnapshotId_inlock();
        }
        _snapshotId = *_activeSnapshotsIter;
        _snapshotInitialized = true;
    }

    void RocksTransaction::_cleanup_inlock() {
        if (_snapshotInitialized) {
            _transactionEngine->_cleanupSnapshot_inlock(_activeSnapshotsIter);
            _snapshotInitialized = false;
            _snapshotId = std::numeric_limits<uint64_t>::max();
        }
    }
}
