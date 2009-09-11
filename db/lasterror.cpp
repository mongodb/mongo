// lasterror.cpp

#include "stdafx.h"

#include "../util/unittest.h"

#include "lasterror.h"
#include "jsobj.h"

namespace mongo {

    LastError LastError::noError;
    LastErrorHolder lastError;
    
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
        _id.reset( id );
    }
    
    LastError * LastErrorHolder::get(){
        int id = _id.get();
        if ( id == 0 )
            return _tl.get();
        
        LastErrorIDMap::iterator i = _ids.find( id );
        if ( i == _ids.end() )
            return 0;
        
        LastErrorStatus & status = i->second;
        status.first = time(0);
        return status.second;
    }

    void LastErrorHolder::remove( int id ){
        LastErrorIDMap::iterator i = _ids.find( id );
        if ( i == _ids.end() )
            return;
        
        delete i->second.second;
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
        
        LastErrorStatus & status = _ids[id];
        status.first = time(0);
        status.second = le;
    }

    struct LastErrorHolderTest : public UnitTest {
    public:
        
        void test( int i ){
            _tl.reset( i );
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
        
        ThreadLocalInt _tl;
    } lastErrorHolderTest;

} // namespace mongo
