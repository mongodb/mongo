// pdfiletests.cpp : query.{h,cpp} unit tests.
//

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
 */

#include "../db/query.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"

#include "dbtests.h"

namespace QueryTests {

    class Base {
    public:
        Base() {
            dblock lk;
            setClient( ns() );
            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                auto_ptr< Cursor > c = theDataFileMgr.findAll( ns() );
                vector< DiskLoc > toDelete;
                for(; c->ok(); c->advance() )
                    toDelete.push_back( c->currLoc() );
                for( vector< DiskLoc >::iterator i = toDelete.begin(); i != toDelete.end(); ++i )
                    theDataFileMgr.deleteRecord( ns(), i->rec(), *i, false );
            } catch ( ... ) {
                FAIL( "Exception while cleaning up records" );
            }
        }
    protected:
        static const char *ns() {
            return "unittest.querytests";
        }
        static void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", "index" );
            b.append( "ns", ns() );
            b.append( "key", key );
            BSONObj o = b.done();
            stringstream indexNs;
            indexNs << ns() << ".system.indexes";
            theDataFileMgr.insert( indexNs.str().c_str(), o.objdata(), o.objsize() );
        }
        static void insert( const char *s ) {
            insert( fromjson( s ) );
        }
        static void insert( const BSONObj &o ) {
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize() );
        }
    };
    
    class CountBasic : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            BSONObj cmd = fromjson( "{\"query\":{}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }
    };
    
    class CountQuery : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"b\",\"x\":\"y\"}" );
            insert( "{\"a\":\"c\"}" );
            BSONObj cmd = fromjson( "{\"query\":{\"a\":\"b\"}}" );
            string err;
            ASSERT_EQUALS( 2, runCount( ns(), cmd, err ) );
        }        
    };
    
    class CountFields : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"c\":\"d\"}" );
            BSONObj cmd = fromjson( "{\"query\":{},\"fields\":{\"a\":1}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }        
    };

    class CountQueryFields : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"c\"}" );
            insert( "{\"d\":\"e\"}" );
            BSONObj cmd = fromjson( "{\"query\":{\"a\":\"b\"},\"fields\":{\"a\":1}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }        
    };

    class CountIndexedRegex : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"c\"}" );            
            BSONObj cmd = fromjson( "{\"query\":{\"a\":/^b/}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }
    };
    
    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {
            mongo::lastError.reset( new LastError() );
        }
        ~ClientBase() {
            mongo::lastError.release();
        }
    protected:
        static void insert( const char *ns, BSONObj o ) {
            client_.insert( ns, o );
        }
        static void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            client_.update( ns, Query( q ), o, upsert );
        }
        static bool error() {
            return !client_.getPrevError().getField( "err" ).isNull();
        }
        DBDirectClient &client() const { return client_; }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient ClientBase::client_;
    
    class Fail : public ClientBase {
    public:
        virtual ~Fail() {}
        void run() {
            prep();
            ASSERT( !error() );
            doIt();
            ASSERT( error() );
        }
    protected:
        const char *ns() { return "QueryTests_Fail"; }
        virtual void prep() {
            insert( ns(), fromjson( "{a:1}" ) );   
        }
        virtual void doIt() = 0;
    };
    
    class ModId : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$set:{'_id':4}}" ) );
        }
    };
    
    class ModNonmodMix : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$set:{a:4},z:3}" ) );
        }        
    };
    
    class InvalidMod : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$awk:{a:4}}" ) );
        }        
    };

    class ModNotFirst : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{z:3,$set:{a:4}}" ) );
        }        
    };
    
    class ModDuplicateFieldSpec : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$set:{a:4},$inc:{a:1}}" ) );
        }        
    };

    class IncNonNumber : public Fail {
        void doIt() {
            update( ns(), BSONObj(), fromjson( "{$inc:{a:'d'}}" ) );
        }        
    };
    
    class IncTargetNonNumber : public Fail {
        void doIt() {
            insert( ns(), BSON( "a" << "a" ) );
            update( ns(), BSON( "a" << "a" ), fromjson( "{$inc:{a:1}}" ) );
        }        
    };
    
    class BoundedKey : public ClientBase {
    public:
        void run() {
            const char *ns = "querytests.BoundedKey";
            insert( ns, BSON( "a" << 1 ) );
            BSONObjBuilder a;
            a.appendMaxKey( "$lt" );
            BSONObj limit = a.done();
            ASSERT( !client().findOne( ns, QUERY( "a" << limit ) ).isEmpty() );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( !client().findOne( ns, QUERY( "a" << limit ).hint( BSON( "a" << 1 ) ) ).isEmpty() );
        }
    };
    
    class GetMore : public ClientBase {
    public:
        ~GetMore() {
            client().dropCollection( "querytests.GetMore" );
        }
        void run() {
            const char *ns = "querytests.GetMore";
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            insert( ns, BSON( "a" << 3 ) );
            auto_ptr< DBClientCursor > cursor = client().query( ns, BSONObj(), 2 );
            long long cursorId = cursor->getCursorId();
            cursor->decouple();
            cursor.reset();
            cursor = client().getMore( ns, cursorId );
            ASSERT( cursor->more() );
            ASSERT_EQUALS( 3, cursor->next().getIntField( "a" ) );
        }
    };
    
    class ReturnOneOfManyAndTail : public ClientBase {
    public:
        ~ReturnOneOfManyAndTail() {
            client().dropCollection( "querytests.ReturnOneOfManyAndTail" );
        }
        void run() {
            const char *ns = "querytests.ReturnOneOfManyAndTail";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "a" << GT << 0 ).hint( BSON( "$natural" << 1 ) ), 1, 0, 0, Option_CursorTailable );
            // If only one result requested, a cursor is not saved.
            ASSERT_EQUALS( 0, c->getCursorId() );
            ASSERT( c->more() );
            ASSERT_EQUALS( 1, c->next().getIntField( "a" ) );
        }
    };
    
    class TailNotAtEnd : public ClientBase {
    public:
        ~TailNotAtEnd() {
            client().dropCollection( "querytests.TailNotAtEnd" );
        }
        void run() {
            const char *ns = "querytests.TailNotAtEnd";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable );
            ASSERT( 0 != c->getCursorId() );
            while( c->more() )
                c->next();
            ASSERT( 0 != c->getCursorId() );
            insert( ns, BSON( "a" << 3 ) );
            insert( ns, BSON( "a" << 4 ) );
            insert( ns, BSON( "a" << 5 ) );
            insert( ns, BSON( "a" << 6 ) );
            ASSERT( c->more() );
            ASSERT_EQUALS( 3, c->next().getIntField( "a" ) );
        }
    };
    
    class EmptyTail : public ClientBase {
    public:
        ~EmptyTail() {
            client().dropCollection( "querytests.EmptyTail" );
        }
        void run() {
            const char *ns = "querytests.EmptyTail";
            ASSERT_EQUALS( 0, client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable )->getCursorId() );
            insert( ns, BSON( "a" << 0 ) );
            ASSERT( 0 != client().query( ns, QUERY( "a" << 1 ).hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable )->getCursorId() );
        }
    };
    
    class TailableDelete : public ClientBase {
    public:
        ~TailableDelete() {
            client().dropCollection( "querytests.TailableDelete" );
        }
        void run() {
            const char *ns = "querytests.TailableDelete";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            client().remove( ns, QUERY( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            ASSERT( !c->more() );
            ASSERT_EQUALS( 0, c->getCursorId() );
        }
    };

    class TailableInsertDelete : public ClientBase {
    public:
        ~TailableInsertDelete() {
            client().dropCollection( "querytests.TailableInsertDelete" );
        }
        void run() {
            const char *ns = "querytests.TailableInsertDelete";
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, Option_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            insert( ns, BSON( "a" << 2 ) );
            client().remove( ns, QUERY( "a" << 1 ) );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "a" ) );
            ASSERT( !c->more() );
        }
    };    
    
    class OplogReplayMode : public ClientBase {
    public:
        ~OplogReplayMode() {
            client().dropCollection( "querytests.OplogReplayMode" );            
        }
        void run() {
            const char *ns = "querytests.OplogReplayMode";
            insert( ns, BSON( "a" << 3 ) );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "a" << GT << 1 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, Option_OplogReplay );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "a" ) );
            ASSERT( !c->more() );
        }
    };
    
    class SetBase : public ClientBase {
    public:
        ~SetBase() {
            client().dropCollection( ns() );
        }
    protected:
        const char *ns() { return "querytests.SetBase"; }
    };
    
    class SetNum : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << 1 ) );
            client().update( ns(), BSON( "a" << 1 ), BSON( "$set" << BSON( "a" << 4 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << 4 ) ).isEmpty() );
        }
    };
    
    class SetString : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "b" ) );
            client().update( ns(), BSON( "a" << "b" ), BSON( "$set" << BSON( "a" << "c" ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << "c" ) ).isEmpty() );
        }
    };
    
    class SetStringDifferentLength : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "b" ) );
            client().update( ns(), BSON( "a" << "b" ), BSON( "$set" << BSON( "a" << "cd" ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << "cd" ) ).isEmpty() );            
        }
    };
    
    class SetStringToNum : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "b" ) );
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "a" << 5 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << 5 ) ).isEmpty() );
        }        
    };
    
    class SetStringToNumInPlace : public SetBase {
    public:
        void run() {
            client().insert( ns(), BSON( "a" << "bcd" ) );
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "a" << 5.0 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a" << 5.0 ) ).isEmpty() );            
        }
    };
    
    class ModDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{a:{b:4}}" ) );
            client().update( ns(), BSONObj(), BSON( "$inc" << BSON( "a.b" << 10 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a.b" << 14 ) ).isEmpty() );            
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "a.b" << 55 ) ) );
            ASSERT( !client().findOne( ns(), BSON( "a.b" << 55 ) ).isEmpty() );                        
        }
    };

    class SetInPlaceDotted : public SetBase {
    public:
        void run() {
            client().insert( ns(), fromjson( "{a:{b:'cdef'}}" ) );
            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "a.b" << "llll" ) ) );
            out() << "one: " << client().findOne( ns(), BSONObj() ) << endl; 
            ASSERT( !client().findOne( ns(), BSON( "a.b" << "llll" ) ).isEmpty() );                        
        }
    };

