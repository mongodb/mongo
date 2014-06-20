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

/**
 * tests for util/concurrency/lock_mgr.* capabilities
 *
 * Testing concurrency requires multiple threads.  In this test, these are packaged as
 * instances of a ClientTransaction class.  The test's main thread creates instances
 * of ClientTransactions and communicates with them through a pair of producer/consumer
 * buffers.  The driver thread sends requests to ClientTransaction threads using a
 * TxCommandBuffer, and waits for responses in a TxResponseBuffer. This protocol
 * allows precise control over timing.
 *
 * The producer/consumer buffer is possibly overkill. At present there is only
 * one producer and one consumer using any one buffer.  Also, the current set of
 * tests are 'lock-step', with the driver never issuing more than one command
 * before waiting for a response.
 */

#include <boost/thread/thread.hpp>
#include "mongo/unittest/unittest.h"
#include "mongo/db/concurrency/lock_mgr.h"
#include "mongo/util/log.h"

namespace mongo {

    enum TxCmd {
        ACQUIRE,
        RELEASE,
        ABORT,
        POLICY,
        QUIT,
        INVALID
    };

    enum TxRsp {
        ACQUIRED,
        RELEASED,
        BLOCKED,
        AWAKENED,
        ABORTED,
        INVALID_RESPONSE
    };

    class TxResponse {
    public:
        TxRsp rspCode;
        TxId xid;
        unsigned mode;
        ResourceId resId;
    };

    class TxRequest {
    public:
        TxCmd cmd;
        TxId xid;
        unsigned mode;
        ResourceId resId;
        LockManager::Policy policy;
    };

    class TxResponseBuffer {
    public:
        TxResponseBuffer() : _count(0), _readPos(0), _writePos(0) { }

        void post(const TxRsp& rspCode) {
            boost::unique_lock<boost::mutex> guard(_guard);
            while (_count == 10)
                _full.wait(guard);
            buffer[_writePos++].rspCode = rspCode;
            _writePos %= 10;
            _count++;
            _empty.notify_one();
        }

        TxResponse* consume() {
            boost::unique_lock<boost::mutex> guard(_guard);
            while (_count == 0)
                _empty.wait(guard);
            TxResponse* result = &buffer[_readPos++];
            _readPos %= 10;
            _count--;
            _full.notify_one();
            return result;
        }

        boost::mutex _guard;
        boost::condition_variable _full;
        boost::condition_variable _empty;

        size_t _count;
        size_t _readPos;
        size_t _writePos;

        TxResponse buffer[10];
    };

class TxCommandBuffer {
public:
    TxCommandBuffer() : _count(0), _readPos(0), _writePos(0) { }

    void post(const TxCmd& cmd,
              const TxId& xid = 0,
              const unsigned& mode = 0,
              const ResourceId& resId = 0,
              const LockManager::Policy& policy = LockManager::kPolicyFirstCome) {
        boost::unique_lock<boost::mutex> guard(_guard);
        while (_count == 10)
            _full.wait(guard);
        buffer[_writePos].cmd = cmd;
        buffer[_writePos].xid = xid;
        buffer[_writePos].mode = mode;
        buffer[_writePos].resId = resId;
        buffer[_writePos].policy = policy;
        _writePos++;
        _writePos %= 10;
        _count++;
        _empty.notify_one();
    }

    TxRequest* consume() {
        boost::unique_lock<boost::mutex> guard(_guard);
        while (_count == 0)
            _empty.wait(guard);
        TxRequest* result = &buffer[_readPos++];
        _readPos %= 10;
        _count--;
        _full.notify_one();
        return result;
    }

    boost::mutex _guard;
    boost::condition_variable _full;
    boost::condition_variable _empty;

    size_t _count;
    size_t _readPos;
    size_t _writePos;

    TxRequest buffer[10];
};

class ClientTransaction : public LockManager::Notifier {
public:
    // these are called in the main driver program
    
    ClientTransaction(LockManager* lm, const TxId& xid) : _lm(lm), _xid(xid), _thr(&ClientTransaction::processCmd, this) { }
    virtual ~ClientTransaction() { _thr.join(); }

