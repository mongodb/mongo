// lasterror.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include "mongo/db/lasterror.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/net/message.h"

namespace mongo {

    LastError LastError::noError;
    LastErrorHolder lastError;

    bool isShell = false;
    void setLastError(int code , const char *msg) {
        LastError *le = lastError.get();
        if ( le == 0 ) {
            /* might be intentional (non-user thread) */
            DEV {
                static unsigned n;
                if( ++n < 4 && !isShell ) log() << "dev: lastError==0 won't report:" << msg << endl;
            }
        }
        else if ( le->disabled ) {
            log() << "lastError disabled, can't report: " << code << ":" << msg << endl;
        }
        else {
            le->raiseError(code, msg);
        }
    }

    void LastError::appendSelfStatus( BSONObjBuilder &b ) {
        if ( writebackId.isSet() ) {
            b.append( "writeback" , writebackId );
            b.append( "writebackSince", writebackSince );
            b.append( "instanceIdent" , prettyHostName() ); // this can be any unique string
        }
    }

    bool LastError::appendSelf( BSONObjBuilder &b , bool blankErr ) {

        appendSelfStatus( b );

        if ( !valid ) {
            if ( blankErr )
                b.appendNull( "err" );
            b.append( "n", 0 );
            return false;
        }

        if ( msg.empty() ) {
            if ( blankErr ) {
                b.appendNull( "err" );
            }
        }
        else {
            b.append( "err", msg );
        }

        if ( code )
            b.append( "code" , code );
        if ( updatedExisting != NotUpdate )
            b.appendBool( "updatedExisting", updatedExisting == True );
        if ( upsertedId.isSet() )
            b.append( "upserted" , upsertedId );

        b.appendNumber( "n", nObjects );

        return ! msg.empty();
    }

    LastErrorHolder::~LastErrorHolder() {
    }


    LastError * LastErrorHolder::disableForCommand() {
        LastError *le = _get();
        uassert(13649, "no operation yet", le);
        le->disabled = true;
        le->nPrev--; // caller is a command that shouldn't count as an operation
        le->writebackSince--; // same as above
        return le;
    }

    LastError * LastErrorHolder::get( bool create ) {
        LastError *ret = _get( create );
        if ( ret && !ret->disabled )
            return ret;
        return 0;
    }

    LastError * LastErrorHolder::_get( bool create ) {
        LastError * le = _tl.get();
        if ( ! le && create ) {
            le = new LastError();
            _tl.reset( le );
        }
        return le;
    }

    void LastErrorHolder::release() {
        _tl.release();
    }

    /** ok to call more than once. */
    void LastErrorHolder::initThread() {
        if( ! _tl.get() ) 
            _tl.reset( new LastError() );
    }

    void LastErrorHolder::reset( LastError * le ) {
        _tl.reset( le );
    }

    void prepareErrForNewRequest( Message &m, LastError * err ) {
        // a killCursors message shouldn't affect last error
        verify( err );
        if ( m.operation() == dbKillCursors ) {
            err->disabled = true;
        }
        else {
            err->disabled = false;
            err->nPrev++;
            err->writebackSince++;
        }
    }

    LastError * LastErrorHolder::startRequest( Message& m , LastError * le ) {
        verify( le );
        prepareErrForNewRequest( m, le );
        return le;
    }

} // namespace mongo
