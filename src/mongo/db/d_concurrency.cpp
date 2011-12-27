// @file d_concurrency.cpp 

#include "pch.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "../util/concurrency/mapsf.h"
#include "../util/assert_util.h"
#include "client.h"
#include "namespacestring.h"
#include "d_globals.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

namespace mongo {

    using namespace clcimpl;

    Client::LockStatus::LockStatus() { 
        excluder=global=collection=0;
    }

    namespace clcimpl {
        Shared::Shared(unsigned& _state, RWLock& lock) : state(_state) {
            rw = 0;
            if( state ) { 
                // already locked
                dassert( (state & (AcquireShared|AcquireExclusive)) == 0 );
                return;
            }
            rw = &lock;
            state = AcquireShared;
            rw->lock_shared();
            state = LockedShared;
        }
        Shared::~Shared() { 
            if( rw ) {
                state = Unlocked;
                rw->unlock_shared();
            }
        }
        Exclusive::Exclusive(unsigned& _state, RWLock& lock) : state(_state) { 
            rw = 0;
            if( state ) { 
                // already locked
                dassert( (state & (AcquireShared|AcquireExclusive)) == 0 );
                assert( state == LockedExclusive ); // can't be in shared state
                return;
            }
            rw = &lock;
            state = AcquireExclusive;
            rw->lock();
            state = LockedExclusive;
        }
        Exclusive::~Exclusive() { 
            if( rw ) {
                state = Unlocked;
                rw->unlock();
            }
        }
    } // clcimpl namespace

    // this tie-in temporary until MongoMutex is folded in more directly.
    // called when the lock has been achieved
    void MongoMutex::lockedExclusively() {
        Client& c = cc();
        curopGotLock(&c); // hopefully lockStatus replaces one day
        c.lockStatus.global = clcimpl::LockedExclusive;
        _minfo.entered(); // hopefully eliminate one day 
    }

    void MongoMutex::unlockingExclusively() {
        Client& c = cc();
        _minfo.leaving();
        c.lockStatus.global = Unlocked;
    }

    MongoMutex::MongoMutex(const char *name) : _m(name) {
        static int n = 0;
        assert( ++n == 1 ); // below releasingWriteLock we assume MongoMutex is a singleton, and uses dbMutex ref above
        _remapPrivateViewRequested = false;
    }

    bool subcollectionOf(const string& parent, const char *child) {
        if( parent == child ) 
            return true;
        if( !str::startsWith(child, parent) )
            return false;
        const char *p = child + parent.size();
        uassert(15963, str::stream() << "bad collection name: " << child, !str::endsWith(p, '.'));
        return *p == '.' && p[1] == '$';
    }

    // (maybe tbd) ...
    // we will use the global write lock for writing to system.* collections for simplicity 
    // for now; this has some advantages in speed as we don't need to latch just for that then; 
    // also there are cases to be handled carefully otherwise such as namespacedetails methods
    // reaching into system.indexes implicitly
    // exception : system.profile
    static bool lkspecial(const string& ns) { 
        NamespaceString s(ns);
        return s.isSystem() && s.coll != "system.profile";
    }

    /** Notes on d.writeExcluder
        we want to be able to block any attempted write while allowing reads; additionally 
        force non-greedy acquisition so that reads can continue -- 
        that is, disallow greediness of write lock acquisitions.  This is for that purpose.  The 
        #1 need is by groupCommitWithLimitedLocks() but useful elsewhere such as for lock and fsync.
    */

    ExcludeAllWrites::ExcludeAllWrites() : 
        lk(cc().lockStatus.excluder, d.writeExcluder), 
        gslk()
    {
        LOG(3) << "ExcludeAllWrites" << endl;
        wassert( !d.dbMutex.isWriteLocked() );
    };
    ExcludeAllWrites::~ExcludeAllWrites() {
    }

    // CLC turns on the "collection level concurrency" code 
    // (which is under development and not finished)
#if defined(CLC)
    // called after a context is set. check that the correct collection is locked
    void Client::checkLocks() const { 
        DEV {
            if( !d.dbMutex.isWriteLocked() ) { 
                const char *n = ns();
                if( lockStatus.whichCollection.empty() ) { 
                    log() << "DEBUG checkLocks error expected to already be locked: " << n << endl;
                    dassert(false);
                }
                dassert( subcollectionOf(lockStatus.whichCollection, n) || lkspecial(n) );
            }
        }
    }
#endif

    // we don't keep these locks in the namespacedetailstransient and Database 
    // objects -- that makes things safer as we need not prove to ourselves that they 
    // are always in scope when we need them.
    // todo: we don't clean these locks up yet.
    // todo: avoiding the mutex here might be nice.
    class LockObjectForEachCollection {
        //mapsf<string,RWLock*> dblocks;
        mapsf<string,RWLock*> nslocks;
    public:
        /*RWLock& fordb(string db) { 
            mapsf<string,RWLock*>::ref r(dblocks);
            RWLock*& rw = r[db];
            if( rw == 0 )
                rw = new RWLock(0);
            return *rw;
        }*/
        RWLock& forns(string ns) { 
            mapsf<string,RWLock*>::ref r(nslocks);
#if defined(CLC)
            massert(15964, str::stream() << "bad collection name to lock: " << ns, str::contains(ns, '.'));
#endif
            RWLock*& rw = r[ns];
            if( rw == 0 ) { 
                rw = new RWLock(0);
            }
            return *rw;
        }
    } theLocks;

#if defined(CLC)
    LockCollectionForWriting::Locks::Locks(string ns) : 
        excluder(d.writeExcluder),
        gslk(),
        clk(theLocks.forns(ns),true)
    { }
    LockCollectionForWriting::~LockCollectionForWriting() { 
        if( locks.get() ) {
            Client::LockStatus& s = cc().lockStatus;
            s.whichCollection.clear();
        }
    }
    LockCollectionForWriting::LockCollectionForWriting(string coll)
    {
        Client::LockStatus& s = cc().lockStatus;
        LockBits b(s.state);
        if( !s.whichCollection.empty() ) {
            if( !subcollectionOf(s.whichCollection, coll.c_str()) ) { 
                massert(15937, str::stream() << "can't nest lock of " << coll << " beneath " << s.whichCollection, false);
            }
            if( b.get(LockBits::Collection) != LockBits::Exclusive ) {
                massert(15938, str::stream() << "want collection write lock but it is already read locked " << s.state, false);
            }
            return;
        }
        verify(15965, !lkspecial(coll)); // you must global write lock for writes to special's
        s.whichCollection = coll;
        b.set(LockBits::Collection, LockBits::NotLocked, LockBits::Exclusive);
        locks.reset( new Locks(coll) );
    }    
#endif

    LockCollectionForReading::LockCollectionForReading(string ns) : 
      gslk(),
      clk( cc().lockStatus.collection, theLocks.forns(ns) ) 
    { 
        Client::LockStatus& s = cc().lockStatus;
        if( s.whichCollection.empty() ) {
            s.whichCollection = ns;
        }
        else {
            if( !subcollectionOf(s.whichCollection, ns.c_str()) ) {
                if( lkspecial(ns) )
                    return;
                massert(15939, 
                    str::stream() << "can't nest lock of " << ns << " beneath " << s.whichCollection, 
                    false);
            }
        }
    }
    LockCollectionForReading::~LockCollectionForReading() {
        if( !clk.recursed() ) {
            Client::LockStatus& s = cc().lockStatus;
            s.whichCollection.clear();
        }
    }

}
