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

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/platform/compiler.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/timer.h"

#include "mongo/bson/util/atomic_int.h"

/*
 * LockManager controls access to resources through two functions: acquire and release
 *
 * Resources can be databases, collections, oplogs, records, btree-nodes, key-value pairs,
 * forward/backward pointers, or anything at all that can be unambiguously identified.
 *
 * Resources are acquired by Transactions for either shared or exclusive use. If an
 * acquisition request conflicts with a pre-existing use of a resource, the requesting
 * transaction will block until the original conflicting requests and any new conflicting
 * requests have been released.
 *
 * Contention for a resource is resolved by a LockingPolicy, which determines which blocked
 * resource requests to awaken when the blocker releases the resource.
 *
 */

namespace mongo {

    class ResourceId {
    public:
        ResourceId() : _rid(0) { }
        ResourceId(size_t rid) : _rid(rid) { }
        bool operator<(const ResourceId& other) const { return _rid < other._rid; }
        bool operator==(const ResourceId& other) const { return _rid == other._rid; }
        operator size_t() const { return _rid; }

    private:
        size_t _rid;
    };
    static const ResourceId kReservedResourceId = 0;


    /**
     * LockModes: shared and exclusive
     */
    enum LockMode {
        kShared = 0, // conflicts only with kExclusive
        kExclusive = 1, // conflicts with all lock modes
        kInvalid
    };


    class Transaction;

    /**
     * Data structure used to record a resource acquisition request
     */
    class LockRequest {
    public:
        LockRequest(const ResourceId& resId,
                    const LockMode& mode,
                    Transaction* requestor,
                    bool heapAllocated = false);


        ~LockRequest();

        bool matches(const Transaction* tx,
                     const LockMode& mode,
                     const ResourceId& resId) const;

        bool isBlocked() const;
        bool shouldAwake();

        std::string toString() const;

        // insert/append in resource chain
        void insert(LockRequest* lr);
        void append(LockRequest* lr);

        // transaction that made this request (not owned)
        Transaction* requestor;

        // shared or exclusive use
        const LockMode mode;

        // resource requested
        const ResourceId resId;

        // a hash of resId modulo kNumResourcePartitions
        // used to mitigate cost of mutex locking
        unsigned slice;

        // number of times a tx requested resource in this mode
        // lock request will be deleted when count goes to 0
        size_t count;

        // number of existing things blocking this request
        // usually preceding requests on the queue, but also policy
        size_t sleepCount;

        // ResourceLock classes (see below) using the RAII pattern
        // allocate LockRequests on the stack.
        bool heapAllocated;

        // lock requests are chained by their resource
        LockRequest* nextOnResource;
        LockRequest* prevOnResource;

        // lock requests are also chained by their requesting transaction
        LockRequest* nextOfTransaction;
        LockRequest* prevOfTransaction;

        // used for waiting and waking
        boost::condition_variable lock;
    };

    /**
     * Data structure used to describe resource requestors,
     * used for conflict resolution, deadlock detection, and abort
     */
    class Transaction {
    public:
        Transaction(unsigned txId=0, int priority=0);
        ~Transaction();

        /**
         * transactions are identified by an id
         */
        Transaction* setTxIdOnce(unsigned txId);
        unsigned getTxId() const { return _txId; }

        /**
         * override default LockManager's default Policy for a transaction.
         *
         * positive priority moves transaction's resource requests toward the front
         * of the queue, behind only those requests with higher priority.
         *
         * negative priority moves transaction's resource requests toward the back
         * of the queue, ahead of only those requests with lower priority.
         *
         * zero priority uses the LockManager's default Policy
         */
        void setPriority(int newPriority);
        int getPriority() const;

        /**
         * maintain the queue of lock requests made by the transaction
         */
        void removeLock(LockRequest* lr);
	void addLock(LockRequest* lr);

        /**
         * should be age of the transaction.  currently using txId as a proxy.
         */
        bool operator<(const Transaction& other);

        /**
         * for debug
         */
        std::string toString() const;

    private:
        friend class LockManager;
        friend class LockRequest;

        void _addWaiter(Transaction* waiter);

        /**
         * it might be useful to reject lock manager requests from inactive TXs.
         */
        enum TxState {
            kInvalid,
            kActive,
            kAborted,
            kCommitted
        };