    void acquire(const unsigned& mode, const ResourceId resId, const TxRsp& rspCode) {
        _cmd.post(ACQUIRE, _xid, mode, 1, resId);
        TxResponse* rsp = _rsp.consume();
        ASSERT(rspCode == rsp->rspCode);
    }

    void release(const unsigned& mode, const ResourceId resId) {
        _cmd.post(RELEASE, _xid, mode, 1, resId);
        TxResponse* rsp = _rsp.consume();
        ASSERT(RELEASED == rsp->rspCode);
    }

    void abort() {
        _cmd.post(ABORT, _xid);
        TxResponse* rsp = _rsp.consume();
        ASSERT(ABORTED == rsp->rspCode);
    }

    void wakened() {
        TxResponse* rsp = _rsp.consume();
        ASSERT(ACQUIRED == rsp->rspCode);
    }

    void setPolicy(const LockManager::Policy& policy, const TxRsp& rspCode) {
        _cmd.post(POLICY, _xid, 0, 0, 0, policy);
        TxResponse* rsp = _rsp.consume();
        ASSERT(rspCode == rsp->rspCode);
    }

    void quit() {
        _cmd.post(QUIT);
    }
    
    // these are run within the client threads
    void processCmd() {
        bool more = true;
        while (more) {
            TxRequest* req = _cmd.consume();
            switch (req->cmd) {
            case ACQUIRE:
                try {
                    _lm->acquire(_xid, req->mode, req->resId, this);
                    _rsp.post(ACQUIRED);
                } catch (const LockManager::AbortException& err) {
                    _rsp.post(ABORTED);
//          log() << "t" << _xid << ": aborted, ending" << endl;
                    return;
                }
                break;
            case RELEASE:
                _lm->release(_xid, req->mode, req->resId);
                _rsp.post(RELEASED);
                break;
            case ABORT:
                try {
                    _lm->abort(_xid);
                } catch (const LockManager::AbortException& err) {
                    _rsp.post(ABORTED);
                }
                break;
            case POLICY:
                try {
                    _lm->setPolicy(_xid, req->policy, this);
                    _rsp.post(ACQUIRED);
                } catch( const LockManager::AbortException& err) {
                    _rsp.post(ABORTED);
                }
                break;
            case QUIT:
            default:
                more = false;
                break;
            }
        }
    }