//    class SetRecreateDotted : public SetBase {
//    public:
//        void run() {
//            client().insert( ns(), fromjson( "{a:{b:'cdef'}}" ) );
//            client().update( ns(), BSONObj(), BSON( "$set" << BSON( "a.b" << "lllll" ) ) );
//            ASSERT( !client().findOne( ns(), BSON( "a.b" << "lllll" ) ).isEmpty() );                        
//        }
//    };
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< CountBasic >();
            add< CountQuery >();
            add< CountFields >();
            add< CountQueryFields >();
            add< CountIndexedRegex >();
            add< ModId >();
            add< ModNonmodMix >();
            add< InvalidMod >();
            add< ModNotFirst >();
            add< ModDuplicateFieldSpec >();
            add< IncNonNumber >();
            add< IncTargetNonNumber >();
            add< BoundedKey >();
            add< GetMore >();
            add< ReturnOneOfManyAndTail >();
            add< TailNotAtEnd >();
            add< EmptyTail >();
            add< TailableDelete >();
            add< TailableInsertDelete >();
            add< OplogReplayMode >();
            add< SetNum >();
            add< SetString >();
            add< SetStringDifferentLength >();
            add< SetStringToNum >();
            add< SetStringToNumInPlace >();
            add< ModDotted >();
            add< SetInPlaceDotted >();
//            add< SetRecreateDotted >();
        }
    };
    
} // namespace QueryTests

UnitTest::TestPtr queryTests() {
    return UnitTest::createSuite< QueryTests::All >();
}
