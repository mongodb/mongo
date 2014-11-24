// rocks_transaction.cpp

/*
 * TODO(mongo) Add licence header
 */

#include "mongo/db/storage/rocks/rocks_transaction.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>

// for invariant()
#include "mongo/util/assert_util.h"

namespace mongo {
    RocksTransactionEngine::RocksTransactionEngine() : _latestSeqId(1) {
        for (size_t i = 0; i < kNumSeqIdShards; ++i) {
            _seqId[i] = 0;
            _uncommittedTransactionId[i] = 0;
        }
    }

    void RocksTransaction::commit() {
        if (_writeShards.empty()) {
            return;
        }
        uint64_t newSeqId = 0;
        {
            boost::mutex::scoped_lock lk(_transactionEngine->_commitLock);
            for (auto writeShard : _writeShards) {
                invariant(_transactionEngine->_seqId[writeShard] <= _snapshotSeqId);
                invariant(_transactionEngine->_uncommittedTransactionId[writeShard] ==
                          _transactionId);
                _transactionEngine->_uncommittedTransactionId[writeShard] = 0;
            }
            newSeqId =
                _transactionEngine->_latestSeqId.load(std::memory_order::memory_order_relaxed) + 1;
            for (auto writeShard : _writeShards) {
                _transactionEngine->_seqId[writeShard] = newSeqId;
            }
            _transactionEngine->_latestSeqId.store(newSeqId);
        }
        // cleanup
        _snapshotSeqId = newSeqId;
        _writeShards.clear();
    }

    bool RocksTransaction::registerWrite(uint64_t hash) {
        uint64_t shard = hash % RocksTransactionEngine::kNumSeqIdShards;

        boost::mutex::scoped_lock lk(_transactionEngine->_commitLock);
        if (_transactionEngine->_seqId[shard] > _snapshotSeqId) {
            // write-committed write conflict
            return false;
        }
        if (_transactionEngine->_uncommittedTransactionId[shard] != 0 &&
            _transactionEngine->_uncommittedTransactionId[shard] != _transactionId) {
            // write-uncommitted write conflict
            return false;
        }
        _writeShards.insert(shard);
        _transactionEngine->_uncommittedTransactionId[shard] = _transactionId;
        return true;
    }

    void RocksTransaction::abort() {
        if (_writeShards.empty()) {
            return;
        }
        {
            boost::mutex::scoped_lock lk(_transactionEngine->_commitLock);
            for (auto writeShard : _writeShards) {
                _transactionEngine->_uncommittedTransactionId[writeShard] = 0;
            }
        }
        _writeShards.clear();
    }

    void RocksTransaction::recordSnapshotId() {
        _snapshotSeqId = _transactionEngine->getLatestSeqId();
    }
}