    // inherited from Notifier, used by LockManager::acquire
    virtual void operator()(const TxId& blocker) {
        _rsp.post(BLOCKED);
    }

private:
    TxCommandBuffer _cmd;
    TxResponseBuffer _rsp;
    LockManager* _lm;
    TxId _xid;
    boost::thread _thr;
    
};

TEST(LockManagerTest, TxError) {
    LockManager lm;
    LockManager::LockStatus status;

    // release a lock on a container we haven't locked
    lm.release(1, LockManager::kShared, 0, 1);


    // release a lock on a record we haven't locked
    status = lm.release(1, LockManager::kShared, 0, 1);
    ASSERT(LockManager::kLockContainerNotFound == status);


    // release a lock on a record we haven't locked in a store we have locked
    lm.acquire(1, LockManager::kShared, 0, 2);
    status = lm.release(1, LockManager::kShared, 0, 1); // this is in error
    ASSERT(LockManager::kLockResourceNotFound == status);
    status = lm.release(1, LockManager::kShared, 0, 2);
    ASSERT(LockManager::kLockReleased == status);

    // release a record we've locked in a different mode
    lm.acquire(1, LockManager::kShared, 0, 1);
    status = lm.release(1, LockManager::kExclusive, 0, 1); // this is in error
    ASSERT(LockManager::kLockModeNotFound == status);
    status = lm.release(1, LockManager::kShared, 0, 1);
    ASSERT(LockManager::kLockReleased == status);

    lm.acquire(1, LockManager::kExclusive, 0, 1);
    status = lm.release(1, LockManager::kShared, 0, 1); // this is in error
    ASSERT(LockManager::kLockModeNotFound == status);
    status = lm.release(1, LockManager::kExclusive, 0, 1);
    ASSERT(LockManager::kLockReleased == status);

    // attempt to acquire on a transaction that aborted
    try {
        lm.abort(1);
    } catch (const LockManager::AbortException& err) { }
    try {
        lm.acquire(1, LockManager::kShared, 0, 1); // error
        ASSERT(false);
    } catch (const LockManager::AbortException& error) {
    }
}

TEST(LockManagerTest, SingleTx) {
    LockManager lm;
    ResourceId store = 1;
    TxId t1 = 1;
    ResourceId r1 = 1;
    LockManager::LockStatus status;

    // acquire a shared record lock
    ASSERT(! lm.isLocked(t1, LockManager::kShared, store, r1));
    lm.acquire(t1, LockManager::kShared, store, r1);
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));

    // release a shared record lock
    lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kShared, store, r1));

    // acquire a shared record lock twice, on same ResourceId
    lm.acquire(t1, LockManager::kShared, store, r1);
    lm.acquire(t1, LockManager::kShared, store, r1);
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));

    // release the twice-acquired lock, once.  Still locked
    status = lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(LockManager::kLockCountDecremented == status);
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));

    // after 2nd release, it's not locked
    status = lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(LockManager::kLockReleased == status);
    ASSERT(!lm.isLocked(t1, LockManager::kShared, store, r1));



    // --- test downgrade and release ---

    // acquire an exclusive then a shared lock, on the same ResourceId
    lm.acquire(t1, LockManager::kExclusive, store, r1);
    ASSERT(lm.isLocked(t1, LockManager::kExclusive, store, r1));
    lm.acquire(t1, LockManager::kShared, store, r1);
    ASSERT(lm.isLocked(t1, LockManager::kExclusive, store, r1));
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));

    // release shared first, then exclusive
    lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kShared, store, r1));
    ASSERT(lm.isLocked(t1, LockManager::kExclusive, store, r1));
    lm.release(t1, LockManager::kExclusive, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kExclusive, store, r1));

    // release exclusive first, then shared
    lm.acquire(t1, LockManager::kExclusive, store, r1);
    lm.acquire(t1, LockManager::kShared, store, r1);
    lm.release(t1, LockManager::kExclusive, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kExclusive, store, r1));
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));
    lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kShared, store, r1));



    // --- test upgrade and release ---

    // acquire a shared, then an exclusive lock on the same ResourceId
    lm.acquire(t1, LockManager::kShared, store, r1);
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));
    lm.acquire(t1, LockManager::kExclusive, store, r1);
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));
    ASSERT(lm.isLocked(t1, LockManager::kExclusive, store, r1));

    // release exclusive first, then shared
    lm.release(t1, LockManager::kExclusive, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kExclusive, store, r1));
    ASSERT(lm.isLocked(t1, LockManager::kShared, store, r1));
    lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kShared, store, r1));

    // release shared first, then exclusive
    lm.acquire(t1, LockManager::kShared, store, r1);
    lm.acquire(t1, LockManager::kExclusive, store, r1);
    lm.release(t1, LockManager::kShared, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kShared, store, r1));
    ASSERT(lm.isLocked(t1, LockManager::kExclusive, store, r1));
    lm.release(t1, LockManager::kExclusive, store, r1);
    ASSERT(! lm.isLocked(t1, LockManager::kExclusive, store, r1));
}

TEST(LockManagerTest, TxConflict) {
    LockManager lm;
    ClientTransaction t1(&lm, 1);
    ClientTransaction t2(&lm, 2);

    // no conflicts with shared locks on same/different objects

    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 2, ACQUIRED);
    t1.acquire(LockManager::kShared, 2, ACQUIRED);
    t2.acquire(LockManager::kShared, 1, ACQUIRED);

    t1.release(LockManager::kShared, 1);
    t1.release(LockManager::kShared, 2);
    t2.release(LockManager::kShared, 1);
    t2.release(LockManager::kShared, 2);


    // no conflicts with exclusive locks on different objects
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t2.acquire(LockManager::kExclusive, 2, ACQUIRED);
    t1.release(LockManager::kExclusive, 1);
    t2.release(LockManager::kExclusive, 2);


    // shared then exclusive conflict
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    // t2's request is incompatible with t1's lock, so it should block
    t2.acquire(LockManager::kExclusive, 1, BLOCKED);
    t1.release(LockManager::kShared, 1);
    t2.wakened(); // with t1's lock released, t2 should wake
    t2.release(LockManager::kExclusive, 1);

    // exclusive then shared conflict
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 1, BLOCKED);
    t1.release(LockManager::kExclusive, 1);
    t2.wakened();
    t2.release(LockManager::kShared, 1);

    // exclusive then exclusive conflict
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t2.acquire(LockManager::kExclusive, 1, BLOCKED);
    t1.release(LockManager::kExclusive, 1);
    t2.wakened();
    t2.release(LockManager::kExclusive, 1);

    t1.quit();
    t2.quit();
}

