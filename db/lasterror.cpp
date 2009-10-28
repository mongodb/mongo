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

#include "stdafx.h"

#include "../util/unittest.h"
#include "../util/message.h"


#include "lasterror.h"
#include "jsobj.h"

namespace mongo {

    LastError LastError::noError;
    LastErrorHolder lastError;
    boost::mutex LastErrorHolder::_idsmutex;

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
        if ( updatedExisting != NotUpdate )
            b.appendBool( "updatedExisting", updatedExisting == True );
        b.append( "n", nObjects );
    }

    void LastErrorHolder::setID( int id ){
        _id.set( id );
    }
    
    int LastErrorHolder::getID(){
        return _id.get();
    }

    LastError * LastErrorHolder::get( bool create ){
        int id = _id.get();
        if ( id == 0 )
            return _tl.get();
        
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
        boostlock lock(_idsmutex);
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
    
    void LastErrorHolder::reset( LastError * le ){
        int id = _id.get();
        if ( id == 0 ){
            _tl.reset( le );
            return;
        }
        
        Status & status = _ids[id];
        status.time = time(0);
        status.lerr = le;
    }
    
    void LastErrorHolder::startRequest( Message& m ) {
        int id = m.data->id & 0xFFFF0000;
        setID( id );
        LastError * le = get( true);
        le->nPrev++;
    }

    void LastErrorHolder::startRequest( Message& m , LastError * connectionOwned ) {
        if ( !connectionOwned->overridenById ) {
            connectionOwned->nPrev++;
            return;
        }
        startRequest(m);
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
