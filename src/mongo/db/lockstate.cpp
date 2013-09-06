// lockstate.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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


#include "mongo/pch.h"

#include "mongo/db/lockstate.h"

#include "mongo/db/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/client.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    LockState::LockState() 
        : _batchWriter(false),
          _recursive(0),
          _threadState(0),
          _whichNestable( Lock::notnestable ),
          _nestableCount(0), 
          _otherCount(0), 
          _otherLock(NULL),
          _scopedLk(NULL),
          _lockPending(false),
          _lockPendingParallelWriter(false)
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

    bool LockState::hasAnyWriteLock() const { 
        return _threadState == 'w' || _threadState == 'W';
    }

    bool LockState::isLocked( const StringData& ns ) {
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns, db);
        
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

    void LockState::lockedStart( char newState ) {
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

    BSONObj LockState::reportState() {
        BSONObjBuilder b;
        reportState( b );
        return b.obj();
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
            b.append("^", buf);
        }
        if( _nestableCount ) {
            string s = "?";
            if( _whichNestable == Lock::local ) 
                s = "^local";
            else if( _whichNestable == Lock::admin ) 
                s = "^admin";
            b.append(s, kind(_nestableCount));
        }
        if( _otherCount ) { 
            WrapperForRWLock *k = _otherLock;
            if( k ) {
                string s = "^";
                s += k->name();
                b.append(s, kind(_otherCount));
            }
        }
        BSONObj o = b.obj();
        if( !o.isEmpty() ) 
            res.append("locks", o);
        res.append( "waitingForLock" , _lockPending );
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

    void LockState::lockedOther( int type ) {
        fassert( 16231 , _otherCount == 0 );
        _otherCount = type;
    }

    void LockState::lockedOther( const StringData& other , int type , WrapperForRWLock* lock ) {
        fassert( 16170 , _otherCount == 0 );
        _otherName = other.toString();
        _otherCount = type;
        _otherLock = lock;
    }

    void LockState::unlockedOther() {
        // we leave _otherName and _otherLock set as
        // _otherLock exists to cache a pointer
        _otherCount = 0;
    }

    LockStat* LockState::getRelevantLockStat() {
        if ( _whichNestable )
            return Lock::nestableLockStat( _whichNestable );

        if ( _otherCount && _otherLock )
            return &_otherLock->stats;
        
        if ( isRW() ) 
            return Lock::globalLockStat();

        return 0;
    }


    Acquiring::Acquiring( Lock::ScopedLock* lock,  LockState& ls )
        : _lock( lock ), _ls( ls ){
        _ls._lockPending = true;
    }

    Acquiring::~Acquiring() {
        _ls._lockPending = false;
        LockStat* stat = _ls.getRelevantLockStat();
        if ( stat && _lock )
            stat->recordAcquireTimeMicros( _ls.threadState(), _lock->acquireFinished( stat ) );
    }
    
    AcquiringParallelWriter::AcquiringParallelWriter( LockState& ls )
        : _ls( ls ) {
        _ls._lockPendingParallelWriter = true;
    }
    
    AcquiringParallelWriter::~AcquiringParallelWriter() {
        _ls._lockPendingParallelWriter = false;
    }

}