TEST(LockManagerTest, TxDeadlock) {
    LockManager lm(LockManager::kPolicyReadersFirst);
    ClientTransaction t1(&lm, 1);
    ClientTransaction t2(&lm, 2);

    ClientTransaction a1(&lm, 4);
    ClientTransaction a2(&lm, 5);
    ClientTransaction a3(&lm, 6);
    ClientTransaction a4(&lm, 7);
    ClientTransaction a5(&lm, 8);

    // simple deadlock test 1
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    a1.acquire(LockManager::kShared, 2, ACQUIRED);
    t1.acquire(LockManager::kExclusive, 2, BLOCKED);
    // a1's request would form a dependency cycle, so it should abort
    a1.acquire(LockManager::kExclusive, 1, ABORTED);
    t1.wakened(); // with t2's locks released, t1 should wake
    t1.release(LockManager::kExclusive, 2);
    t1.release(LockManager::kShared, 1);

    // simple deadlock test 2
    a2.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 2, ACQUIRED);
    t2.acquire(LockManager::kExclusive, 1, BLOCKED);
    // a2's request would form a dependency cycle, so it should abort
    a2.acquire(LockManager::kExclusive, 2, ABORTED);
    t2.wakened(); // with a2's locks released, t2 should wake
    t2.release(LockManager::kExclusive, 1);
    t2.release(LockManager::kShared, 2);

    // three way deadlock
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 2, ACQUIRED);
    a3.acquire(LockManager::kShared, 3, ACQUIRED);
    t1.acquire(LockManager::kExclusive, 2, BLOCKED);
    t2.acquire(LockManager::kExclusive, 3, BLOCKED);
    // a3's request would form a dependency cycle, so it should abort
    a3.acquire(LockManager::kExclusive, 1, ABORTED);
    t2.wakened(); // with a3's lock release, t2 should wake
    t2.release(LockManager::kShared, 2);
    t1.wakened(); // with t2's locks released, t1 should wake
    t2.release(LockManager::kExclusive, 3);
    t1.release(LockManager::kShared, 1);
    t1.release(LockManager::kExclusive, 2);

    // test for phantom deadlocks
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kExclusive, 1, BLOCKED);
    t1.release(LockManager::kShared, 1);
    t2.wakened();
    // at this point, t2 should no longer be waiting for t1
    // so it should be OK for t1 to wait for t2
    t1.acquire(LockManager::kShared, 1, BLOCKED);
    t2.release(LockManager::kExclusive, 1);
    t1.wakened();
    t1.release(LockManager::kShared, 1);

    // test for missing deadlocks
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 2, ACQUIRED); // setup for deadlock with a4
    t2.acquire(LockManager::kExclusive, 1, BLOCKED); // block on t1
    // after this, because readers first policy, t2 should
    // also be waiting on a4.
    a4.acquire(LockManager::kShared, 1, ACQUIRED);
    // after this, t2 should be waiting ONLY on a4
    t1.release(LockManager::kShared, 1);
    // So a4 should not be allowed to wait on t2's resource.
    a4.acquire(LockManager::kExclusive, 2, ABORTED);
    t2.wakened();
    t2.release(LockManager::kShared, 2);
    t2.release(LockManager::kExclusive, 1);


    // test for missing deadlocks: due to downgrades
    a5.acquire(LockManager::kExclusive, 1, ACQUIRED);
    a5.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 2, ACQUIRED); // setup for deadlock with a5
    t2.acquire(LockManager::kExclusive, 1, BLOCKED); // block on a5
    a5.release(LockManager::kExclusive, 1);
    // at this point, t2 should still be blocked on a5's downgraded lock
    // So a5 should not be allowed to wait on t2's resource.
    a5.acquire(LockManager::kExclusive, 2, ABORTED);
    t2.wakened();
    t2.release(LockManager::kShared, 2);
    t2.release(LockManager::kExclusive, 1);

    t1.quit();
    t2.quit();
}

