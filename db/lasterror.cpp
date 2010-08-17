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

#include "pch.h"

#include "../util/unittest.h"
#include "../util/message.h"


#include "lasterror.h"
#include "jsobj.h"

namespace mongo {

    LastError LastError::noError;
    LastErrorHolder lastError;
    mongo::mutex LastErrorHolder::_idsmutex("LastErrorHolder");

    bool isShell = false;
    void raiseError(int code , const char *msg) {
        LastError *le = lastError.get();
        if ( le == 0 ) {
            /* might be intentional (non-user thread) */    
            DEV {
                static unsigned n;
                if( ++n < 4 && !isShell ) log() << "warning dev: lastError==0 won't report:" << msg << endl;
            }
        } else if ( le->disabled ) {
            log() << "lastError disabled, can't report: " << code << ":" << msg << endl;
        } else {
            le->raiseError(code, msg);
        }
    }

    void LastError::appendSelf( BSONObjBuilder &b ) {
        if ( !valid ) {
            b.appendNull( "err" );
            b.append( "n", 0 );
            return;
        }
        if ( msg.empty() )
            b.appendNull( "err" );
        else
            b.append( "err", msg );
        if ( code )
            b.append( "code" , code );
        if ( updatedExisting != NotUpdate )
            b.appendBool( "updatedExisting", updatedExisting == True );
        if ( upsertedId.isSet() )
            b.append( "upserted" , upsertedId );
        if ( writebackId.isSet() )
            b.append( "writeback" , writebackId );
        b.appendNumber( "n", nObjects );
    }

    LastErrorHolder::~LastErrorHolder(){
        for ( IDMap::iterator i = _ids.begin(); i != _ids.end(); ++i ){
            delete i->second.lerr;
            i->second.lerr = 0;
        }
        _ids.clear();
    }


    void LastErrorHolder::setID( int id ){
        _id.set( id );
    }
    
    int LastErrorHolder::getID(){
        return _id.get();
    }

    LastError * LastErrorHolder::disableForCommand() {
        LastError *le = _get();
        assert( le );
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
    
    LastError * LastErrorHolder::_get( bool create ){
        int id = _id.get();
        if ( id == 0 ){
            LastError * le = _tl.get();
            if ( ! le && create ){
                le = new LastError();
                _tl.reset( le );
            }
            return le;
        }

        scoped_lock lock(_idsmutex);        
        map<int,Status>::iterator i = _ids.find( id );
        if ( i == _ids.end() ){
            if ( ! create )
                return 0;
            
            LastError * le = new LastError();
            Status s;
            s.time = time(0);
            s.lerr = le;
            _ids[id] = s;
            return le;
        }
        
        Status &status = i->second;
        status.time = time(0);
        return status.lerr;
    }

    void LastErrorHolder::remove( int id ){
        scoped_lock lock(_idsmutex);
        map<int,Status>::iterator i = _ids.find( id );
        if ( i == _ids.end() )
            return;
        
        delete i->second.lerr;
        _ids.erase( i );
    }

    void LastErrorHolder::release(){
        int id = _id.get();
        if ( id == 0 ){
            _tl.release();
            return;
        }
        
        remove( id );
    }

    /** ok to call more than once. */
    void LastErrorHolder::initThread() { 
        if( _tl.get() ) return;
        assert( _id.get() == 0 );
        _tl.reset( new LastError() );
    }
    
    void LastErrorHolder::reset( LastError * le ){
        int id = _id.get();
        if ( id == 0 ){
            _tl.reset( le );
            return;
        }

        scoped_lock lock(_idsmutex);
        Status & status = _ids[id];
        status.time = time(0);
        status.lerr = le;
    }
    
    void prepareErrForNewRequest( Message &m, LastError * err ) {
        // a killCursors message shouldn't affect last error
        if ( m.operation() == dbKillCursors ) {
            err->disabled = true;
        } else {
            err->disabled = false;
            err->nPrev++;
        }        
    }
    
    LastError * LastErrorHolder::startRequest( Message& m , int clientId ) {

        if ( clientId == 0 )
            clientId = m.header()->id & 0xFFFF0000;
        setID( clientId );

        LastError * le = _get( true );
        prepareErrForNewRequest( m, le );
        return le;
    }

    void LastErrorHolder::startRequest( Message& m , LastError * connectionOwned ) {
        if ( !connectionOwned->overridenById ) {
            prepareErrForNewRequest( m, connectionOwned );
            return;
        }
        startRequest(m);
    }

    void LastErrorHolder::disconnect( int clientId ){
        if ( clientId )
            remove(clientId);
    }

    struct LastErrorHolderTest : public UnitTest {
    public:
        
        void test( int i ){
            _tl.set( i );
            assert( _tl.get() == i );
        }
        
        void tlmaptest(){
            test( 1 );
            test( 12123123 );
            test( -123123 );
            test( numeric_limits<int>::min() );
            test( numeric_limits<int>::max() );
        }
        
        void run(){
            tlmaptest();

            LastError * a = new LastError();
            LastError * b = new LastError();
            
            LastErrorHolder holder;
            holder.reset( a );
            assert( a == holder.get() );
            holder.setID( 1 );
            assert( 0 == holder.get() );
            holder.reset( b );
            assert( b == holder.get() );
            holder.setID( 0 );
            assert( a == holder.get() );
            
            holder.remove( 1 );
        }
        
        ThreadLocalValue<int> _tl;
    } lastErrorHolderTest;

} // namespace mongo