        // uniquely identify the transaction
        unsigned _txId;
#ifdef REGISTER_TRANSACTIONS
        // for mutex parallelism while handling transactions
        unsigned _txSlice;
#endif

        // transaction priorities:
        //     0 => neutral, use LockManager's default _policy
        //     + => high, queue forward
        //     - => low, queue back
        //
        int _priority;

        // LockManager doesnt accept new requests from completed transactions
        TxState _state;

        // For cleanup and abort processing, references all LockRequests made by a transaction
        LockRequest* _locks;

        // For deadlock detection: the set of transactions blocked by another transaction
        // NB: a transaction can only be directly waiting for a single resource/transaction
        // but to facilitate deadlock detection, if T1 is waiting for T2 and T2 is waiting
        // for T3, then both T1 and T2 are listed as T3's waiters.
        //
        // This is a multiset to handle some obscure situations.  If T1 has upgraded or downgraded
        // its lock on a resource, it has two lock requests.  If T2 then requests exclusive
        // access to the same resource, it must wait for BOTH T1's locks to be relased.
        //
        // the max size of the set is ~2*number-concurrent-transactions.  the set is only
        // consulted/updated when there's a lock conflict.  When there are many more documents
        // than transactions, the set will usually be empty.
        //
        std::multiset<Transaction*> _waiters;
    };

    /**
     *  LockManager is used to control access to resources. Usually a singleton. For deadlock detection
     *  all resources used by a set of transactions that could deadlock, should use one LockManager.
     *
     *  Primary functions are:
     *     acquire    - acquire a resource for shared or exclusive use; may throw Abort.
     *     release    - release a resource previously acquired for shared/exclusive use
     */
    class LockManager {
    public:

        /**
         * thrown primarily when deadlocks are detected, or when LockManager::abort is called.
         * also thrown when LockManager requests are made during shutdown.
         */
        class AbortException : public std::exception {
        public:
            const char* what() const throw ();
        };

        /**
         * Used to decide which blocked requests to honor first when resource becomes available
         */
        enum Policy {
            kPolicyFirstCome,     // wake the first blocked request in arrival order
            kPolicyReadersFirst,  // wake the first blocked read request(s)
            kPolicyOldestTxFirst, // wake the blocked request with the lowest TxId
            kPolicyBlockersFirst, // wake the blocked request which is itself the most blocking
            kPolicyReadersOnly,   // block write requests (used during fsync)
            kPolicyWritersOnly    // block read requests
        };

        /**
         * returned by ::conflictExists, called from acquire
         */
        enum ResourceStatus {
            kResourceAcquired,       // requested resource was already acquired, increment count
            kResourceAvailable,      // requested resource is available. no waiting
            kResourceConflict,       // requested resource is in use by another transaction
            kResourcePolicyConflict, // requested mode blocked by READER/kPolicyWritersOnly policy
            kResourceUpgradeConflict // requested resource was previously acquired for shared use
                                     // now requested for exclusive use, but there are other shared
                                     // users, so this request must wait.
        };

        /**
         * returned by find_lock(), and release() and, mostly for testing
         * explains why a lock wasn't found or released
         */
        enum LockStatus {
            kLockFound,             // found a matching lock request
            kLockReleased,          // released requested lock
            kLockCountDecremented,  // decremented lock count, but didn't release
            kLockNotFound,          // specific lock not found
            kLockResourceNotFound,  // no locks on the resource
            kLockModeNotFound       // locks on the resource, but not of the specified mode
        };


        /**
         * A Notifier is used to do something just before an acquisition request blocks.
         *
         * XXX: should perhaps define Notifier as a functor so C++11 lambda's match
         * for the test.cpp, it was convenient for the call operator to access private
         * state, which is why we're using the class formulation for now
         */
        class Notifier {
        public:
            virtual ~Notifier() { }
            virtual void operator()(const Transaction* blocker) = 0;
        };

        /**
         * Tracks locking statistics.
         */
        class LockStats {
        public:
        LockStats()
            : _numRequests(0)
            , _numPreexistingRequests(0)
            , _numSameRequests(0)
            , _numBlocks(0)
            , _numDeadlocks(0)
            , _numDowngrades(0)
            , _numUpgrades(0)
            , _numMillisBlocked(0) { }