TEST(LockManagerTest, TxDowngrade) {
    LockManager lm;
    ClientTransaction t1(&lm, 1);
    ClientTransaction t2(&lm, 2);

    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t1.acquire(LockManager::kShared, 1, ACQUIRED); // downgrade
    // t1 still has exclusive on resource 1, so t2 must wait
    t2.acquire(LockManager::kShared, 1, BLOCKED);
    t1.release(LockManager::kExclusive, 1);
    t2.wakened(); // with the exclusive lock released, t2 wakes
    t1.release(LockManager::kShared, 1);
    t2.release(LockManager::kShared, 1);

    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t1.acquire(LockManager::kShared, 1, ACQUIRED); // downgrade
    // t1 still has exclusive on resource 1, so t2 must wait
    t2.acquire(LockManager::kShared, 1, BLOCKED);
    t1.release(LockManager::kShared, 1);
    // with t1 still holding exclusive on resource 1, t2 still blocked
    t1.release(LockManager::kExclusive, 1);
    t2.wakened(); // with the exclusive lock released, t2 wakes
    t2.release(LockManager::kShared, 1);

    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    // t1 has exclusive on resource 1, so t2 must wait
    t2.acquire(LockManager::kShared, 1, BLOCKED);
    // even though t2 is waiting for resource 1, t1 can still use it shared,
    // because it already owns exclusive lock and can't block on itself
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t1.release(LockManager::kExclusive, 1);
    t2.wakened(); // with the exclusive lock released, t2 wakes
    t1.release(LockManager::kShared, 1);
    t2.release(LockManager::kShared, 1);

    // t2 acquires exclusive during t1's downgrade
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kExclusive, 1, BLOCKED);
    t1.release(LockManager::kExclusive, 1);
    t1.release(LockManager::kShared, 1);
    t2.wakened();
    t2.release(LockManager::kExclusive, 1);

    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    t2.acquire(LockManager::kExclusive, 1, BLOCKED);
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t1.release(LockManager::kExclusive, 1);
    t1.release(LockManager::kShared, 1);
    t2.wakened();
    t2.release(LockManager::kExclusive, 1);

    t1.quit();
    t2.quit();
}

TEST(LockManagerTest, TxUpgrade) {
    LockManager lm(LockManager::kPolicyReadersFirst);
    ClientTransaction t1(&lm, 1);
    ClientTransaction t2(&lm, 2);
    ClientTransaction t3(&lm, 3);

    ClientTransaction a2(&lm, 4);
    ClientTransaction a3(&lm, 5);

    // test upgrade succeeds, blocks subsequent reads
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED); // upgrade
    t2.acquire(LockManager::kShared, 1, BLOCKED);
    t1.release(LockManager::kExclusive, 1);
    t2.wakened();
    t1.release(LockManager::kShared, 1);
    t2.release(LockManager::kShared, 1);

    // test upgrade blocks, then wakes
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 1, ACQUIRED);
    // t1 can't use resource 1 exclusively yet, because t2 is using it
    t1.acquire(LockManager::kExclusive, 1, BLOCKED);
    t2.release(LockManager::kShared, 1);
    t1.wakened(); // with t2's shared lock released, t1 wakes
    t1.release(LockManager::kExclusive, 1);
    t1.release(LockManager::kShared, 1);

    // test upgrade blocks on several, then wakes
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 1, ACQUIRED);
    // t1 can't use resource 1 exclusively yet, because t2 is using it
    t1.acquire(LockManager::kExclusive, 1, BLOCKED);
    t3.acquire(LockManager::kShared, 1, ACQUIRED); // additional blocker
    t2.release(LockManager::kShared, 1); // t1 still blocked
    t3.release(LockManager::kShared, 1);
    t1.wakened(); // with t3's shared lock released, t1 wakes
    t1.release(LockManager::kExclusive, 1);
    t1.release(LockManager::kShared, 1);

    // failure to upgrade
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    a2.acquire(LockManager::kShared, 1, ACQUIRED);
    t1.acquire(LockManager::kExclusive, 1, BLOCKED);
    a2.acquire(LockManager::kExclusive, 1, ABORTED);
    // with a2's abort, t1 can wake
    t1.wakened();
    t1.release(LockManager::kShared, 1);
    t1.release(LockManager::kExclusive, 1);

    // failure to upgrade
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.acquire(LockManager::kShared, 1, ACQUIRED);
    t1.acquire(LockManager::kExclusive, 1, BLOCKED);
    a3.acquire(LockManager::kShared, 1, ACQUIRED);
    t2.release(LockManager::kShared, 1); // t1 still blocked on a3
    a3.acquire(LockManager::kExclusive, 1, ABORTED);

    t1.quit();
    t2.quit();
    t3.quit();
}

