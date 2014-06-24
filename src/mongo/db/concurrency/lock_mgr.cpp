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

#include "mongo/db/concurrency/lock_mgr.h"

#include <boost/thread/locks.hpp>
#include <sstream>

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include "mongo/base/init.h"

using boost::unique_lock;

using std::endl;
using std::exception;
using std::map;
using std::multiset;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace mongo {

    /*---------- Utility functions ----------*/

    namespace {

        bool isExclusive(const LockMode& mode) {
            return kExclusive == mode;
        }

        bool isShared(const LockMode& mode) {
            return kShared == mode;
        }

        bool isCompatible(const LockMode& mode1, const LockMode& mode2) {
            return mode1==mode2 && isShared(mode1);
        }

        bool hasConflict(const LockManager::ResourceStatus& status) {
            return LockManager::kResourceConflict == status ||
                LockManager::kResourceUpgradeConflict == status ||
                LockManager::kResourcePolicyConflict == status;
        }
    } // namespace

    /*---------- AbortException functions ----------*/

    const char* LockManager::AbortException::what() const throw() { return "AbortException"; }

    /*---------- LockStats functions ----------*/
    string LockManager::LockStats::toString() const {
        stringstream result;
        result << "----- LockManager Stats -----" << endl
               << "\ttotal requests: " << getNumRequests() << endl
               << "\t# pre-existing: " << getNumPreexistingRequests() << endl
               << "\t# same: " << getNumSameRequests() << endl
               << "\t# times blocked: " << getNumBlocks() << endl
               << "\t# ms blocked: " << getNumMillisBlocked() << endl
               << "\t# deadlocks: " << getNumDeadlocks() << endl
               << "\t# downgrades: " << getNumDowngrades() << endl
               << "\t# upgrades: " << getNumUpgrades() << endl
            ;
        return result.str();
    }

    LockManager::LockStats& LockManager::LockStats::operator+=(const LockStats& other) {
        _numRequests += other._numRequests;
        _numPreexistingRequests += other._numPreexistingRequests;
        _numSameRequests += other._numSameRequests;
        _numBlocks += other._numBlocks;
        _numDeadlocks += other._numDeadlocks;
        _numBlocks += other._numBlocks;
        _numDowngrades += other._numDowngrades;
        _numUpgrades += other._numUpgrades;
        _numMillisBlocked += other._numMillisBlocked;
        return *this;
    }

    /*---------- Transaction functions ----------*/
    Transaction::Transaction(unsigned txId, int priority)
        : _txId(txId)
#ifdef REGISTER_TRANSACTION
        , _txSlice(LockManager::partitionTransaction(txId))
#endif
        , _priority(priority)
        , _state((0==txId) ? kInvalid : kActive)
        , _locks(NULL) { }

    Transaction::~Transaction() {
        
    }

    Transaction* Transaction::setTxIdOnce(unsigned txId) {
        if (0 == _txId) {
            _txId = txId;
            _state = kActive;
        }

        return this;
    }

    bool Transaction::operator<(const Transaction& other) {
        return _txId < other._txId;
    }

    int Transaction::getPriority() const { return _priority; }
    void Transaction::setPriority(int newPriority) { _priority = newPriority; }

    void Transaction::removeLock(LockRequest* lr) {
        if (lr->nextOfTransaction) {
            lr->nextOfTransaction->prevOfTransaction = lr->prevOfTransaction;
        }
        if (lr->prevOfTransaction) {
            lr->prevOfTransaction->nextOfTransaction = lr->nextOfTransaction;
        }
        else {
            _locks = lr->nextOfTransaction;
        }
        lr->nextOfTransaction = NULL;
        lr->prevOfTransaction = NULL;
        if (lr->heapAllocated) delete lr;
    }

    void Transaction::addLock(LockRequest* lr) {
        lr->nextOfTransaction = _locks;

        if (_locks) {
            _locks->prevOfTransaction = lr;
        }
        _locks = lr;
    }

    void Transaction::_addWaiter(Transaction* waiter) {
        _waiters.insert(waiter);
        _waiters.insert(waiter->_waiters.begin(), waiter->_waiters.end());
    }

    string Transaction::toString() const {
        stringstream result;
        result << "<xid:" << _txId
#ifdef REGISTER_TRANSACTIONS
               << ",slice:" << _txSlice
#endif
               << ",priority:" << _priority
               << ",state:" << ((kActive == _state) ? "active" : "completed");

        result << ",locks: {";
        bool firstLock=true;
        for (LockRequest* nextLock = _locks; nextLock; nextLock=nextLock->nextOfTransaction) {
            if (firstLock) firstLock=false;
            else result << ",";
            result << nextLock->toString();
        }
        result << "}";

        result << ">,waiters: {";
        bool firstWaiter=true;
        for (multiset<Transaction*>::const_iterator nextWaiter = _waiters.begin();
             nextWaiter != _waiters.end(); ++nextWaiter) {
            if (firstWaiter) firstWaiter=false;
            else result << ",";
            result << (*nextWaiter)->_txId;
        }
        result << "}>";
        return result.str();
    }


    /*---------- LockRequest functions ----------*/


    LockRequest::LockRequest(const ResourceId& resId,
                             const LockMode& mode,
                             Transaction* tx,
                             bool heapAllocated)
        : requestor(tx)
        , mode(mode)
        , resId(resId)
        , slice(LockManager::partitionResource(resId))
        , count(1)
        , sleepCount(0)
        , heapAllocated(heapAllocated)
        , nextOnResource(NULL)
        , prevOnResource(NULL)
        , nextOfTransaction(NULL)
        , prevOfTransaction(NULL) { }


    LockRequest::~LockRequest() {
        verify(NULL == nextOfTransaction);
        verify(NULL == prevOfTransaction);
        verify(NULL == nextOnResource);
        verify(NULL == prevOnResource);
    }

    bool LockRequest::matches(const Transaction* tx,
                              const LockMode& mode,
                              const ResourceId& resId) const {
        return
            this->requestor == tx &&
            this->mode == mode &&
            this->resId == resId;
    }

    string LockRequest::toString() const {
        stringstream result;
        result << "<xid:" << requestor->_txId
               << ",mode:" << mode
               << ",resId:" << resId
               << ",count:" << count
               << ",sleepCount:" << sleepCount
               << ">";
        return result.str();
    }

    bool LockRequest::isBlocked() const {
        return sleepCount > 0;
    }

    bool LockRequest::shouldAwake() {
        return 0 == --sleepCount;
    }

    void LockRequest::insert(LockRequest* lr) {
        lr->prevOnResource = this->prevOnResource;
        lr->nextOnResource = this;

        if (this->prevOnResource) {
            this->prevOnResource->nextOnResource = lr;
        }
        this->prevOnResource = lr;
    }

    void LockRequest::append(LockRequest* lr) {
        lr->prevOnResource = this;
        lr->nextOnResource = this->nextOnResource;

        if (this->nextOnResource) {
            this->nextOnResource->prevOnResource = lr;
        }
        this->nextOnResource = lr;
    }

    /*---------- LockManager public functions (mutex guarded) ---------*/

    static LockManager* _singleton = NULL;

    MONGO_INITIALIZER(InstantiateLockManager)(InitializerContext* context) {
        _singleton = new LockManager();
        return Status::OK();
    }

    LockManager& LockManager::getSingleton() {
        return *_singleton;
    }

    LockManager::LockManager(const Policy& policy)
        : _policy(policy)
        , _mutex()
        , _shuttingDown(false)
        , _millisToQuiesce(-1)
        , _systemTransaction(new Transaction(0))
        , _numCurrentActiveReadRequests(0)
        , _numCurrentActiveWriteRequests(0)
    { }

    LockManager::~LockManager() {
        delete _systemTransaction;
    }

    void LockManager::shutdown(const unsigned& millisToQuiesce) {
        unique_lock<boost::mutex> lk(_mutex);

#ifdef DONT_ALLOW_CHANGE_TO_QUIESCE_PERIOD
        // XXX not sure whether we want to allow multiple shutdowns
        // in order to change quiesce period?
        if (_shuttingDown) {
            return; // already in shutdown, don't extend quiescence(?)
        }
#endif

        _shuttingDown = true;
        _millisToQuiesce = millisToQuiesce;
        _timer.millisReset();
    }

    LockManager::Policy LockManager::getPolicy() const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown();
        return _policy;
    }

    Transaction* LockManager::getPolicySetter() const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown();
        return _policySetter;
    }

    void LockManager::setPolicy(Transaction* tx, const Policy& policy, Notifier* notifier) {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown();
        
        if (policy == _policy) return;

        _policySetter = tx;
        Policy oldPolicy = _policy;
        _policy = policy;

        // if moving away from {READERS,WRITERS}_ONLY, awaken requests that were pending
        //
        if (kPolicyReadersOnly == oldPolicy || kPolicyWritersOnly == oldPolicy) {

            // Awaken requests that were blocked on the old policy.
            // iterate over TxIds blocked on kReservedTxId (these are blocked on policy)

            for (multiset<Transaction*>::iterator nextWaiter = _systemTransaction->_waiters.begin();
                 nextWaiter != _systemTransaction->_waiters.end(); ++nextWaiter) {

                // iterate over the locks acquired by the blocked transactions
                for (LockRequest* nextLock = (*nextWaiter)->_locks; nextLock;
                     nextLock = nextLock->nextOfTransaction) {
                    if (nextLock->isBlocked() && nextLock->shouldAwake()) {

                        // each transaction can only be blocked by one request at time
                        // this one must be due to policy that's now changed
                        nextLock->lock.notify_one();
                    }
                }
            }
            _systemTransaction->_waiters.clear();
        }

        // if moving to {READERS,WRITERS}_ONLY, block until no incompatible locks
        if (kPolicyReadersOnly == policy || kPolicyWritersOnly == policy) {
            unsigned (LockManager::*numBlockers)() const = (kPolicyReadersOnly == policy)
                ? &LockManager::_numActiveWrites
                : &LockManager::_numActiveReads;

            if ((this->*numBlockers)() > 0) {
                if (notifier) {
                    (*notifier)(_systemTransaction);
                }
                do {
                    _policyLock.wait(lk);
                } while ((this->*numBlockers)() > 0);
            }
        }
    }

    void LockManager::acquireLock(LockRequest* lr, Notifier* notifier) {
        if (NULL == lr) return;
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown();
        }

        // don't accept requests from aborted transactions
        if (Transaction::kAborted == lr->requestor->_state) {
            throw AbortException();
        }
        unique_lock<boost::mutex> lk(_resourceMutexes[lr->slice]);

        LockRequest* queue = _resourceLocks[lr->slice][lr->resId];
        LockRequest* conflictPosition = queue;
        ResourceStatus status = _getConflictInfo(lr->requestor, lr->mode, lr->resId, lr->slice,
                                                 queue, conflictPosition);
        if (kResourceAcquired == status) { return; }

        // add lock request to requesting transaction's list
        lr->requestor->addLock(lr);

        _acquireInternal(lr, queue, conflictPosition, status, notifier, lk);
        _incStatsForMode(lr->mode);
    }

    void LockManager::acquire(Transaction* requestor,
                              const LockMode& mode,
                              const ResourceId& resId,
                              Notifier* notifier) {
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown();
        }

        // don't accept requests from aborted transactions
        if (Transaction::kAborted == requestor->_state) {
            throw AbortException();
        }
        unsigned slice = partitionResource(resId);
        unique_lock<boost::mutex> lk(_resourceMutexes[slice]);

        LockRequest* queue = _resourceLocks[slice][resId];
        LockRequest* conflictPosition = queue;
        ResourceStatus status = _getConflictInfo(requestor, mode, resId, slice,
                                                 queue, conflictPosition);
        if (kResourceAcquired == status) { return; }

        LockRequest* lr = new LockRequest(resId, mode, requestor, true);

        // add lock request to requesting transaction's list
        lr->requestor->addLock(lr);

        _acquireInternal(lr, queue, conflictPosition, status, notifier, lk);
        _incStatsForMode(mode);
    }

    int LockManager::acquireOne(Transaction* requestor,
                                const LockMode& mode,
                                const vector<ResourceId>& resources,
                                Notifier* notifier) {
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown(requestor);
        }

        // don't accept requests from aborted transactions
        if (Transaction::kAborted == requestor->_state) {
            throw AbortException();
        }

        if (resources.empty()) { return -1; }

        // acquire the first available recordId
        for (unsigned ix=0; ix < resources.size(); ix++) {
            ResourceId resId = resources[ix];
            unsigned slice = partitionResource(resId);
            bool isAvailable = false;
            {
                unique_lock<boost::mutex> lk(_resourceMutexes[slice]);
                isAvailable = _isAvailable(requestor, mode, resId, slice);
            }
            if (isAvailable) {
                acquire(requestor, mode, resId, notifier);
                return ix;
            }
        }

        // sigh. none of the records are currently available. wait on the first.
        acquire(requestor, mode, resources[0], notifier);
        return 0;
    }

    LockManager::LockStatus LockManager::releaseLock(LockRequest* lr) {
        if (NULL == lr) return kLockNotFound;
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown(lr->requestor);
        }
        unique_lock<boost::mutex> lk(_resourceMutexes[lr->slice]);
        _decStatsForMode(lr->mode);
        return _releaseInternal(lr);
    }

    LockManager::LockStatus LockManager::release(const Transaction* holder,
                                                 const LockMode& mode,
                                                 const ResourceId& resId) {
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown(holder);
        }
        unsigned slice = partitionResource(resId);
        unique_lock<boost::mutex> lk(_resourceMutexes[slice]);

        LockRequest* lr;
        LockStatus status = _findLock(holder, mode, resId, slice, lr);
        if (kLockFound != status) {
            return status; // error, resource wasn't acquired in this mode by holder
        }
        _decStatsForMode(mode);
        return _releaseInternal(lr);
    }

