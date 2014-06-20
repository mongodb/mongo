/**
*    Copyright (C) MongoDB Inc.
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

using boost::unique_lock;

using std::endl;
using std::exception;
using std::list;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace mongo {

    /*---------- Utility functions ----------*/

    namespace {

	/*
	 * Given a 'hierarchical' resource (e.g. document in collection in database)
	 * return true if the resource is reqeuested for exclusive access at a given level.
	 * In the example above, level 0 for document, level 1 for collection, level 2 for DB
	 * and level 3 for the system as a whole.
	 *
	 * mode is a bit vector, level is the index.
	 */
        bool isExclusive(unsigned mode, const unsigned level=0) {
            return 0 != (mode & (0x1 << level));
        }

        bool isShared(unsigned mode, const unsigned level=0) {
            return 0 == (mode & (0x1 << level));
        }

        bool isCompatible(unsigned mode1, unsigned mode2) {
            return mode1==mode2 && (isShared(mode1) || isShared(mode1));
        }

        bool hasConflict(const LockManager::ResourceStatus& status) {
            return LockManager::kResourceConflict == status ||
                LockManager::kResourceUpgradeConflict == status ||
                LockManager::kResourcePolicyConflict == status;
        }

        // unique id for each LockRequest. zero is reserved
        static LockManager::LockId nextLid = 1;
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

    /*---------- LockRequest functions ----------*/


    LockManager::LockRequest::LockRequest(const TxId& xid,
					  const unsigned& mode,
					  const ResourceId& resId)
        : lid(nextLid++)
        , xid(xid)
        , mode(mode)
        , resId(resId)
        , count(1)
        , sleepCount(0) { }


    LockManager::LockRequest::~LockRequest() { }

    bool LockManager::LockRequest::matches(const TxId& xid,
                                           const unsigned& mode,
                                           const ResourceId& resId) const {
        return
            this->xid == xid &&
            this->mode == mode &&
            this->resId == resId;
    }

    string LockManager::LockRequest::toString() const {
        stringstream result;
        result << "<lid:" << lid
               << ",xid:" << xid
               << ",mode:" << mode
               << ",resId:" << resId
               << ",count:" << count
               << ",sleepCount:" << sleepCount
               << ">";
        return result.str();
    }

    bool LockManager::LockRequest::isBlocked() const {
        return sleepCount > 0;
    }

    bool LockManager::LockRequest::shouldAwake() {
        return 0 == --sleepCount;
    }

    /*---------- LockManager public functions (mutex guarded) ---------*/

    LockManager* LockManager::_singleton = NULL;
    boost::mutex LockManager::_getSingletonMutex;
    LockManager& LockManager::getSingleton() {
        unique_lock<boost::mutex> lk(_getSingletonMutex);
        if (NULL == _singleton) {
            _singleton = new LockManager();
        }
        return *_singleton;
    }

    LockManager::LockManager(const Policy& policy)
        : _policy(policy),
          _mutex(),
          _shuttingDown(false),
          _millisToQuiesce(-1) { }

    LockManager::~LockManager() {
        unique_lock<boost::mutex> lk(_mutex);
        for (map<LockId, LockRequest*>::iterator locks = _locks.begin();
             locks != _locks.end(); ++locks) {
            delete locks->second;
        }
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

    TxId LockManager::getPolicySetter() const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown();
	return _policySetter;
    }

    void LockManager::setTransactionPriority(const TxId& xid, int priority) {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(xid);
        _txPriorities[xid] = priority;
    }

    int LockManager::getTransactionPriority(const TxId& xid) const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(xid);
        return _getTransactionPriorityInternal(xid);
    }

    LockManager::LockId LockManager::acquire(const TxId& requestor,
                                             const uint32_t& mode,
                                             const ResourceId& resId,
                                             Notifier* notifier) {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(requestor);

        // don't accept requests from aborted transactions
        if (_abortedTxIds.find(requestor) != _abortedTxIds.end()) {
            throw AbortException();
        }

        _stats.incRequests();
        _stats.incStatsForMode(mode);

        return _acquireInternal(requestor, mode, resId, notifier, lk);
    }

    int LockManager::acquireOne(const TxId& requestor,
                                const uint32_t& mode,
                                const vector<ResourceId>& resources,
                                Notifier* notifier) {

        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(requestor);

        if (resources.empty()) { return -1; }

        // don't accept requests from aborted transactions
        if (_abortedTxIds.find(requestor) != _abortedTxIds.end()) {
            throw AbortException();
        }

        _stats.incRequests();

        // acquire the first available recordId
        for (unsigned ix=0; ix < resources.size(); ix++) {
            if (_isAvailable(requestor, mode, resources[ix])) {
                _acquireInternal(requestor, mode, resources[ix], notifier, lk);
                _stats.incStatsForMode(mode);
                return ix;
            }
        }

        // sigh. none of the records are currently available. wait on the first.
        _stats.incStatsForMode(mode);
        _acquireInternal(requestor, mode, resources[0], notifier, lk);
        return 0;
    }

    LockManager::LockStatus LockManager::releaseLock(const LockId& lid) {
        unique_lock<boost::mutex> lk(_mutex);

        LockMap::iterator it = _locks.find(lid);
        if (it != _locks.end()) {
            LockRequest* theLock = it->second;
            _throwIfShuttingDown(theLock->xid);
            _stats.decStatsForMode(theLock->mode);
            if ((kPolicyWritersOnly == _policy && 0 == _stats.numActiveReads()) ||
                (kPolicyReadersOnly == _policy && 0 == _stats.numActiveWrites())) {
                _policyLock.notify_one();
            }
        }
        return _releaseInternal(lid);
    }

    LockManager::LockStatus LockManager::release(const TxId& holder,
                                                 const uint32_t& mode,
                                                 const ResourceId& resId) {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(holder);

        LockId lid;
        LockStatus status = _findLock(holder, mode, resId, &lid);
        if (kLockFound != status) {
            return status; // error, resource wasn't acquired in this mode by holder
        }
        _stats.decStatsForMode(_locks[lid]->mode);
        if ((kPolicyWritersOnly == _policy && 0 == _stats.numActiveReads()) ||
            (kPolicyReadersOnly == _policy && 0 == _stats.numActiveWrites())) {
            _policyLock.notify_one();
        }
        return _releaseInternal(lid);
    }

    /*
     * release all resource acquired by a transaction, returning the count
     */
    size_t LockManager::release(const TxId& holder) {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(holder);

        TxLockMap::iterator lockIdsHeld = _xaLocks.find(holder);
        if (lockIdsHeld == _xaLocks.end()) { return 0; }
        size_t numLocksReleased = 0;
        for (set<LockId>::iterator nextLockId = lockIdsHeld->second.begin();
             nextLockId != lockIdsHeld->second.end(); ++nextLockId) {
            _releaseInternal(*nextLockId);

            _stats.decStatsForMode(_locks[*nextLockId]->mode);

            if ((kPolicyWritersOnly == _policy && 0 == _stats.numActiveReads()) ||
                (kPolicyReadersOnly == _policy && 0 == _stats.numActiveWrites())) {
                _policyLock.notify_one();
            }
            numLocksReleased++;
        }
        return numLocksReleased;
    }

    void LockManager::abort(const TxId& goner) {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(goner);
        _abortInternal(goner);
    }

    LockManager::LockStats LockManager::getStats() const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown();
        return _stats;
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

        result << "\t_locks:" << endl;
        for (map<LockId,LockRequest*>::const_iterator locks = _locks.begin();
             locks != _locks.end(); ++locks) {
            result << "\t\t" << locks->first << locks->second->toString() << endl;
        }

        result << "\t_resourceLocks:" << endl;
		bool firstResource=true;
		result << "resources=" << ": {";
		for (map<ResourceId,list<LockId> >::const_iterator nextResource = _resourceLocks.begin();
			 nextResource != _resourceLocks.end(); ++nextResource) {
			if (firstResource) firstResource=false;
			else result << ", ";
			result << nextResource->first << ": {";
			bool firstLock=true;
			for (list<LockId>::const_iterator nextLockId = nextResource->second.begin();
				 nextLockId != nextResource->second.end(); ++nextLockId) {
				if (firstLock) firstLock=false;
				else result << ", ";
				result << *nextLockId;
			}
			result << "}";
		}
		result << "}" << endl;

        result << "\t_waiters:" << endl;
        for (map<TxId, set<TxId> >::const_iterator txWaiters = _waiters.begin();
             txWaiters != _waiters.end(); ++txWaiters) {
            bool firstTime=true;
            result << "\t\t" << txWaiters->first << ": {";
            for (set<TxId>::const_iterator nextWaiter = txWaiters->second.begin();
                 nextWaiter != txWaiters->second.end(); ++nextWaiter) {
                if (firstTime) firstTime=false;
                else result << ", ";
                result << *nextWaiter;
            }
            result << "}" << endl;
        }

        bool firstGoner = true;
        result << "\t_aborted: {" << endl;
        for (set<TxId>::iterator goners = _abortedTxIds.begin();
             goners != _abortedTxIds.end(); ++goners) {
            if (firstGoner) firstGoner = false;
            else result << ",";
            result << "t" << *goners;
        }
        result << "}";

        return result.str();
    }

    bool LockManager::isLocked(const TxId& holder,
                               const uint32_t& mode,
                               const ResourceId& resId) const {
        unique_lock<boost::mutex> lk(_mutex);
        _throwIfShuttingDown(holder);

        LockId unused;
        return kLockFound == _findLock(holder, mode, resId, &unused);
    }

    /*---------- LockManager private functions (alphabetical) ----------*/

    /*
     * release resources acquired by a transaction about to abort, notifying
     * any waiters that they can retry their resource acquisition.  cleanup
     * and throw an AbortException.
     */
    void LockManager::_abortInternal(const TxId& goner) {
        TxLockMap::iterator locks = _xaLocks.find(goner);

        if (locks == _xaLocks.end()) {
            // unusual, but possible to abort a transaction with no locks
            throw AbortException();
        }

        // make a copy of the TxId's locks, because releasing
        // would otherwise affect the iterator. XXX find a better way?
        //
        set<LockId> copyOfLocks = locks->second;

        // release all resources acquired by this transaction
        // notifying any waiters that they can continue
        //
        for (set<LockId>::iterator nextLockId = copyOfLocks.begin();
             nextLockId != copyOfLocks.end(); ++nextLockId) {
            _releaseInternal(*nextLockId);
        }

        // add to set of aborted transactions
        _abortedTxIds.insert(goner);

        throw AbortException();
    }

    LockManager::LockId LockManager::_acquireInternal(const TxId& requestor,
                                                      const uint32_t& mode,
                                                      const ResourceId& resId,
                                                      Notifier* sleepNotifier,
                                                      unique_lock<boost::mutex>& guard) {
#if 0
        static unsigned long funCount = 0;
        if (0 == ++funCount % 10000) {
            log() << _stats.toString();
        }
#endif
        list<LockId>& queue = _resourceLocks[resId];
		if (!queue.empty()) { _stats.incPreexisting(); }
        list<LockId>::iterator conflictPosition = queue.begin();
        ResourceStatus resourceStatus = _conflictExists(requestor, mode, resId,
                                                        queue, conflictPosition);
        if (kResourceAcquired == resourceStatus) {
			_stats.incSame();
            ++_locks[*conflictPosition]->count;
            return *conflictPosition;
        }

        // create the lock request and add to TxId's set of lock requests

        // XXX should probably use placement operator new and manage LockRequest memory
        LockRequest* lr = new LockRequest(requestor, mode, resId);
        _locks[lr->lid] = lr;

        // add lock request to set of requests of requesting TxId
		_xaLocks[requestor].insert(lr->lid);

        if (kResourceAvailable == resourceStatus) {
            queue.insert(conflictPosition, lr->lid);
            _addWaiters(lr, conflictPosition, queue.end());
            return lr->lid;
        }

        // some type of conflict, insert after confictPosition

        verify(conflictPosition != queue.end());
        ++conflictPosition;

        if (kResourceUpgradeConflict == resourceStatus) {
            queue.insert(conflictPosition, lr->lid);
        }
        else {
            _addLockToQueueUsingPolicy(lr, queue, conflictPosition);
        }
#if 0
        if (isExclusive(mode)) {
            for (list<LockId>::iterator followers = conflictPosition;
                 followers != queue.end(); ++followers) {
                LockRequest* nextLockRequest = _locks[*followers];
                if (nextLockRequest->xid == requestor) continue;
                verify(nextLockRequest->isBlocked());
            }
        }
#endif
        // set remaining incompatible requests as lr's waiters
        _addWaiters(lr, conflictPosition, queue.end());


        // call the sleep notification function once
        if (NULL != sleepNotifier) {
            // XXX should arg be xid of blocker?
            (*sleepNotifier)(lr->xid);
        }

        _stats.incBlocks();

		// this loop typically executes once
        do {
            // set up for future deadlock detection add requestor to blockers' waiters
            //
            for (list<LockId>::iterator nextBlocker = queue.begin();
                 nextBlocker != conflictPosition; ++nextBlocker) {
                LockRequest* nextBlockingRequest = _locks[*nextBlocker];
                if (nextBlockingRequest->lid == lr->lid) {break;}
                if (nextBlockingRequest->xid == requestor) {continue;}
                if (isCompatible(_locks[*nextBlocker]->mode, lr->mode)) {continue;}
                _addWaiter(_locks[*nextBlocker]->xid, requestor);
                ++lr->sleepCount;
            }
            if (kResourcePolicyConflict == resourceStatus) {
                // to facilitate waking once the policy reverts, add requestor to system's waiters
                _addWaiter(kReservedTxId, requestor);
                ++lr->sleepCount;
            }

            // wait for blocker to release
            while (lr->isBlocked()) {
                Timer timer;
                lr->lock.wait(guard);
                _stats.incTimeBlocked(timer.millis());
            }

            conflictPosition = queue.begin();
            resourceStatus = _conflictExists(lr->xid, lr->mode, lr->resId, queue, conflictPosition);
        } while (hasConflict(resourceStatus));

        return lr->lid;
    }

    /*
     * called only when there are conflicting LockRequests
     * positions a lock request (lr) in a queue at or after position
     * also adds remaining requests in queue as lr's waiters
     * for subsequent deadlock detection
     */
    void LockManager::_addLockToQueueUsingPolicy(LockRequest* lr,
						 list<LockId>& queue,
						 list<LockId>::iterator& position) {

        if (position == queue.end()) {
            queue.insert(position, lr->lid);
            return;
        }

        // use lock request's transaction's priority if specified
        int txPriority = _getTransactionPriorityInternal(lr->xid);
        if (txPriority > 0) {
            for (; position != queue.end(); ++position) {
                LockRequest* nextRequest = _locks[*position];
                if (txPriority > _getTransactionPriorityInternal(nextRequest->xid)) {
                    // add in front of request with lower priority that is either
                    // compatible, or blocked
					//
                    queue.insert(position, lr->lid);
                    return;
                }
            }
            queue.push_back(lr->lid);
            return;
        }
        else if (txPriority < 0) {
            // for now, just push to end
            // TODO: honor position of low priority requests
            queue.push_back(lr->lid);
        }

        // use LockManager's default policy
        switch (_policy) {
        case kPolicyFirstCome:
            queue.push_back(lr->lid);
	    position = queue.end();
            return;
        case kPolicyReadersFirst:
            if (isExclusive(lr->mode)) {
                queue.push_back(lr->lid);
                position = queue.end();
                return;
            }
            for (; position != queue.end(); ++position) {
                LockRequest* nextRequest = _locks[*position];
                if (isExclusive(nextRequest->mode) && nextRequest->isBlocked()) {
                    // insert shared lock before first sleeping exclusive lock
                    queue.insert(position, lr->lid);
                    return;
                }
            }
            break;
        case kPolicyOldestTxFirst:
            for (; position != queue.end(); ++position) {
                LockRequest* nextRequest = _locks[*position];
                if (lr->xid < nextRequest->xid &&
                    (isCompatible(lr->mode, nextRequest->mode) || nextRequest->isBlocked())) {
                    // smaller xid is older, so queue it before
                    queue.insert(position, lr->lid);
                    return;
                }
            }
            break;
        default:
            break;
        }

        queue.push_back(lr->lid);
	position = queue.end();
    }

    void LockManager::_addWaiter(const TxId& blocker, const TxId& requestor) {
        if (blocker == requestor) {
            // can't wait on self
            return;
        }

		// add requestor to blocker's waiters
		_waiters[blocker].insert(requestor);

		// add all of requestor's waiters to blocker's waiters
		_waiters[blocker].insert(_waiters[requestor].begin(), _waiters[requestor].end());
    }

    void LockManager::_addWaiters(LockRequest* blocker,
				  list<LockId>::iterator nextLockId,
				  list<LockId>::iterator lastLockId) {
        for (; nextLockId != lastLockId; ++nextLockId) {
            LockRequest* nextLockRequest = _locks[*nextLockId];
            if (! isCompatible(blocker->mode, nextLockRequest->mode)) {
                if (nextLockRequest->sleepCount > 0) {
		    _addWaiter(blocker->xid, nextLockRequest->xid);
		    ++nextLockRequest->sleepCount;
		}
            }
        }
    }

    bool LockManager::_comesBeforeUsingPolicy(const TxId& requestor,
					      const unsigned& mode,
					      const LockRequest* oldRequest) const {

        // handle special policies
        if (kPolicyReadersOnly == _policy && kShared == mode && oldRequest->isBlocked())
            return true;
        if (kPolicyWritersOnly == _policy && kExclusive == mode && oldRequest->isBlocked())
            return true;

        if (_getTransactionPriorityInternal(requestor) >
            _getTransactionPriorityInternal(oldRequest->xid)) {
            return true;
        }

        switch (_policy) {
        case kPolicyFirstCome:
            return false;
        case kPolicyReadersFirst:
            return isShared(mode);
        case kPolicyOldestTxFirst:
            return requestor < oldRequest->xid;
        default:
            return false;
        }
    }

    LockManager::ResourceStatus LockManager::_conflictExists(const TxId& requestor,
                                                             const unsigned& mode,
                                                             const ResourceId& resId,
                                                             list<LockId>& queue,
                                                             list<LockId>::iterator& nextLockId) {

        // handle READERS/kPolicyWritersOnly policy conflicts
        if ((kPolicyReadersOnly == _policy && isExclusive(mode)) ||
            (kPolicyWritersOnly == _policy && isShared(mode))) {

            if (nextLockId == queue.end()) { return kResourcePolicyConflict; }

            // position past the last active lock request on the queue
            list<LockId>::iterator lastActivePosition = queue.end();
            for (; nextLockId != queue.end(); ++nextLockId) {
                LockRequest* nextLockRequest = _locks[*nextLockId];
                if (requestor == nextLockRequest->xid && mode == nextLockRequest->mode) {
                    return kResourceAcquired; // already have the lock
                }
                if (! nextLockRequest->isBlocked()) {
                    lastActivePosition = nextLockId;
                }
            }
            if (lastActivePosition != queue.end()) {
                nextLockId = lastActivePosition;
            }
            return kResourcePolicyConflict;
        }

        // loop over the lock requests in the queue, looking for the 1st conflict
        // normally, we'll leave the nextLockId iterator positioned at the 1st conflict
        // if there is one, or the position (often the end) where we know there is no conflict.
        //
        // upgrades complicate this picture, because we want to position the iterator
        // after all initial share locks.  but we may not know whether an exclusived request
        // is an upgrade until we look at all the initial share locks.
        //
        // so we record the position of the 1st conflict, but continue advancing the
        // nextLockId iterator until we've seen all initial share locks.  If none have
        // the same TxId as the exclusive request, we restore the position to 1st conflict
        //
        list<LockId>::iterator firstConflict = queue.end(); // invalid
        set<TxId> sharedOwners; // all initial share lock owners
        bool alreadyHadLock = false;  // true if we see a lock with the same Txid

        for (; nextLockId != queue.end(); ++nextLockId) {

            LockRequest* nextLockRequest = _locks[*nextLockId];

            if (nextLockRequest->matches(requestor, mode, resId)) {
                // if we're already on the queue, there's no conflict
                return kResourceAcquired;
            }

            if (requestor == nextLockRequest->xid) {
                // an upgrade or downgrade request, can't conflict with ourselves
                if (isShared(mode)) {
                    // downgrade
                    _stats.incDowngrades();
                    ++nextLockId;
                    return kResourceAvailable;
                }

                // upgrade
                alreadyHadLock = true;
                _stats.incUpgrades();
                // position after initial readers
                continue;
            }

            if (isShared(nextLockRequest->mode)) {
                invariant(!nextLockRequest->isBlocked() || kPolicyWritersOnly == _policy);

                sharedOwners.insert(nextLockRequest->xid);

                if (isExclusive(mode) && firstConflict == queue.end()) {
                    // if "lr" proves not to be an upgrade, restore this position later
                    firstConflict = nextLockId;
                }
                // either there's no conflict yet, or we're not done checking for an upgrade
                continue;
            }

            // the next lock on the queue is an exclusive request
            invariant(isExclusive(nextLockRequest->mode));

            if (alreadyHadLock) {
                // bumped into something incompatible while up/down grading
                if (isExclusive(mode)) {
                    // upgrading: bumped into another exclusive lock
                    if (sharedOwners.find(nextLockRequest->xid) != sharedOwners.end()) {
                        // the exclusive lock is also an upgrade, and it must
                        // be blocked, waiting for our original share lock to be released
                        // if we wait for its shared lock, we would deadlock
                        //
                        invariant(nextLockRequest->isBlocked());
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
                invariant(nextLockRequest->isBlocked());
                // lr will be inserted before nextLockRequest
                return kResourceAvailable;
            }
            else if (firstConflict != queue.end()) {
                // restore first conflict position
                nextLockId = firstConflict;
                nextLockRequest = _locks[*nextLockId];
            }

            // no conflict if nextLock is blocked and we come before
            if (nextLockRequest->isBlocked() &&
                _comesBeforeUsingPolicy(requestor, mode, nextLockRequest)) {
                return kResourceAvailable;
            }

            return kResourceConflict;
        }

        // positioned to the end of the queue
        if (alreadyHadLock && isExclusive(mode) && !sharedOwners.empty()) {
            // upgrading, queue consists of requestor's earlier share lock
            // plus other share lock.  Must wait for the others to release
            return kResourceUpgradeConflict;
        }
        else if (firstConflict != queue.end()) {
            nextLockId = firstConflict;
            LockRequest* nextLockRequest = _locks[*nextLockId];

            if (_comesBeforeUsingPolicy(requestor, mode, nextLockRequest)) {
                return kResourceAvailable;
            }

            return kResourceConflict;
        }
        return kResourceAvailable;
    }

    LockManager::LockStatus LockManager::_findLock(const TxId& holder,
                                                   const unsigned& mode,
                                                   const ResourceId& resId,
                                                   LockId* outLockId) const {

        *outLockId = kReservedLockId; // set invalid;

        // get iterator for resId's locks
        ResourceLocks::const_iterator resourceLocks = _resourceLocks.find(resId);
        if (resourceLocks == _resourceLocks.end()) { return kLockResourceNotFound; }

        // look for an existing lock request from holder in mode
        for (list<LockId>::const_iterator nextLockId = resourceLocks->second.begin();
             nextLockId != resourceLocks->second.end(); ++nextLockId) {
            LockRequest* nextLockRequest = _locks.at(*nextLockId);
            if (nextLockRequest->xid == holder && nextLockRequest->mode == mode) {
                *outLockId = nextLockRequest->lid;
                return kLockFound;
            }
        }
        return kLockModeNotFound;
    }

    int LockManager::_getTransactionPriorityInternal(const TxId& xid) const {
        map<TxId, int>::const_iterator txPriority = _txPriorities.find(xid);
        if (txPriority == _txPriorities.end()) {
            return 0;
        }
        return txPriority->second;
    }

    /*
     * Used by acquireOne
     * XXX: there's overlap between this, _conflictExists and _findLock
     */
    bool LockManager::_isAvailable(const TxId& requestor,
                                   const unsigned& mode,
                                   const ResourceId& resId) const {

        // check for exceptional policies
        if (kPolicyReadersOnly == _policy && isExclusive(mode))
            return false;
        else if (kPolicyWritersOnly == _policy && isShared(mode))
            return false;

        ResourceLocks::const_iterator resLocks = _resourceLocks.find(resId);
        if (resLocks == _resourceLocks.end()) {
            return true; // no lock requests against this ResourceId, so must be available
        }

        // walk over the queue of previous requests for this ResourceId
        const list<LockId>& queue = resLocks->second;
        for (list<LockId>::const_iterator nextLockId = queue.begin();
             nextLockId != queue.end(); ++nextLockId) {

            LockRequest* nextLockRequest = _locks.at(*nextLockId);

            if (nextLockRequest->matches(requestor, mode, resId)) {
                // we're already have this lock, if we're asking, we can't be asleep
                invariant(! nextLockRequest->isBlocked());
                return true;
            }

            // no conflict if we're compatible
            if (isCompatible(mode, nextLockRequest->mode)) continue;

            // no conflict if nextLock is blocked and we come before
            if (nextLockRequest->isBlocked() && _comesBeforeUsingPolicy(requestor, mode, nextLockRequest))
                return true;

            return false; // we're incompatible and would block
        }

        // everything on the queue (if anything is on the queue) is compatible
        return true;
    }

    LockManager::LockStatus LockManager::_releaseInternal(const LockId& lid) {

        if (kReservedLockId == lid) { return kLockIdNotFound; }

        LockRequest* lr = _locks[lid];
        if (NULL == lr) { return kLockIdNotFound; }

        const TxId holder = lr->xid;
        const unsigned mode = lr->mode;
        const ResourceId resId = lr->resId;

        ResourceLocks::iterator recordLocks = _resourceLocks.find(resId);
        if (recordLocks == _resourceLocks.end()) {
            return kLockResourceNotFound;
        }

        bool foundLock = false;
        bool foundResource = false;

        list<LockId>& queue = recordLocks->second;
        list<LockId>::iterator nextLockId = queue.begin();

        // find the position of the lock to release in the queue
        for(; !foundLock && nextLockId != queue.end(); ++nextLockId) {
            LockRequest* nextLock = _locks[*nextLockId];
            if (lid != *nextLockId) {
                if (nextLock->xid == holder) {
                    foundResource = true;
                }
            }
            else {
                // this is our lock.
                if (0 < --nextLock->count) { return kLockCountDecremented; }

                // release the lock
                _xaLocks[holder].erase(*nextLockId);
                _locks.erase(*nextLockId);
                queue.erase(nextLockId++);
                delete nextLock;

                foundLock = true;
                break; // don't increment nextLockId again
            }
        }

        if (! foundLock) {
            // can't release a lock that hasn't been acquired in the specified mode
            return foundResource ? kLockModeNotFound : kLockResourceNotFound;
        }

        if (isShared(mode)) {
            // skip over any remaining shared requests. they can't be waiting for us.
            for (; nextLockId != queue.end(); ++nextLockId) {
                LockRequest* nextLock = _locks[*nextLockId];
                if (isExclusive(nextLock->mode)) {
                    break;
                }
            }
        }

        // everything left on the queue potentially conflicts with the lock just
        // released, unless it's an up/down-grade of that lock.  So iterate, and
        // when TxIds differ, decrement sleepCount, wake those with zero counts, and
        // decrement their sleep counts, waking sleepers with zero counts, and
        // cleanup state used for deadlock detection

        for (; nextLockId != queue.end(); ++nextLockId) {
            LockRequest* nextSleeper = _locks[*nextLockId];
            if (nextSleeper->xid == holder) continue;

            invariant(nextSleeper->isBlocked());

            // wake up sleepy heads
            if (nextSleeper->shouldAwake()) {
                nextSleeper->lock.notify_one();
            }
        }
#if 0
        // verify stuff
        if (_xaLocks[holder].empty()) {
            for (WaitersMap::iterator dependencies = _waiters.begin();
                 dependencies != _waiters.end(); ++dependencies) {
                verify( dependencies->second.find(holder) == dependencies->second.end());
            }
        }

	if (!queue.empty()) {
	    verify(! _locks[queue.front()]->isBlocked());
	}
#endif
        return kLockReleased;
    }

    void LockManager::_throwIfShuttingDown(const TxId& xid) const {
        if (_shuttingDown && (_timer.millis() >= _millisToQuiesce ||
                              _xaLocks.find(xid) == _xaLocks.end())) {

            throw AbortException(); // XXX should this be something else? ShutdownException?
        }
    }

    /*---------- ResourceLock functions ----------*/

    ResourceLock::ResourceLock(LockManager& lm,
                               const TxId& requestor,
                               const uint32_t& mode,
                               const ResourceId& resId,
                               LockManager::Notifier* notifier)
        : _lm(lm),
          _lid(LockManager::kReservedLockId)  // if acquire throws, we want this initialized
    {
        _lid = lm.acquire(requestor, mode, resId, notifier);
    }

    ResourceLock::~ResourceLock() {
        _lm.releaseLock(_lid);
    }

} // namespace mongo