            void incRequests() { _numRequests++; }
            void incPreexisting() { _numPreexistingRequests++; }
            void incSame() { _numSameRequests++; }
            void incBlocks() { _numBlocks++; }
            void incDeadlocks() { _numDeadlocks++; }
            void incDowngrades() { _numDowngrades++; }
            void incUpgrades() { _numUpgrades++; }
            void incTimeBlocked(size_t numMillis ) { _numMillisBlocked += numMillis; }

            size_t getNumRequests() const { return _numRequests; }
            size_t getNumPreexistingRequests() const { return _numPreexistingRequests; }
            size_t getNumSameRequests() const { return _numSameRequests; }
            size_t getNumBlocks() const { return _numBlocks; }
            size_t getNumDeadlocks() const { return _numDeadlocks; }
            size_t getNumDowngrades() const { return _numDowngrades; }
            size_t getNumUpgrades() const { return _numUpgrades; }
            size_t getNumMillisBlocked() const { return _numMillisBlocked; }

            LockStats& operator+=(const LockStats& other);
            std::string toString() const;

        private:
            // total number of resource requests. >= number or resources requested.
            size_t _numRequests;

            // the number of times a resource was requested when there
            // was a pre-existing request (possibly compatible)
            size_t _numPreexistingRequests;

            // the number of times a transaction requested the same resource in the
            // same mode while already holding a lock on that resource
            size_t _numSameRequests;

            // the number of times a resource request blocked.  This is usually 
            // because of a conflicting pre-existing request, but could be because
            // of a policy like READERS_ONLY
            size_t _numBlocks;

            size_t _numDeadlocks;
            size_t _numDowngrades;
            size_t _numUpgrades;

            // aggregates time requests spent blocked.
            size_t _numMillisBlocked;
        };


    public:

        /**
         * Singleton factory - retrieves a common instance of LockManager
         */
        static LockManager& getSingleton();

        /**
         * It's possibly useful to allow multiple LockManagers for non-overlapping sets
         * of resources, so the constructor is left public.  Eventually we may want
         * to enforce a singleton pattern.
         */
        explicit LockManager(const Policy& policy=kPolicyFirstCome);
        ~LockManager();

        /**
         * Change the current Policy.  For READERS/kPolicyWritersOnly, this
         * call may block until all current writers/readers have released their locks.
         */
        void setPolicy(Transaction* tx, const Policy& policy, Notifier* notifier = NULL);

        /**
         * Get the current policy
         */
        Policy getPolicy() const;

        /**
         * Who set the current policy.  Of use when the Policy is ReadersOnly
         * and we want to find out who is blocking a writer.
         */
        Transaction* getPolicySetter() const;

        /**
         * Initiate a shutdown, specifying a period of time to quiesce.
         *
         * During this period, existing transactions can continue to acquire resources,
         * but new transaction requests will throw AbortException.
         *
         * After quiescing, any new requests will throw AbortException
         */
        void shutdown(const unsigned& millisToQuiesce = 1000);


        /**
         * acquire a resource in a mode.
         * can throw AbortException
         */
        void acquire(Transaction* requestor,
                     const LockMode& mode,
                     const ResourceId& resId,
                     Notifier* notifier = NULL);

        /**
         * acquire a designated lock.  usually called from RAII objects
         * that have allocated their lock on the stack.  May throw AbortException.
         */
        void acquireLock(LockRequest* request, Notifier* notifier = NULL);

        /**
         * for bulk operations:
         * acquire one of a vector of ResourceIds in a mode,
         * hopefully without blocking, return index of
         * acquired ResourceId, or -1 if vector was empty
         */
        int acquireOne(Transaction* requestor,
                       const LockMode& mode,
                       const std::vector<ResourceId>& records,
                       Notifier* notifier = NULL);

        /**
         * release a ResourceId.
         * The mode here is just the mode that applies to the resId
         */
        LockStatus release(const Transaction* holder,
                           const LockMode& mode,
                           const ResourceId& resId);

        /**
         * releases the lock returned by acquire.  should perhaps replace above?
         */
        LockStatus releaseLock(LockRequest* request);

        /**
         * release all resources acquired by a transaction
         * returns number of locks released
         */
        size_t release(const Transaction* holder);

