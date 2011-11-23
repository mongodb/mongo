#include "pch.h"
#include "database.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "client.h"
#include "../util/assert_util.h"

#if defined(CLC)
   
namespace mongo {

    LockDatabaseSharable::LockDatabaseSharable() {
        Client& c = cc();
        Client::LockStatus& s = c.lockStatus;
        already = false;
        if( dbMutex.isWriteLocked() ) {
            already = true;
            assert( s.dbLockCount == 0 );
            return;
        }
        Database *db = c.database();
        assert( db );
        if( s.dbLockCount == 0 ) {
            s.whichDB = db;
            db->dbLock.lock_shared();
        }
        else {
            // recursed
            massert( 15919, "wrong database while locking", db == s.whichDB);
        }
        s.dbLockCount--; // < 0 means sharable
    }

    LockDatabaseSharable::~LockDatabaseSharable() { 
        if( already ) 
            return;
        Client& c = cc();
        Client::LockStatus& s = c.lockStatus;
        wassert( c.database() == s.whichDB );
        wassert( s.dbLockCount < 0 );
        if( ++s.dbLockCount == 0 ) { 
            c.database()->dbLock.unlock_shared();
        }
    }

    bool subcollectionOf(const string& parent, const char *child);

    /** notes
        note if we r and w lock arbitarily with nested rwlocks we can deadlock. so we avoid this.
        also have to be careful about any throws in things like this
    */
    LockCollectionForReading::LockCollectionForReading(const char *ns)
    {
        Client& c = cc();
        Client::LockStatus& s = c.lockStatus;
        already = false;
        if( dbMutex.isWriteLocked() || s.dbLockCount > 0 ) { 
            // already locked exclusively at a higher level in the hierarchy
            already = true;
            assert( s.collLockCount == 0 );
            return;
        }

        assert( c.ns() && ns && str::equals(c.ns(),ns) );
        if( s.collLockCount == 0 ) {
            s.whichCollection = ns;
            s.collLock.lock_shared();
        }
        else {
            // must be the same ns or a child ns
            assert( subcollectionOf(s.whichCollection, ns) );
            if( s.whichCollection != ns ) {
                DEV log() << "info lock on nested ns: " << ns << endl;
            }
        }
        s.collLockCount--; // < 0 means sharable state
    }

    LockCollectionForReading::~LockCollectionForReading() { 
        if( already ) 
            return;
        Client& c = cc();
        Client::LockStatus& s = c.lockStatus;
        wassert( c.ns() && s.whichCollection == c.ns() );
        wassert( s.collLockCount < 0 );
        if( ++s.collLockCount == 0 ) { 
            s.collLock.unlock_shared();
        }
    }

}

#endif