TEST(LockManagerTest, TxPolicy) {

    {
        // Test FirstComeFirstServe policy
        LockManager lm_first;
        ClientTransaction t1(&lm_first, 1);
        ClientTransaction t2(&lm_first, 2);
        ClientTransaction t3(&lm_first, 3);
        // test1
        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);
        // t2 should wake first, because its request came before t3's
        t2.wakened();
        t2.release(LockManager::kShared, 1);
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);

        // test2
        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);
        // t3 should wake first, because its request came before t2's
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);
        t2.wakened();
        t2.release(LockManager::kShared, 1);

        t1.quit();
        t2.quit();
        t3.quit();
    }

    {
        // Test kPolicyReadersFirst
        // shared request are considered read requests

        LockManager lm_readers(LockManager::kPolicyReadersFirst);
        ClientTransaction t1(&lm_readers, 1);
        ClientTransaction t2(&lm_readers, 2);
        ClientTransaction t3(&lm_readers, 3);

        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);

        // t2 should wake first, even though t3 came first in time
        // because t2 is a reader and t3 is a writer
        t2.wakened();
        t2.release(LockManager::kShared, 1);
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);

        t1.quit();
        t2.quit();
        t3.quit();
    }

    {
        // Test OLDEST_TX_FIRST policy
        // for now, smaller TxIds are considered older

        LockManager lm_oldest(LockManager::kPolicyOldestTxFirst);
        ClientTransaction t1(&lm_oldest, 1);
        ClientTransaction t2(&lm_oldest, 2);
        ClientTransaction t3(&lm_oldest, 3);

        // test 1
        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);

        // t2 should wake first, even though t3 came first in time
        // because t2 is older than t3
        t2.wakened();
        t2.release(LockManager::kShared, 1);
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);

        // test 2
        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);

        // t2 should wake first, because it's older than t3
        t2.wakened();
        t2.release(LockManager::kShared, 1);
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);

        t1.quit();
        t2.quit();
        t3.quit();
    }

    {
        LockManager lm_blockers(LockManager::kPolicyBlockersFirst);
        ClientTransaction t1(&lm_blockers, 1);
        ClientTransaction t2(&lm_blockers, 2);
        ClientTransaction t3(&lm_blockers, 3);
        ClientTransaction t4(&lm_blockers, 4);

        // BIGGEST_BLOCKER_FIRST policy

        // set up t3 as the biggest blocker
        t3.acquire(LockManager::kExclusive, 2, ACQUIRED);
        t4.acquire(LockManager::kExclusive, 2, BLOCKED);

        // test 1
        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);
        // t3 should wake first, because it's a bigger blocker than t2
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);
        t2.wakened();
        t2.release(LockManager::kShared, 1);

        // test 2
        t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
        t2.acquire(LockManager::kShared, 1, BLOCKED);
        t3.acquire(LockManager::kExclusive, 1, BLOCKED);
        t1.release(LockManager::kExclusive, 1);
        // t3 should wake first, even though t2 came first,
        // because it's a bigger blocker than t2
        t3.wakened();
        t3.release(LockManager::kExclusive, 1);
        t2.wakened();
        t2.release(LockManager::kShared, 1);

        t3.release(LockManager::kExclusive, 2);
        t4.wakened();
        t4.release(LockManager::kExclusive, 2);

        t1.quit();
        t2.quit();
        t3.quit();
        t4.quit();
    }
}