        /**
         * called internally for deadlock
         * possibly called publicly to stop a long transaction
         * also used for testing
         */
        MONGO_COMPILER_NORETURN void abort(Transaction* goner);

        /**
         * returns a copy of the stats that exist at the time of the call
         */
        LockStats getStats() const;

        /**
         * slices the space of ResourceIds and TransactionIds into
         * multiple partitions that can be separately guarded to
         * spread the cost of mutex locking.
         */
        static unsigned partitionResource(const ResourceId& resId);
        static unsigned partitionTransaction(unsigned txId);



        // --- for testing and logging

        std::string toString() const;

        /**
         * test whether a Transaction has locked a ResourceId in a mode.
         * most callers should use acquireOne instead
         */
        bool isLocked(const Transaction* holder,
                      const LockMode& mode,
                      const ResourceId& resId) const;

    private: // alphabetical

        /**
         * called by public ::abort and internally upon deadlock
         * releases all locks acquired by goner, notify's any
         * transactions that were waiting, then throws AbortException
         */
        MONGO_COMPILER_NORETURN void _abortInternal(Transaction* goner);

        /**
         * main workhorse for acquiring locks on resources, blocking
         * or aborting on conflict
         *
         * throws AbortException on deadlock
         */
        void _acquireInternal(LockRequest* lr,
                              LockRequest* queue,
                              LockRequest* conflictPosition,
                              ResourceStatus resourceStatus,
                              Notifier* notifier,
                              boost::unique_lock<boost::mutex>& guard);

        /**
         * adds a conflicting lock request to the list of requests for a resource
         * using the Policy.  Called by acquireInternal
         */
        void _addLockToQueueUsingPolicy(LockRequest* lr,
                                        LockRequest* queue,
                                        LockRequest*& position);

        /**
         * when inserting a new lock request into the middle of a queue,
         * add any remaining incompatible requests in the queue to the
         * new lock request's set of waiters... for future deadlock detection
         */
        void _addWaiters(LockRequest* blocker,
                         LockRequest* nextLock,
                         LockRequest* lastLock);

        /**
         * returns true if a newRequest should be honored before an oldRequest according
         * to the lockManager's policy.  Used by acquire to decide whether a new share request
         * conflicts with a previous upgrade-to-exclusive request that is blocked.
         */
        bool _comesBeforeUsingPolicy(const Transaction* newReqTx,
                                     const LockMode& newReqMode,
                                     const LockRequest* oldReq) const;

        /**
         * determine whether a resource request would conflict with an existing lock
         * set the position to the first possible insertion point, which is usually
         * the position of the first conflict, or the end of the queue, or to an existing lock
         */
        ResourceStatus _conflictExists(Transaction* requestor,
                                       const LockMode& mode,
                                       const ResourceId& resId,
                                       unsigned slice,
                                       LockRequest* queue,
                                       LockRequest*& position /* in/out */);

        /**
         * looks for an existing LockRequest that matches the four input params
         * if not found, sets outLid to zero and returns a reason, otherwise
         * sets outLock to the LockRequest that matches and returns kLockFound
         */
        LockStatus _findLock(const Transaction* requestor,
                             const LockMode& mode,
                             const ResourceId& resId,
                             unsigned slice,
                             LockRequest*& outLock) const;

	/**
	 * @return status of requested resource id
	 * set conflictPosition on output if conflict
	 * update several status variables
	 */
	ResourceStatus _getConflictInfo(Transaction* requestor,
					const LockMode& mode,
					const ResourceId& resId,
					unsigned slice,
					LockRequest* queue,
					LockRequest*& conflictPosition);

        /**
         * returns true if acquire would return without waiting
         * used by acquireOne
         */
        bool _isAvailable(const Transaction* requestor,
                          const LockMode& mode,
                          const ResourceId& resId,
                          unsigned slice) const;

        /**
         * maintain the resourceLocks queue
         */
        void _push_back(LockRequest* lr);
        void _removeFromResourceQueue(LockRequest* lr);


        /**
         * called by public ::release and internally by abort.
         * assumes caller as acquired a mutex.
         */
        LockStatus _releaseInternal(LockRequest* lr);

