/**
*    Copyright (C) 2009 10gen Inc.
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
*/

#include "pch.h"
#include "curop.h"
#include "database.h"

namespace mongo {

    // todo : move more here

    CurOp::CurOp( Client * client , CurOp * wrapped ) : 
        _client(client), 
        _wrapped(wrapped) 
    {
        _lockType = 0;
        if ( _wrapped )
            _client->_curOp = this;
        _start = _checkpoint = 0;
        _active = false;
        _reset();
        _op = 0;
        // These addresses should never be written to again.  The zeroes are
        // placed here as a precaution because currentOp may be accessed
        // without the db mutex.
        memset(_ns, 0, sizeof(_ns));
    }

    void CurOp::_reset() {
        _suppressFromCurop = false;
        _command = false;
        _lockType = 0;
        _dbprofile = 0;
        _end = 0;
        _waitingForLock = false;
        _message = "";
        _progressMeter.finished();
        _killed = false;
        _numYields = 0;
        _expectedLatencyMs = 0;
    }

    void CurOp::reset() {
        _reset();
        _start = _checkpoint = 0;
        _opNum = _nextOpNum++;
        _ns[0] = 0;
        _debug.reset();
        _query.reset();
        _active = true; // this should be last for ui clarity
    }

    void CurOp::reset( const HostAndPort& remote, int op ) {
        reset();
        if( _remote != remote ) {
            // todo : _remote is not thread safe yet is used as such!
            _remote = remote;
        }
        _op = op;
    }
        
    ProgressMeter& CurOp::setMessage( const char * msg , unsigned long long progressMeterTotal , int secondsBetween ) {
        if ( progressMeterTotal ) {
            if ( _progressMeter.isActive() ) {
                cout << "about to assert, old _message: " << _message << " new message:" << msg << endl;
                verify( ! _progressMeter.isActive() );
            }
            _progressMeter.reset( progressMeterTotal , secondsBetween );
        }
        else {
            _progressMeter.finished();
        }
        _message = msg;
        return _progressMeter;
    }


    BSONObj CurOp::info() {
        if( ! cc().getAuthenticationInfo()->isAuthorized("admin") ) {
            BSONObjBuilder b;
            b.append("err", "unauthorized");
            return b.obj();
        }
        return infoNoauth();
    }

    CurOp::~CurOp() {
        if ( _wrapped ) {
            scoped_lock bl(Client::clientsMutex);
            _client->_curOp = _wrapped;
        }
        _client = 0;
    }

    void CurOp::enter( Client::Context * context ) {
        ensureStarted();
        setNS( context->ns() );
        _dbprofile = context->_db ? context->_db->profile : 0;
    }
    
    void CurOp::leave( Client::Context * context ) {
        unsigned long long now = curTimeMicros64();
        Top::global.record( _ns , _op , _lockType , now - _checkpoint , _command );
        _checkpoint = now;
    }

    BSONObj CurOp::infoNoauth() {
        BSONObjBuilder b;
        b.append("opid", _opNum);
        bool a = _active && _start;
        b.append("active", a);
        if ( _lockType ) {
            char str[2];
            str[0] = _lockType;
            str[1] = 0;
            b.append("lockType" , str);
        }
        b.append("waitingForLock" , _waitingForLock );

        if( a ) {
            b.append("secs_running", elapsedSeconds() );
        }

        b.append( "op" , opToString( _op ) );

        b.append("ns", _ns);

        _query.append( b , "query" );

        if( !_remote.empty() ) {
            b.append("client", _remote.toString());
        }

        if ( _client ) {
            b.append( "desc" , _client->desc() );
            if ( _client->_threadId.size() ) 
                b.append( "threadId" , _client->_threadId );
            if ( _client->_connectionId )
                b.appendNumber( "connectionId" , _client->_connectionId );
            _client->_ls.reportState(b);
        }
        
        if ( ! _message.empty() ) {
            if ( _progressMeter.isActive() ) {
                StringBuilder buf;
                buf << _message.toString() << " " << _progressMeter.toString();
                b.append( "msg" , buf.str() );
                BSONObjBuilder sub( b.subobjStart( "progress" ) );
                sub.appendNumber( "done" , (long long)_progressMeter.done() );
                sub.appendNumber( "total" , (long long)_progressMeter.total() );
                sub.done();
            }
            else {
                b.append( "msg" , _message.toString() );
            }
        }

        if( killed() ) 
            b.append("killed", true);
        
        b.append( "numYields" , _numYields );

        return b.obj();
    }

    void KillCurrentOp::checkForInterrupt( bool heedMutex ) {
        Client& c = cc();
        if ( heedMutex && Lock::somethingWriteLocked() && c.hasWrittenThisPass() )
            return;
        if( _globalKill )
            uasserted(11600,"interrupted at shutdown");
        if( c.curop()->killed() ) {
            uasserted(11601,"operation was interrupted");
        }
        if( c.sometimes(1024) ) {
            AbstractMessagingPort *p = cc().port();
            if( p ) 
                p->assertStillConnected();
        }
    }
    
    const char * KillCurrentOp::checkForInterruptNoAssert() {
        Client& c = cc();
        if( _globalKill )
            return "interrupted at shutdown";
        if( c.curop()->killed() )
            return "interrupted";
        if( c.sometimes(1024) ) {
            try { 
                AbstractMessagingPort *p = cc().port();
                if( p ) 
                    p->assertStillConnected();
            }
            catch(...) { 
                log() << "no longer connected to client";
                return "no longer connected to client";
            }
        }
        return "";
    }


    AtomicUInt CurOp::_nextOpNum;

}
