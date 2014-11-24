// rocks_transaction.h

/*
 * TODO(mongo) Add licence header
 */

#pragma once

#include <atomic>
#include <set>
#include <memory>
#include <string>

#include <boost/thread/mutex.hpp>

namespace mongo {
    class RocksTransaction;

    class RocksTransactionEngine {
    public:
        RocksTransactionEngine();

        uint64_t getLatestSeqId() const {
            return _latestSeqId.load(std::memory_order::memory_order_acquire);
        }

    private:
        uint64_t nextTransactionId() {
          return _nextTransactionId.fetch_add(1);
        }

        friend class RocksTransaction;
        std::atomic<uint64_t> _latestSeqId;
        std::atomic<uint64_t> _nextTransactionId;
        // Lock when mutating state here
        boost::mutex _commitLock;

        static const size_t kNumSeqIdShards = 1 << 20;
        // Slots to store latest update SeqID for documents.
        uint64_t _seqId[kNumSeqIdShards];
        uint64_t _uncommittedTransactionId[kNumSeqIdShards];
    };

    class RocksTransaction {
    public:
        RocksTransaction(RocksTransactionEngine* transactionEngine)
            : _snapshotSeqId(std::numeric_limits<uint64_t>::max()),
              _transactionId(transactionEngine->nextTransactionId()),
              _transactionEngine(transactionEngine) {}

        ~RocksTransaction() { abort(); }

        // returns true if OK
        // returns false on conflict
        bool registerWrite(uint64_t hash) ;

        void commit();

        void abort();

        void recordSnapshotId();

    private:
        friend class RocksTransactionEngine;
        uint64_t _snapshotSeqId;
        uint64_t _transactionId;
        RocksTransactionEngine* _transactionEngine;
        std::set<uint64_t> _writeShards;
    };
}