#if 0
    /*
     * release all resource acquired by a transaction, returning the count
     */
    size_t LockManager::release(Transaction* holder) {
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown(holder);
        }

        TxLockMap::iterator lockIdsHeld = _xaLocks.find(holder);
        if (lockIdsHeld == _xaLocks.end()) { return 0; }
        size_t numLocksReleased = 0;
        for (set<LockId>::iterator nextLockId = lockIdsHeld->second.begin();
             nextLockId != lockIdsHeld->second.end(); ++nextLockId) {
            _releaseInternal(*nextLockId);

            _decStatsForMode(_locks[*nextLockId]->mode);

            if ((kPolicyWritersOnly == _policy && 0 == _stats.numActiveReads()) ||
                (kPolicyReadersOnly == _policy && 0 == _stats.numActiveWrites())) {
                _policyLock.notify_one();
            }
            numLocksReleased++;
        }
        return numLocksReleased;
    }
#endif
    void LockManager::abort(Transaction* goner) {
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown(goner);
        }
        _abortInternal(goner);
    }

    LockManager::LockStats LockManager::getStats() const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown();

        LockStats result;
        for (unsigned ix=0; ix < kNumResourcePartitions; ix++) {
            result += _stats[ix];
        }
        return result;
    }

    string LockManager::toString() const {
//     unique_lock<boost::mutex> lk(_mutex);
#ifdef DONT_CARE_ABOUT_DEBUG_EVEN_WHEN_SHUTTING_DOWN
        // seems like we might want to allow toString for debug during shutdown?
        _throwIfShuttingDown();
#endif
        stringstream result;
        result << "Policy: ";
        switch(_policy) {
        case kPolicyFirstCome:
            result << "FirstCome";
            break;
        case kPolicyReadersFirst:
            result << "ReadersFirst";
            break;
        case kPolicyOldestTxFirst:
            result << "OldestFirst";
            break;
        case kPolicyBlockersFirst:
            result << "BiggestBlockerFirst";
            break;
        case kPolicyReadersOnly:
            result << "ReadersOnly";
            break;
        case kPolicyWritersOnly:
            result << "WritersOnly";
            break;
        }
        result << endl;

        if (_shuttingDown)
            result << " shutting down in " << _millisToQuiesce - _timer.millis();

        result << "\t_resourceLocks:" << endl;
        bool firstResource=true;
        result << "resources=" << ": {";
        for (unsigned slice=0; slice < kNumResourcePartitions; ++slice) {
            for (map<ResourceId, LockRequest*>::const_iterator nextResource = _resourceLocks[slice].begin();
                 nextResource != _resourceLocks[slice].end(); ++nextResource) {
                if (firstResource) firstResource=false;
                else result << ", ";
                result << nextResource->first << ": {";
                bool firstLock=true;
                for (LockRequest* nextLock = nextResource->second;
                     nextLock; nextLock=nextLock->nextOnResource) {
                    if (firstLock) firstLock=false;
                    else result << ", ";
                    result << nextLock->toString();
                }
                result << "}";
            }
        }
        result << "}" << endl;
#ifdef REGISTER_TRANSACTIONS
        result << "\tTransactions:" << endl;
        bool firstTx=true;
        for (unsigned jx=0; jx < kNumTransactionPartitions; ++jx) {
            for (set<Transaction*>::const_iterator nextTx = _activeTransactions[jx].begin();
                 nextTx != _activeTransactions[jx].end(); ++nextTx) {
                if (firstTx) firstTx=false;
                else result << ", ";
                result << "\t\t" << (*nextTx)->toString();
            }
        }
#endif
        return result.str();
    }

    bool LockManager::isLocked(const Transaction* holder,
                               const LockMode& mode,
                               const ResourceId& resId) const {
        {
            unique_lock<boost::mutex> lk(_mutex);
            _throwIfShuttingDown(holder);
        }

        LockRequest* unused=NULL;
        return kLockFound == _findLock(holder, mode, resId, partitionResource(resId), unused);
    }

    unsigned LockManager::partitionResource(const ResourceId& resId) {
        // when resIds are DiskLocs, their low-order bits are mostly zero
        // so add up nibbles as cheap hash
        size_t resIdValue = resId;
        size_t resIdHash = 0;
        size_t mask = 0xf;
        for (unsigned ix=0; ix < 2*sizeof(size_t); ++ix) {
            resIdHash += (resIdValue >> ix*4) & mask;
        }
        return resIdHash % kNumResourcePartitions;
    }
