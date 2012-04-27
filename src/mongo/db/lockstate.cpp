// lockstate.cpp

#include "pch.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/namespacestring.h"
#include "mongo/db/client.h"
#include "mongo/util/mongoutils/str.h"
#include "lockstate.h"

namespace mongo {

    LockState::LockState() 
        : _recursive(0), 
          _threadState(0),
          _whichNestable( Lock::notnestable ),
          _nestableCount(0), 
          _otherCount(0), 
          _otherLock(NULL),
          _scopedLk(NULL)
    {
    }

    bool LockState::isRW() const { 
        return _threadState == 'R' || _threadState == 'W'; 
    }

    bool LockState::isW() const { 
        return _threadState == 'W'; 
    }

    bool LockState::hasAnyReadLock() const { 
        return _threadState == 'r' || _threadState == 'R';
    }

    bool LockState::isLocked( const StringData& ns ) {
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns.data(), db);
        
        DEV verify( _otherName.find( '.' ) == string::npos ); // XXX this shouldn't be here, but somewhere
        if ( _otherCount && db == _otherName )
            return true;

        if ( _nestableCount ) {
            if ( mongoutils::str::equals( db , "local" ) )
                return _whichNestable == Lock::local;
            if ( mongoutils::str::equals( db , "admin" ) )
                return _whichNestable == Lock::admin;
        }

        return false;
    }

    void LockState::locked( char newState ) {
        _threadState = newState;
    }
    void LockState::unlocked() {
        _threadState = 0;
    }

    void LockState::changeLockState( char newState ) {
        fassert( 16169 , _threadState != 0 );
        _threadState = newState;
    }

    static string kind(int n) { 
        if( n > 0 )
            return "W";
        if( n < 0 ) 
            return "R";
        return "?";
    }

    
    /** Note: this is is called by the currentOp command, which is a different 
              thread. So be careful about thread safety here. For example reading 
              this->otherName would not be safe as-is!
    */
    void LockState::reportState(BSONObjBuilder& res) {
        BSONObjBuilder b;
        if( _threadState ) {
            char buf[2];
            buf[0] = _threadState; 
            buf[1] = 0;
            b.append(".", buf);
        }
        if( _nestableCount ) {
            string s = "?";
            if( _whichNestable == Lock::local ) 
                s = ".local";
            else if( _whichNestable == Lock::admin ) 
                s = ".admin";
            b.append(s, kind(_nestableCount));
        }
        if( _otherCount ) { 
            WrapperForRWLock *k = _otherLock;
            if( k ) {
                string s = ".";
                s += k->name();
                b.append(s, kind(_otherCount));
            }
        }
        BSONObj o = b.obj();
        if( !o.isEmpty() ) 
            res.append("locks", o);
    }

    void LockState::Dump() {
        cc().lockState().dump();
    }
    void LockState::dump() {
        char s = _threadState;
        stringstream ss;
        ss << "lock status: ";
        if( s == 0 ){
            ss << "unlocked"; 
        }
        else {
            ss << s;
            if( _recursive ) { 
                ss << " recursive:" << _recursive;
            }
            ss << " otherCount:" << _otherCount;
            if( _otherCount ) {
                ss << " otherdb:" << _otherName;
            }
            if( _nestableCount ) {
                ss << " nestableCount:" << _nestableCount << " which:";
                if( _whichNestable == Lock::local ) 
                    ss << "local";
                else if( _whichNestable == Lock::admin ) 
                    ss << "admin";
                else 
                    ss << (int)_whichNestable;
            }
        }
        log() << ss.str() << endl;
    }

    void LockState::enterScopedLock( Lock::ScopedLock* lock ) {
        _recursive++;
        if ( _recursive == 1 ) {
            fassert(16115, _scopedLk == 0);
            _scopedLk = lock;
        }
    }

    Lock::ScopedLock* LockState::leaveScopedLock() {
        _recursive--;
        dassert( _recursive < 10000 );
        Lock::ScopedLock* temp = _scopedLk;

        if ( _recursive > 0 ) {
            return NULL;
        }
        
        _scopedLk = NULL;
        return temp;
    }

    void LockState::lockedNestable( Lock::Nestable what , int type) {
        verify( type );
        _whichNestable = what;
        _nestableCount += type;
    }

    void LockState::unlockedNestable() {
        _whichNestable = Lock::notnestable;
        _nestableCount = 0;
    }

    void LockState::lockedOther( const string& other , int type , WrapperForRWLock* lock ) {
        fassert( 16170 , _otherCount == 0 );
        _otherName = other;
        _otherCount = type;
        _otherLock = lock;
    }

    void LockState::unlockedOther() {
        _otherName = "";
        _otherCount = 0;
        _otherLock = 0;
    }


}
