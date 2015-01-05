// rocks_transaction.h

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

namespace mongo {
    class RocksTransaction;

    class RocksTransactionEngine {
    public:
        RocksTransactionEngine();

    private:
        // REQUIRES: transaction engine lock locked
        std::list<uint64_t>::iterator _getLatestSnapshotId_inlock();

        // REQUIRES: transaction engine lock locked
        // Cleans up the snapshot from the _activeSnapshots list
        void _cleanupSnapshot_inlock(const std::list<uint64_t>::iterator& snapshotIter);

        uint64_t _getNextTransactionId() {
          return _nextTransactionId.fetch_add(1);
        }

        // returns true if the key was committed after the snapshotId, thus causing a write
        // conflict
        // REQUIRES: transaction engine lock locked
        bool _isKeyCommittedAfterSnapshot_inlock(const std::string& key, uint64_t snapshotId);

        // REQUIRES: transaction engine lock locked
        void _registerCommittedKey_inlock(const std::string& key, uint64_t newSnapshotId);

        // REQUIRES: transaction engine lock locked
        void _cleanUpKeysCommittedBeforeSnapshot_inlock(uint64_t snapshotId);

        friend class RocksTransaction;
        uint64_t _latestSnapshotId;
        std::atomic<uint64_t> _nextTransactionId;
        // Lock when mutating state here
        boost::mutex _lock;

        // The following data structures keep information about when were the keys committed.
        // They can answer the following questions:
        // * Which stored committed key has the earliest snapshot
        // * When was a certain key committed
        // _keysSortedBySnapshot is a list of {key, sequence_id} and it's sorted by the sequence_id.
        // Committing keys are always monotonically increasing, so to keep it sorted we just need to
        // push to the end.
        // _keyInfo is a map from the key to the two-part information about the key:
        // * snapshot ID of the last commit to this key
        // * an iterator pointing to the corresponding entry in _keysSortedBySnapshot. This is used
        // to update the list at the same time as we update the _keyInfo
        // TODO optimize these structures to store only one key instead of two
        typedef std::list<std::pair<std::string, uint64_t>> KeysSortedBySnapshotList;
        typedef std::list<std::pair<std::string, uint64_t>>::iterator KeysSortedBySnapshotListIter;
        KeysSortedBySnapshotList _keysSortedBySnapshot;
        // map of key -> pair{seq_id, pointer to corresponding _keysSortedBySnapshot}
        std::unordered_map<std::string, std::pair<uint64_t, KeysSortedBySnapshotListIter>> _keyInfo;

        std::unordered_map<std::string, uint64_t> _uncommittedTransactionId;

        // this list is sorted
        std::list<uint64_t> _activeSnapshots;
    };

    class RocksTransaction {
    public:
        RocksTransaction(RocksTransactionEngine* transactionEngine)
            : _snapshotInitialized(false),
              _snapshotId(std::numeric_limits<uint64_t>::max()),
              _transactionId(transactionEngine->_getNextTransactionId()),
              _transactionEngine(transactionEngine) {}

        ~RocksTransaction() { abort(); }

        // returns true if OK
        // returns false on conflict
        bool registerWrite(const std::string& key);

        void commit();

        void abort();

        void recordSnapshotId();

    private:
        // REQUIRES: transaction engine lock locked
        void _cleanup_inlock();

        friend class RocksTransactionEngine;
        bool _snapshotInitialized;
        uint64_t _snapshotId;
        std::list<uint64_t>::iterator _activeSnapshotsIter;
        uint64_t _transactionId;
        RocksTransactionEngine* _transactionEngine;
        std::set<std::string> _writtenKeys;
    };
}