#ifdef REGISTER_TRANSACTIONS
    unsigned LockManager::partitionTransaction(unsigned xid) {
        return xid % kNumTransactionPartitions;
    }
#endif

    void LockManager::_push_back(LockRequest* lr) {
        LockRequest* nextLock = _resourceLocks[lr->slice][lr->resId];
        if (NULL == nextLock) {
            _resourceLocks[lr->slice][lr->resId] = lr;
            return;
        }

        while (nextLock->nextOnResource) {
            nextLock = nextLock->nextOnResource;
        }

        nextLock->append(lr);
    }

    void LockManager::_removeFromResourceQueue(LockRequest* lr) {
        if (lr->nextOnResource) {
            lr->nextOnResource->prevOnResource = lr->prevOnResource;
        }
        if (lr->prevOnResource) {
            lr->prevOnResource->nextOnResource = lr->nextOnResource;
        }
        else if (NULL == lr->nextOnResource) {
            _resourceLocks[lr->slice].erase(lr->resId);
        }
        else {
            _resourceLocks[lr->slice][lr->resId] = lr->nextOnResource;
        }
        lr->nextOnResource = NULL;
        lr->prevOnResource = NULL;
    }

    /*---------- LockManager private functions (alphabetical) ----------*/

    /*
     * release resources acquired by a transaction about to abort, notifying
     * any waiters that they can retry their resource acquisition.  cleanup
     * and throw an AbortException.
     */
    void LockManager::_abortInternal(Transaction* goner) {

        goner->_state = Transaction::kAborted;

        if (NULL == goner->_locks) {
            // unusual, but possible to abort a transaction with no locks
            throw AbortException();
        }

        // release all resources acquired by this transaction
        // notifying any waiters that they can continue
        //
        LockRequest* nextLock = goner->_locks;
        while (nextLock) {
            // releaseInternal deletes nextLock, so get the next ptr here
            LockRequest* newNextLock = nextLock->nextOfTransaction;
            _releaseInternal(nextLock);
            nextLock = newNextLock;
        }

        // erase aborted transaction's waiters
        goner->_waiters.clear();

        throw AbortException();
    }

    LockManager::ResourceStatus LockManager::_getConflictInfo(Transaction* requestor,
                                                              const LockMode& mode,
                                                              const ResourceId& resId,
                                                              unsigned slice,
                                                              LockRequest* queue,
                                                              LockRequest*& conflictPosition) {
        _stats[slice].incRequests();

        if (queue) { _stats[slice].incPreexisting(); }

        ResourceStatus resourceStatus = _conflictExists(requestor, mode, resId,
                                                        slice, queue, conflictPosition);
        if (kResourceAcquired == resourceStatus) {
            _stats[slice].incSame();
            ++conflictPosition->count;
        }
        return resourceStatus;
    }

    void LockManager::_acquireInternal(LockRequest* lr,
                                       LockRequest* queue,
                                       LockRequest* conflictPosition,
                                       ResourceStatus resourceStatus,
                                       Notifier* sleepNotifier,
                                       unique_lock<boost::mutex>& guard) {

        if (kResourceAvailable == resourceStatus) {
            if (!conflictPosition)
                _push_back(lr);
            else if (conflictPosition == queue) {
                lr->nextOnResource = _resourceLocks[lr->slice][lr->resId];
                _resourceLocks[lr->slice][lr->resId] = lr;
            }
            else {
                conflictPosition->prevOnResource->nextOnResource = lr;
                lr->nextOnResource = conflictPosition;
                lr->prevOnResource = conflictPosition->prevOnResource;
            }

            _addWaiters(lr, conflictPosition, NULL);
            return;
        }

        // some type of conflict, insert after confictPosition

        verify(conflictPosition ||
               kResourcePolicyConflict == resourceStatus ||
               kResourceUpgradeConflict == resourceStatus);

        if (conflictPosition) {
            conflictPosition = conflictPosition->nextOnResource;
        }

        if (kResourceUpgradeConflict == resourceStatus) {
            if (conflictPosition)
                conflictPosition->insert(lr);
            else
                _push_back(lr);
        }
        else {
            _addLockToQueueUsingPolicy(lr, queue, conflictPosition);
        }

#ifdef VERIFY_LOCK_MANAGER
        if (isExclusive(mode)) {
            for (LockRequest* nextFollower = conflictPosition;
                 nextFollower; nextFollower=nextFollower->nextOnResource) {
                if (nextFollower->requestor == requestor) continue;
                verify(nextFollower->isBlocked());
            }
        }
#endif
        // set remaining incompatible requests as lr's waiters
        _addWaiters(lr, conflictPosition, NULL);


        // call the sleep notification function once
        if (NULL != sleepNotifier) {
            // XXX should arg be xid of blocker?
            (*sleepNotifier)(lr->requestor);
        }

        _stats[lr->slice].incBlocks();

        // this loop typically executes once
        do {
            // set up for future deadlock detection add requestor to blockers' waiters
            //
            for (LockRequest* nextBlocker = queue; nextBlocker != conflictPosition; 
                 nextBlocker=nextBlocker->nextOnResource) {
                if (nextBlocker == lr) {break;}
                if (nextBlocker->requestor == lr->requestor) {continue;}
                if (isCompatible(nextBlocker->mode, lr->mode)) {continue;}
                nextBlocker->requestor->_addWaiter(lr->requestor);
                ++lr->sleepCount;
            }
            if (kResourcePolicyConflict == resourceStatus) {
                // to facilitate waking once the policy reverts, add requestor to system's waiters
                _systemTransaction->_addWaiter(lr->requestor);
                ++lr->sleepCount;
            }

            // wait for blocker to release
            while (lr->isBlocked()) {
                Timer timer;
                lr->lock.wait(guard);
                _stats[lr->slice].incTimeBlocked(timer.millis());
            }

            queue = conflictPosition = _resourceLocks[lr->slice][lr->resId];
            resourceStatus = _conflictExists(lr->requestor, lr->mode, lr->resId, lr->slice,
                                             queue, conflictPosition);
        } while (hasConflict(resourceStatus));
    }

    /*
     * called only when there are conflicting LockRequests
     * positions a lock request (lr) in a queue at or after position
     * also adds remaining requests in queue as lr's waiters
     * for subsequent deadlock detection
     */
    void LockManager::_addLockToQueueUsingPolicy(LockRequest* lr,
                                                 LockRequest* queue,
                                                 LockRequest*& position) {

        if (position == NULL) {
            _push_back(lr);
            return;
        }

        // use lock request's transaction's priority if specified
        int txPriority = lr->requestor->getPriority();
        if (txPriority > 0) {
            for (; position; position=position->nextOnResource) {
                if (txPriority > position->requestor->getPriority()) {
                    // add in front of request with lower priority that is either
                    // compatible, or blocked
                    //
                    position->insert(lr);
                    return;
                }
            }
            _push_back(lr);
            return;
        }
        else if (txPriority < 0) {
            // for now, just push to end
            // TODO: honor position of low priority requests
            _push_back(lr);
        }

        // use LockManager's default policy
        switch (_policy) {
        case kPolicyFirstCome:
            _push_back(lr);
            position = NULL;
            return;
        case kPolicyReadersFirst:
            if (isExclusive(lr->mode)) {
                _push_back(lr);
                position = NULL;
                return;
            }
            for (; position; position=position->nextOnResource) {
                if (isExclusive(position->mode) && position->isBlocked()) {
                    // insert shared lock before first sleeping exclusive lock
                    position->insert(lr);
                    return;
                }
            }
            break;
        case kPolicyOldestTxFirst:
            for (; position; position=position->nextOnResource) {
                if (lr->requestor < position->requestor &&
                    (isCompatible(lr->mode, position->mode) || position->isBlocked())) {
                    // smaller xid is older, so queue it before
                    position->insert(lr);
                    return;
                }
            }
            break;
        case kPolicyBlockersFirst: {
            size_t lrNumWaiters = lr->requestor->_waiters.size();
            for (; position; position=position->nextOnResource) {
                size_t nextRequestNumWaiters = position->requestor->_waiters.size();
                if (lrNumWaiters > nextRequestNumWaiters &&
                    (isCompatible(lr->mode, position->mode) || position->isBlocked())) {
                    position->insert(lr);
                    return;
                }
            }
            break;
        }
        default:
            break;
        }

        _push_back(lr);
        position = NULL;
    }

    void LockManager::_addWaiters(LockRequest* blocker,
                                  LockRequest* nextLock,
                                  LockRequest* lastLock) {
        for (; nextLock != lastLock; nextLock=nextLock->nextOnResource) {
            if (! isCompatible(blocker->mode, nextLock->mode)) {
                if (nextLock->sleepCount > 0) {
                    blocker->requestor->_addWaiter(nextLock->requestor);
                    ++nextLock->sleepCount;
                }
            }
        }
    }

    bool LockManager::_comesBeforeUsingPolicy(const Transaction* requestor,
                                              const LockMode& mode,
                                              const LockRequest* oldRequest) const {

        // handle special policies
        if (kPolicyReadersOnly == _policy && kShared == mode && oldRequest->isBlocked())
            return true;
        if (kPolicyWritersOnly == _policy && kExclusive == mode && oldRequest->isBlocked())
            return true;

        if (requestor->getPriority() >
            oldRequest->requestor->getPriority()) {
            return true;
        }

        switch (_policy) {
        case kPolicyFirstCome:
            return false;
        case kPolicyReadersFirst:
            return isShared(mode);
        case kPolicyOldestTxFirst:
            return requestor < oldRequest->requestor;
        case kPolicyBlockersFirst: {
            return requestor->_waiters.size() > oldRequest->requestor->_waiters.size();
        }
        default:
            return false;
        }
    }

    LockManager::ResourceStatus LockManager::_conflictExists(Transaction* requestor,
                                                             const LockMode& mode,
                                                             const ResourceId& resId,
                                                             unsigned slice,
                                                             LockRequest* queue,
                                                             LockRequest*& nextLock) {

        // handle READERS/kPolicyWritersOnly policy conflicts
        if ((kPolicyReadersOnly == _policy && isExclusive(mode)) ||
            (kPolicyWritersOnly == _policy && isShared(mode))) {

            if (NULL == nextLock) { return kResourcePolicyConflict; }

            // position past the last active lock request on the queue
            LockRequest* lastActivePosition = NULL;
            for (; nextLock; nextLock = nextLock->nextOnResource) {
                if (requestor == nextLock->requestor && mode == nextLock->mode) {
                    return kResourceAcquired; // already have the lock
                }
                if (! nextLock->isBlocked()) {
                    lastActivePosition = nextLock;
                }
            }
            if (lastActivePosition) {
                nextLock = lastActivePosition;
            }
            return kResourcePolicyConflict;
        }

        // loop over the lock requests in the queue, looking for the 1st conflict
        // normally, we'll leave the nextLock positioned at the 1st conflict
        // if there is one, or the position (often the end) where we know there is no conflict.
        //
        // upgrades complicate this picture, because we want to position the iterator
        // after all initial share locks.  but we may not know whether an exclusived request
        // is an upgrade until we look at all the initial share locks.
        //
        // so we record the position of the 1st conflict, but continue advancing the
        // nextLock until we've seen all initial share locks.  If none have
        // the same Transaction as the exclusive request, we restore the position to 1st conflict
        //
        LockRequest* firstConflict = NULL;
        set<Transaction*> sharedOwners; // all initial share lock owners
        bool alreadyHadLock = false;  // true if we see a lock with the same Txid

        for (; nextLock; nextLock=nextLock->nextOnResource) {

            if (nextLock->matches(requestor, mode, resId)) {
                // if we're already on the queue, there's no conflict
                return kResourceAcquired;
            }

            if (requestor == nextLock->requestor) {
                // an upgrade or downgrade request, can't conflict with ourselves
                if (isShared(mode)) {
                    // downgrade
                    _stats[slice].incDowngrades();
                    nextLock = nextLock->nextOnResource;
                    return kResourceAvailable;
                }

                // upgrade
                alreadyHadLock = true;
                _stats[slice].incUpgrades();
                // position after initial readers
                continue;
            }

            if (isShared(nextLock->mode)) {
                invariant(!nextLock->isBlocked() || kPolicyWritersOnly == _policy);

                sharedOwners.insert(nextLock->requestor);

                if (isExclusive(mode) && firstConflict == NULL) {
                    // if "lr" proves not to be an upgrade, restore this position later
                    firstConflict = nextLock;
                }
                // either there's no conflict yet, or we're not done checking for an upgrade
                continue;
            }

            // the next lock on the queue is an exclusive request
            invariant(isExclusive(nextLock->mode));

            if (alreadyHadLock) {
                // bumped into something incompatible while up/down grading
                if (isExclusive(mode)) {
                    // upgrading: bumped into another exclusive lock
                    if (sharedOwners.find(nextLock->requestor) != sharedOwners.end()) {
                        // the exclusive lock is also an upgrade, and it must
                        // be blocked, waiting for our original share lock to be released
                        // if we wait for its shared lock, we would deadlock
                        //
                        invariant(nextLock->isBlocked());
                        _abortInternal(requestor);
                    }

                    if (sharedOwners.empty()) {
                        // simple upgrade, queue in front of nextLockRequest, no conflict
                        return kResourceAvailable;
                    }
                    else {
                        // we have to wait for another shared lock before upgrading
                        return kResourceUpgradeConflict;
                    }
                }

                // downgrading, bumped into an exclusive lock, blocked on our original
                invariant (isShared(mode));
                invariant(nextLock->isBlocked());
                // lr will be inserted before nextLockRequest
                return kResourceAvailable;
            }
            else if (firstConflict) {
                // restore first conflict position
                nextLock = firstConflict;
            }

            // no conflict if nextLock is blocked and we come before
            if (nextLock->isBlocked() &&
                _comesBeforeUsingPolicy(requestor, mode, nextLock)) {
                return kResourceAvailable;
            }

            // there's a conflict, check for deadlock
            if (requestor->_waiters.find(nextLock->requestor) != requestor->_waiters.end()) {
                // the transaction that would block requestor is already blocked by requestor
                // if requestor waited for nextLockRequest, there would be a deadlock
                //
                _stats[slice].incDeadlocks();
                _abortInternal(requestor);
            }
            return kResourceConflict;
        }

        // positioned to the end of the queue
        if (alreadyHadLock && isExclusive(mode) && !sharedOwners.empty()) {
            // upgrading, queue consists of requestor's earlier share lock
            // plus other share lock.  Must wait for the others to release
            return kResourceUpgradeConflict;
        }
        else if (firstConflict) {
            nextLock = firstConflict;

            if (_comesBeforeUsingPolicy(requestor, mode, nextLock)) {
                return kResourceAvailable;
            }

            // there's a conflict, check for deadlock
            if (requestor->_waiters.find(nextLock->requestor) != requestor->_waiters.end()) {
                // the transaction that would block requestor is already blocked by requestor
                // if requestor waited for nextLockRequest, there would be a deadlock
                //
                _stats[slice].incDeadlocks();
                _abortInternal(requestor);
            }
            return kResourceConflict;
        }
        return kResourceAvailable;
    }

    LockManager::LockStatus LockManager::_findLock(const Transaction* holder,
                                                   const LockMode& mode,
                                                   const ResourceId& resId,
                                                   unsigned slice,
                                                   LockRequest*& outLock) const {

        outLock = NULL; // set invalid;

        // get iterator for resId's locks
        map<ResourceId,LockRequest*>::const_iterator resLocks = _resourceLocks[slice].find(resId);
        if (resLocks == _resourceLocks[slice].end()) { return kLockResourceNotFound; }

        // look for an existing lock request from holder in mode
        for (LockRequest* nextLock = resLocks->second;
             nextLock; nextLock=nextLock->nextOnResource) {
            if (nextLock->requestor == holder && nextLock->mode == mode) {
                outLock = nextLock;
                return kLockFound;
            }
        }
        return kLockModeNotFound;
    }

    /*
     * Used by acquireOne
     * XXX: there's overlap between this, _conflictExists and _findLock
     */
    bool LockManager::_isAvailable(const Transaction* requestor,
                                   const LockMode& mode,
                                   const ResourceId& resId,
                                   unsigned slice) const {

        // check for exceptional policies
        if (kPolicyReadersOnly == _policy && isExclusive(mode))
            return false;
        else if (kPolicyWritersOnly == _policy && isShared(mode))
            return false;

        
        // walk over the queue of previous requests for this ResourceId
        for (const LockRequest* nextLock = _resourceLocks[slice].at(resId);
             nextLock; nextLock = nextLock->nextOnResource) {

            if (nextLock->matches(requestor, mode, resId)) {
                // we're already have this lock, if we're asking, we can't be asleep
                invariant(! nextLock->isBlocked());
                return true;
            }

            // no conflict if we're compatible
            if (isCompatible(mode, nextLock->mode)) continue;

            // no conflict if nextLock is blocked and we come before
            if (nextLock->isBlocked() && _comesBeforeUsingPolicy(requestor, mode, nextLock))
                return true;

            return false; // we're incompatible and would block
        }

        // everything on the queue (if anything is on the queue) is compatible
        return true;
    }

    LockManager::LockStatus LockManager::_releaseInternal(LockRequest* lr) {
        Transaction* holder = lr->requestor;
        const LockMode& mode = lr->mode;

        if ((kPolicyWritersOnly == _policy && 0 == _numActiveReads()) ||
            (kPolicyReadersOnly == _policy && 0 == _numActiveWrites())) {
            _policyLock.notify_one();
        }

        LockRequest* queue = _resourceLocks[lr->slice][lr->resId];
        if (NULL == queue) {
            return kLockResourceNotFound;
        }

        bool foundLock = false;
        bool foundResource = false;

        LockRequest* nextLock = queue;

        // find the position of the lock to release in the queue
        for(; !foundLock && nextLock; nextLock=nextLock->nextOnResource) {
            if (lr != nextLock) {
                if (nextLock->requestor == holder) {
                    foundResource = true;
                }
            }
            else {
                // this is our lock.
                if (--nextLock->count > 0) { return kLockCountDecremented; }

                foundLock = true;
                break; // don't increment nextLock again
            }
        }

        if (! foundLock) {
            // can't release a lock that hasn't been acquired in the specified mode
            return foundResource ? kLockModeNotFound : kLockResourceNotFound;
        }

        if (isShared(mode)) {
            // skip over any remaining shared requests. they can't be waiting for us.
            for (; nextLock; nextLock=nextLock->nextOnResource) {
                if (isExclusive(nextLock->mode)) {
                    break;
                }
            }
        }

        // everything left on the queue potentially conflicts with the lock just
        // released, unless it's an up/down-grade of that lock.  So iterate, and
        // when Transactions differ, decrement sleepCount, wake those with zero counts, and
        // decrement their sleep counts, waking sleepers with zero counts, and
        // cleanup state used for deadlock detection

        for (; nextLock; nextLock=nextLock->nextOnResource) {
            LockRequest* nextSleeper = nextLock;
            if (nextSleeper->requestor == holder) continue;

            invariant(nextSleeper->isBlocked());

            // remove nextSleeper and its dependents from holder's waiters

            if (holder->_waiters.find(nextSleeper->requestor) != holder->_waiters.end()) {
                // every sleeper should be among holders waiters, but a previous sleeper might have
                // had the nextSleeper as a dependent as well, in which case nextSleeper was removed
                // previously, hence the test for finding nextSleeper among holder's waiters
                //
                Transaction* sleepersTx = nextSleeper->requestor;
                holder->_waiters.erase(holder->_waiters.find(sleepersTx));
                multiset<Transaction*>::iterator nextSleepersWaiter = sleepersTx->_waiters.begin();
                for(; nextSleepersWaiter != sleepersTx->_waiters.end(); ++nextSleepersWaiter) {
                    holder->_waiters.erase(*nextSleepersWaiter);
                }
            }

            // wake up sleepy heads
            if (nextSleeper->shouldAwake()) {
                nextSleeper->lock.notify_one();
            }
        }

#ifdef VERIFY_LOCK_MANAGER
        if (holder->_waiters.empty()) {
            for (set<Transaction*>::iterator nextTx = _activeTransactions.begin();
                 nextTx != _activeTransactions.end(); ++nextTx) {
                verify( (*nextTx)->_waiters.find(holder) == (*nextTx)->_waiters.end());
            }
        }

        if (queue) {
            verify(!queue->isBlocked());
        }
#endif

        // release the lock
        _removeFromResourceQueue(lr);
        holder->removeLock(lr);

        return kLockReleased;
    }

    void LockManager::_throwIfShuttingDown(const Transaction* tx) const {

        if (_shuttingDown && (_timer.millis() >= _millisToQuiesce))

#ifdef LOCK_MANAGER_TRANSACTION_REGISTRATION
            ||
            _activeTransactions[tx->txSlice].find(tx) == _activeTransactions[tx->txSlice].end()))
#endif
        {

            throw AbortException(); // XXX should this be something else? ShutdownException?
        }
    }

/*---------- ResourceLock functions ----------*/

    ResourceLock::ResourceLock(LockManager& lm,
                               Transaction* requestor,
                               const LockMode& mode,
                               const ResourceId& resId,
                               LockManager::Notifier* notifier)
        : _lm(lm)
        , _lr(resId, mode, requestor)
    {
        _lm.acquireLock(&_lr, notifier);
    }

    ResourceLock::~ResourceLock() {
        _lm.releaseLock(&_lr);
    }

} // namespace mongo