/*
 * test kPolicyReadersOnly and kPolicyWritersOnly
 */
TEST(LockManagerTest, TxOnlyPolicies) {
    LockManager lm;
    ClientTransaction t1(&lm, 1);
    ClientTransaction t2(&lm, 2);
    ClientTransaction t3(&lm, 3);
    ClientTransaction t4(&lm, 4);
    ClientTransaction t5(&lm, 5);
    ClientTransaction tp(&lm, 6);

    // show kPolicyReadersOnly blocking writers, which
    // awake when policy reverts
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    tp.setPolicy(LockManager::kPolicyReadersOnly, ACQUIRED);
    t3.acquire(LockManager::kExclusive, 2, BLOCKED); // just policy conflict
    t4.acquire(LockManager::kExclusive, 1, BLOCKED); // both policy & t1
    t5.acquire(LockManager::kShared, 1, ACQUIRED);   // even tho t4
    tp.setPolicy(LockManager::kPolicyReadersFirst, ACQUIRED);
    t3.wakened();
    t3.release(LockManager::kExclusive, 2);
    t1.release(LockManager::kShared, 1);
    t5.release(LockManager::kShared, 1);
    t4.wakened();
    t4.release(LockManager::kExclusive, 1);

    // show WRITERS_ONLY blocking readers, which
    // awake when policy reverts
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    tp.setPolicy(LockManager::kPolicyWritersOnly, ACQUIRED);
    t3.acquire(LockManager::kShared, 2, BLOCKED);       // just policy conflict
    t4.acquire(LockManager::kShared, 1, BLOCKED);       // both policy & t1
    t1.release(LockManager::kExclusive, 1);
    t5.acquire(0x3/*LockManager::kExclusive*/, 2, ACQUIRED);   // even tho t3
    t5.release(LockManager::kExclusive, 2);
    tp.setPolicy(LockManager::kPolicyReadersFirst, ACQUIRED);
    t3.wakened();
    t3.release(LockManager::kShared, 2);
    t4.wakened();
    t4.release(LockManager::kShared, 1);

    // show READERS_ONLY blocked by existing writer
    // but still blocking new writers
    t1.acquire(LockManager::kExclusive, 1, ACQUIRED);
    tp.setPolicy(LockManager::kPolicyReadersOnly, BLOCKED);  // blocked by t1
    t2.acquire(LockManager::kExclusive, 2, BLOCKED);   // just policy conflict
    t3.acquire(LockManager::kShared, 2, ACQUIRED);     // even tho t2
    t3.release(LockManager::kShared, 2);
    t1.release(LockManager::kExclusive, 1);
    tp.wakened();
    tp.setPolicy(LockManager::kPolicyReadersFirst, ACQUIRED);
    t2.wakened();
    t2.release(LockManager::kExclusive, 2);

    // show WRITERS_ONLY blocked by existing reader
    // but still blocking new readers
    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    tp.setPolicy(LockManager::kPolicyWritersOnly, BLOCKED);  // blocked by t1
    t2.acquire(LockManager::kShared, 2, BLOCKED);      // just policy conflict
    t1.release(LockManager::kShared, 1);
    tp.wakened();
    tp.setPolicy(LockManager::kPolicyReadersFirst, ACQUIRED);
    t2.wakened();
    t2.release(LockManager::kShared, 2);

    t1.quit();
    t2.quit();
    t3.quit();
    t4.quit();
    t5.quit();
    tp.quit();
}

TEST(LockManagerTest, TxShutdown) {
    LockManager lm;
    ClientTransaction t1(&lm, 1);
    ClientTransaction t2(&lm, 2);

    t1.acquire(LockManager::kShared, 1, ACQUIRED);
    lm.shutdown(3000);

    // t1 can still do work while quiescing
    t1.release(LockManager::kShared, 1);
    t1.acquire(LockManager::kShared, 2, ACQUIRED);

    // t2 is new and should be refused
    t2.acquire(LockManager::kShared, 3, ABORTED);

    // after the quiescing period, t1's request should be refused
    sleep(3);
    t1.acquire(LockManager::kShared, 4, ABORTED);
}
}