        /**
         * called at start of public APIs, throws exception
         * if quiescing period has expired, or if xid is new
         */
        void _throwIfShuttingDown(const Transaction* tx=NULL) const;


    private:
        // support functions for changing policy to/from read/write only

        void _incStatsForMode(const LockMode& mode) {
            kShared==mode ? _numCurrentActiveReadRequests++ : _numCurrentActiveWriteRequests++;
        }
        void _decStatsForMode(const LockMode& mode) {
            kShared==mode ? _numCurrentActiveReadRequests-- : _numCurrentActiveWriteRequests--;
        }

        unsigned _numActiveReads() const { return _numCurrentActiveReadRequests; }
        unsigned _numActiveWrites() const { return _numCurrentActiveWriteRequests; }


    private:

        // The Policy controls which requests should be honored first.  This is
        // used to guide the position of a request in a list of requests waiting for
        // a resource.
        //
        // XXX At some point, we may want this to also guide the decision of which
        // transaction to abort in case of deadlock.  For now, the transaction whose
        // request would lead to a deadlock is aborted.  Since deadlocks are rare,
        // careful choices may not matter much.
        //
        Policy _policy;

        // transaction which last set policy, for reporting cause of conflicts. not owned
        Transaction* _policySetter;

        // synchronizes access to the lock manager, which is shared across threads
        static const unsigned kNumResourcePartitions = 16;
        mutable boost::mutex _resourceMutexes[kNumResourcePartitions];

#ifdef REGISTER_TRANSACTIONS
        static const unsigned kNumTransactionPartitions = 16;
        mutable boost::mutex _transactionMutexes[kNumTransactionPartitions];
#endif
        mutable boost::mutex _mutex;

        // for blocking when setting kPolicyReadersOnly or kPolicyWritersOnly policy
        boost::condition_variable _policyLock;

        // only meaningful when _policy == SHUTDOWN
        bool _shuttingDown;
        int _millisToQuiesce;
        Timer _timer;

        // Lists of lock requests associated with a resource,
        //
        // The lock-request lists have two sections.  Some number (at least one) of requests
        // at the front of a list are "active".  All remaining lock requests are blocked by
        // some earlier (not necessarily active) lock request, and are waiting.  The order
        // of lock request in the waiting section is determined by the LockPolicty.
        // The order of lock request in the active/front portion of the list is irrelevant.
        //
        std::map<ResourceId, LockRequest*> _resourceLocks[kNumResourcePartitions];

#ifdef REGISTER_TRANSACTIONS
        std::set<Transaction*> _activeTransactions[kNumTransactionPartitions];
#endif

        // used to track conflicts due to kPolicyReadersOnly or WritersOnly
        Transaction* _systemTransaction;

        // stats
        LockStats _stats[kNumResourcePartitions];

        // used when changing policy to/from Readers/Writers Only
        AtomicUInt _numCurrentActiveReadRequests;
        AtomicUInt _numCurrentActiveWriteRequests;
    };

    /**
     * RAII wrapper around LockManager, for scoped locking
     */
    class ResourceLock {
    public:
        ResourceLock(LockManager& lm,
                     Transaction* requestor,
                     const LockMode& mode,
                     const ResourceId& resId,
                     LockManager::Notifier* notifier = NULL);

        ~ResourceLock();
    private:
        LockManager& _lm;
        LockRequest _lr;
    };

    class SharedResourceLock : public ResourceLock {
    public:
    SharedResourceLock(Transaction* requestor, void* resource)
        : ResourceLock(LockManager::getSingleton(),
                       requestor,
                       kShared,
                       (size_t)resource) { }
    SharedResourceLock(Transaction* requestor, size_t resource)
        : ResourceLock(LockManager::getSingleton(),
                       requestor,
                       kShared,
                       resource) { }
    };

    class ExclusiveResourceLock : public ResourceLock {
    public:
    ExclusiveResourceLock(Transaction* requestor, void* resource)
        : ResourceLock(LockManager::getSingleton(),
                       requestor,
                       kExclusive,
                       (size_t)resource) { }
    ExclusiveResourceLock(Transaction* requestor, size_t resource)
        : ResourceLock(LockManager::getSingleton(),
                       requestor,
                       kExclusive,
                       resource) { }
    };
} // namespace mongo
