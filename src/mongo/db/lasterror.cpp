// lasterror.cpp

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/lasterror.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"

namespace mongo {

    using std::endl;

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

    bool LastError::appendSelf( BSONObjBuilder &b , bool blankErr ) {

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
        if ( !upsertedId.isEmpty() ) {
            b.append( upsertedId[kUpsertedFieldName] );
        }
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
        return le;
    }

    LastError * LastErrorHolder::get( bool create ) {
        LastError *ret = _get( create );
        if ( ret && !ret->disabled )
            return ret;
        return 0;
    }

    LastError * LastErrorHolder::getSafe() {
        LastError * le = get(false);
        if ( ! le ) {
            error() << " no LastError!" << std::endl;
            verify( le );
        }
        return le;
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
        }
    }

    LastError * LastErrorHolder::startRequest( Message& m , LastError * le ) {
        verify( le );
        prepareErrForNewRequest( m, le );
        return le;
    }

} // namespace mongo
