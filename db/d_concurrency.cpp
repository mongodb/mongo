// @file d_concurrency.cpp 

#include "pch.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "../util/concurrency/value.h"
#include "../util/assert_util.h"
#include "client.h"
#include "namespacestring.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

namespace mongo {

    class LockBits {
    public:
        unsigned& x;
        enum Level { 
            LockExcluder = 0,
            Global = 1,
            Collection = 2,
            OplogCollection = 3
        };
        enum State { 
            NotLocked = 0,
            //AwaitShared = 1, // todo...
            Shared = 2,
            // todo: upgradable...
            //AwaitExclusive = 4,
            Exclusive = 8
        };
        enum { B = 8, MASK = 0xff } ;
        LockBits(unsigned &_x) : x(_x) { }
        bool anythingExclusive() const { 
            return (0x88888888 & x) != 0;
        }
        State get(Level l) const { 
            unsigned msk = MASK << (l*B);
            return static_cast<State>((x&msk) >> (l*B));
        }
        void set(Level l, State old, State _new) {
            unsigned msk = MASK << (l*B);
            verify(0, get(l)==old);
            x = (x&~msk) | (_new << (l*B));
        }
    };

    void MongoMutex::lockedExclusively() {
        Client& c = cc();
        curopGotLock(&c); // hopefully lockStatus replaces one day
        LockBits b = c.lockStatus.state;
        b.set(LockBits::Global, LockBits::NotLocked, LockBits::Exclusive);
        _minfo.entered(); // hopefully eliminate one day 
    }

    void MongoMutex::unlockingExclusively() {
        Client& c = cc();
        _minfo.leaving();
        LockBits b = c.lockStatus.state;
        b.set(LockBits::Global, LockBits::Exclusive, LockBits::NotLocked);
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

    // we will use the global write lock for writing to system.* collections for simplicity 
    // for now; this has some advantages in speed as we don't need to latch just for that then; 
    // also there are cases to be handled carefully otherwise such as namespacedetails methods
    // reaching into system.indexes implicitly
    // exception : system.profile
    static bool lkspecial(const string& ns) { 
        NamespaceString s(ns);
        return s.isSystem() && s.coll != "system.profile";
    }

    /** we want to be able to block any attempted write while allowing reads; additionally 
        force non-greedy acquisition so that reads can continue -- 
        that is, disallow greediness of write lock acquisitions.  This is for that purpose.  The 
        #1 need is by groupCommitWithLimitedLocks() but useful elsewhere such as for lock and fsync.
    */
    SimpleRWLock writeExcluder;
    ExcludeAllWrites::ExcludeAllWrites() : 
        lk(writeExcluder)
    {
        LOG(3) << "ExcludeAllWrites" << endl;
        wassert( !dbMutex.isWriteLocked() );
        LockBits b(cc().lockStatus.state);
        wassert( b.x == 0 );
        b.set(LockBits::LockExcluder, LockBits::NotLocked, LockBits::Exclusive);
    };
    ExcludeAllWrites::~ExcludeAllWrites() {
        LockBits(cc().lockStatus.state).set(LockBits::LockExcluder, LockBits::Exclusive, LockBits::NotLocked);
    }

    // CLC turns on the "collection level concurrency" code 
    // (which is under development and not finished)
#if defined(CLC)
    // called after a context is set. check that the correct collection is locked
    void Client::checkLocks() const { 
        DEV {
            if( !dbMutex.isWriteLocked() ) { 
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

    Client::LockStatus::LockStatus() { 
        state = 0;
    }

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

    LockCollectionForWriting::Locks::Locks(string ns) : 
        excluder(writeExcluder),
        gslk(),
        clk(theLocks.forns(ns),true)
    { }
    LockCollectionForWriting::~LockCollectionForWriting() { 
        if( locks.get() ) {
            Client::LockStatus& s = cc().lockStatus;
            s.whichCollection.clear();
            LockBits b( s.state );
            wassert( b.get(LockBits::OplogCollection) == LockBits::NotLocked ); // todo ...
            b.set(LockBits::Collection, LockBits::Exclusive, LockBits::NotLocked);
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

    LockCollectionForReading::Locks::Locks(string ns) : 
      gslk(),
      clk( theLocks.forns(ns) ) 
    { }
    LockCollectionForReading::~LockCollectionForReading() {
        Client::LockStatus& s = cc().lockStatus;
        wassert( !s.whichCollection.empty() );
        if( locks.get() ) {
            s.whichCollection.clear();
            LockBits b(s.state);
            b.set(LockBits::Collection, LockBits::Shared, LockBits::NotLocked);
        }
    }
    LockCollectionForReading::LockCollectionForReading(string coll)
    {
        Client::LockStatus& s = cc().lockStatus;
        if( !s.whichCollection.empty() ) {
            if( !subcollectionOf(s.whichCollection, coll.c_str()) ) {
                if( lkspecial(coll) )
                    return;
                massert(15939, 
                    str::stream() << "can't nest lock of " << coll << " beneath " << s.whichCollection, 
                    false);
            }
            // already locked, so done; might have been a write lock.
            return;
        }
        LockBits b(s.state);
        s.whichCollection = coll; 
        b.set(LockBits::Collection, LockBits::NotLocked, LockBits::Shared);
        locks.reset( new Locks(coll) );
    }

}
