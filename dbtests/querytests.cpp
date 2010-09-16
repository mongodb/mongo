// querytests.cpp : query.{h,cpp} unit tests.
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

#include "pch.h"
#include "../db/query.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"

#include "../util/timer.h"

#include "dbtests.h"

namespace mongo {
    extern int __findingStartInitialTimeout;
}

namespace QueryTests {

    class Base {
        dblock lk;
        Client::Context _context;
    public:
        Base() : _context( ns() ){
            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns() );
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
            return "unittests.querytests";
        }
        static void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", key.firstElement().fieldName() );
            b.append( "ns", ns() );
            b.append( "key", key );
            BSONObj o = b.done();
            stringstream indexNs;
            indexNs << "unittests.system.indexes";
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
            ASSERT_EQUALS( 2, runCount( ns(), cmd, err ) );
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
    
    class FindOne : public Base {
    public:
        void run() {
            addIndex( BSON( "b" << 1 ) );
            addIndex( BSON( "c" << 1 ) );
            insert( BSON( "b" << 2 << "_id" << 0 ) );
            insert( BSON( "c" << 3 << "_id" << 1 ) );
            BSONObj ret;
            ASSERT( Helpers::findOne( ns(), fromjson( "{$or:[{b:2},{c:3}]}" ), ret, true ) );
            ASSERT_EQUALS( string( "b" ), ret.firstElement().fieldName() );
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

        static DBDirectClient client_;
    };
    DBDirectClient ClientBase::client_;

    class BoundedKey : public ClientBase {
    public:
        void run() {
            const char *ns = "unittests.querytests.BoundedKey";
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
            client().dropCollection( "unittests.querytests.GetMore" );
        }
        void run() {
            const char *ns = "unittests.querytests.GetMore";
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

    class PositiveLimit : public ClientBase {
    public:
        const char* ns;
        PositiveLimit() : ns("unittests.querytests.PositiveLimit") {}
        ~PositiveLimit() {
            client().dropCollection( ns );
        }

        void testLimit(int limit){
            ASSERT_EQUALS(client().query( ns, BSONObj(), limit )->itcount(), limit);
        }
        void run() {
            for(int i=0; i<1000; i++)
                insert( ns, BSON( GENOID << "i" << i ) );

            ASSERT_EQUALS( client().query(ns, BSONObj(),    1 )->itcount(), 1);
            ASSERT_EQUALS( client().query(ns, BSONObj(),   10 )->itcount(), 10);
            ASSERT_EQUALS( client().query(ns, BSONObj(),  101 )->itcount(), 101);
            ASSERT_EQUALS( client().query(ns, BSONObj(),  999 )->itcount(), 999);
            ASSERT_EQUALS( client().query(ns, BSONObj(), 1000 )->itcount(), 1000);
            ASSERT_EQUALS( client().query(ns, BSONObj(), 1001 )->itcount(), 1000);
            ASSERT_EQUALS( client().query(ns, BSONObj(),    0 )->itcount(), 1000);
        }
    };

    class ReturnOneOfManyAndTail : public ClientBase {
    public:
        ~ReturnOneOfManyAndTail() {
            client().dropCollection( "unittests.querytests.ReturnOneOfManyAndTail" );
        }
        void run() {
            const char *ns = "unittests.querytests.ReturnOneOfManyAndTail";
            client().createCollection( ns, 0, true );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "a" << GT << 0 ).hint( BSON( "$natural" << 1 ) ), 1, 0, 0, QueryOption_CursorTailable );
            // If only one result requested, a cursor is not saved.
            ASSERT_EQUALS( 0, c->getCursorId() );
            ASSERT( c->more() );
            ASSERT_EQUALS( 1, c->next().getIntField( "a" ) );
        }
    };

    class TailNotAtEnd : public ClientBase {
    public:
        ~TailNotAtEnd() {
            client().dropCollection( "unittests.querytests.TailNotAtEnd" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailNotAtEnd";
            client().createCollection( ns, 0, true );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
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
            client().dropCollection( "unittests.querytests.EmptyTail" );
        }
        void run() {
            const char *ns = "unittests.querytests.EmptyTail";
            client().createCollection( ns, 0, true );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            ASSERT_EQUALS( 0, c->getCursorId() );
            ASSERT( c->isDead() );
            insert( ns, BSON( "a" << 0 ) );
            c = client().query( ns, QUERY( "a" << 1 ).hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            ASSERT( 0 != c->getCursorId() );
            ASSERT( !c->isDead() );            
        }
    };

    class TailableDelete : public ClientBase {
    public:
        ~TailableDelete() {
            client().dropCollection( "unittests.querytests.TailableDelete" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailableDelete";
            client().createCollection( ns, 0, true, 2 );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            insert( ns, BSON( "a" << 2 ) );
            insert( ns, BSON( "a" << 3 ) );
            ASSERT( !c->more() );
            ASSERT_EQUALS( 0, c->getCursorId() );
        }
    };

    class TailableInsertDelete : public ClientBase {
    public:
        ~TailableInsertDelete() {
            client().dropCollection( "unittests.querytests.TailableInsertDelete" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailableInsertDelete";
            client().createCollection( ns, 0, true );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
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

    class TailCappedOnly : public ClientBase {
    public:
        ~TailCappedOnly() {
            client().dropCollection( "unittest.querytests.TailCappedOnly" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailCappedOnly";
            client().insert( ns, BSONObj() );
            auto_ptr< DBClientCursor > c = client().query( ns, BSONObj(), 0, 0, 0, QueryOption_CursorTailable );
            ASSERT( c->isDead() );
            ASSERT( !client().getLastError().empty() );
        }
    };
    
    class TailableQueryOnId : public ClientBase {
    public:
        ~TailableQueryOnId() {
            client().dropCollection( "unittests.querytests.TailableQueryOnId" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailableQueryOnId";
            BSONObj info;
            client().runCommand( "unittests", BSON( "create" << "querytests.TailableQueryOnId" << "capped" << true << "autoIndexId" << true ), info );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c1 = client().query( ns, QUERY( "a" << GT << -1 ), 0, 0, 0, QueryOption_CursorTailable );
            OID id;
            id.init("000000000000000000000000");
            auto_ptr< DBClientCursor > c2 = client().query( ns, QUERY( "_id" << GT << id ), 0, 0, 0, QueryOption_CursorTailable );
            c1->next();
            c1->next();
            ASSERT( !c1->more() );
            c2->next();
            c2->next();
            ASSERT( !c2->more() );
            insert( ns, BSON( "a" << 2 ) );
            ASSERT( c1->more() );
            ASSERT_EQUALS( 2, c1->next().getIntField( "a" ) );
            ASSERT( !c1->more() );
            ASSERT( c2->more() );
            ASSERT_EQUALS( 2, c2->next().getIntField( "a" ) );  // SERVER-645
            ASSERT( !c2->more() );
            ASSERT( !c2->isDead() );
        }
    };

    class OplogReplayMode : public ClientBase {
    public:
        ~OplogReplayMode() {
            client().dropCollection( "unittests.querytests.OplogReplayMode" );
        }
        void run() {
            const char *ns = "unittests.querytests.OplogReplayMode";
            insert( ns, BSON( "ts" << 3 ) );
            insert( ns, BSON( "ts" << 0 ) );
            insert( ns, BSON( "ts" << 1 ) );
            insert( ns, BSON( "ts" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "ts" << GT << 1 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, QueryOption_OplogReplay );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "ts" ) );
            ASSERT( !c->more() );
        }
    };

    class BasicCount : public ClientBase {
    public:
        ~BasicCount() {
            client().dropCollection( "unittests.querytests.BasicCount" );
        }
        void run() {
            const char *ns = "unittests.querytests.BasicCount";
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            count( 0 );
            insert( ns, BSON( "a" << 3 ) );
            count( 0 );
            insert( ns, BSON( "a" << 4 ) );
            count( 1 );
            insert( ns, BSON( "a" << 5 ) );
            count( 1 );
            insert( ns, BSON( "a" << 4 ) );
            count( 2 );
        }
    private:
        void count( unsigned long long c ) const {
            ASSERT_EQUALS( c, client().count( "unittests.querytests.BasicCount", BSON( "a" << 4 ) ) );
        }
    };

    class ArrayId : public ClientBase {
    public:
        ~ArrayId() {
            client().dropCollection( "unittests.querytests.ArrayId" );
        }
        void run() {
            const char *ns = "unittests.querytests.ArrayId";
            client().ensureIndex( ns, BSON( "_id" << 1 ) );
            ASSERT( !error() );
            client().insert( ns, fromjson( "{'_id':[1,2]}" ) );
            ASSERT( error() );
        }
    };

    class UnderscoreNs : public ClientBase {
    public:
        ~UnderscoreNs() {
            client().dropCollection( "unittests.querytests._UnderscoreNs" );
        }
        void run() {
            ASSERT( !error() );
            const char *ns = "unittests.querytests._UnderscoreNs";
            ASSERT( client().findOne( ns, "{}" ).isEmpty() );
            client().insert( ns, BSON( "a" << 1 ) );
            ASSERT_EQUALS( 1, client().findOne( ns, "{}" ).getIntField( "a" ) );
            ASSERT( !error() );
        }
    };

    class EmptyFieldSpec : public ClientBase {
    public:
        ~EmptyFieldSpec() {
            client().dropCollection( "unittests.querytests.EmptyFieldSpec" );
        }
        void run() {
            const char *ns = "unittests.querytests.EmptyFieldSpec";
            client().insert( ns, BSON( "a" << 1 ) );
            ASSERT( !client().findOne( ns, "" ).isEmpty() );
            BSONObj empty;
            ASSERT( !client().findOne( ns, "", &empty ).isEmpty() );
        }
    };

    class MultiNe : public ClientBase {
    public:
        ~MultiNe() {
            client().dropCollection( "unittests.querytests.Ne" );
        }
        void run() {
            const char *ns = "unittests.querytests.Ne";
            client().insert( ns, fromjson( "{a:[1,2]}" ) );
            ASSERT( client().findOne( ns, fromjson( "{a:{$ne:1}}" ) ).isEmpty() );
            BSONObj spec = fromjson( "{a:{$ne:1,$ne:2}}" );
            ASSERT( client().findOne( ns, spec ).isEmpty() );
        }
    };

    class EmbeddedNe : public ClientBase {
    public:
        ~EmbeddedNe() {
            client().dropCollection( "unittests.querytests.NestedNe" );
        }
        void run() {
            const char *ns = "unittests.querytests.NestedNe";
            client().insert( ns, fromjson( "{a:[{b:1},{b:2}]}" ) );
            ASSERT( client().findOne( ns, fromjson( "{'a.b':{$ne:1}}" ) ).isEmpty() );
        }
    };

    class EmbeddedNumericTypes : public ClientBase {
    public:
        ~EmbeddedNumericTypes() {
            client().dropCollection( "unittests.querytests.NumericEmbedded" );
        }
        void run() {
            const char *ns = "unittests.querytests.NumericEmbedded";
            client().insert( ns, BSON( "a" << BSON ( "b" << 1 ) ) );
            ASSERT( ! client().findOne( ns, BSON( "a" << BSON ( "b" << 1.0 ) ) ).isEmpty() );
            client().ensureIndex( ns , BSON( "a" << 1 ) );
            ASSERT( ! client().findOne( ns, BSON( "a" << BSON ( "b" << 1.0 ) ) ).isEmpty() );
        }
    };

    class AutoResetIndexCache : public ClientBase {
    public:
        ~AutoResetIndexCache() {
            client().dropCollection( "unittests.querytests.AutoResetIndexCache" );
        }
        static const char *ns() { return "unittests.querytests.AutoResetIndexCache"; }
        static const char *idxNs() { return "unittests.system.indexes"; }
        void index() const { ASSERT( !client().findOne( idxNs(), BSON( "name" << NE << "_id_" ) ).isEmpty() ); }
        void noIndex() const { ASSERT( client().findOne( idxNs(), BSON( "name" << NE << "_id_" ) ).isEmpty() ); }
        void checkIndex() {
            client().ensureIndex( ns(), BSON( "a" << 1 ) );
            index();
        }
        void run() {
            client().dropDatabase( "unittests" );
            noIndex();
            checkIndex();
            client().dropCollection( ns() );
            noIndex();
            checkIndex();
            client().dropDatabase( "unittests" );
            noIndex();
            checkIndex();
        }
    };

    class UniqueIndex : public ClientBase {
    public:
        ~UniqueIndex() {
            client().dropCollection( "unittests.querytests.UniqueIndex" );
        }
        void run() {
            const char *ns = "unittests.querytests.UniqueIndex";
            client().ensureIndex( ns, BSON( "a" << 1 ), true );
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            ASSERT_EQUALS( 1U, client().count( ns, BSONObj() ) );
            client().dropCollection( ns );
            client().ensureIndex( ns, BSON( "b" << 1 ), true );
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSONObj() ) );
        }
    };

    class UniqueIndexPreexistingData : public ClientBase {
    public:
        ~UniqueIndexPreexistingData() {
            client().dropCollection( "unittests.querytests.UniqueIndexPreexistingData" );
        }
        void run() {
            const char *ns = "unittests.querytests.UniqueIndexPreexistingData";
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            client().ensureIndex( ns, BSON( "a" << 1 ), true );
            ASSERT_EQUALS( 0U, client().count( "unittests.system.indexes", BSON( "ns" << ns << "name" << NE << "_id_" ) ) );
        }
    };

    class SubobjectInArray : public ClientBase {
    public:
        ~SubobjectInArray() {
            client().dropCollection( "unittests.querytests.SubobjectInArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.SubobjectInArray";
            client().insert( ns, fromjson( "{a:[{b:{c:1}}]}" ) );
            ASSERT( !client().findOne( ns, BSON( "a.b.c" << 1 ) ).isEmpty() );
            ASSERT( !client().findOne( ns, fromjson( "{'a.c':null}" ) ).isEmpty() );
        }
    };

    class Size : public ClientBase {
    public:
        ~Size() {
            client().dropCollection( "unittests.querytests.Size" );
        }
        void run() {
            const char *ns = "unittests.querytests.Size";
            client().insert( ns, fromjson( "{a:[1,2,3]}" ) );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( client().query( ns, QUERY( "a" << mongo::SIZE << 3 ).hint( BSON( "a" << 1 ) ) )->more() );
        }
    };

    class FullArray : public ClientBase {
    public:
        ~FullArray() {
            client().dropCollection( "unittests.querytests.IndexedArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.IndexedArray";
            client().insert( ns, fromjson( "{a:[1,2,3]}" ) );
            ASSERT( client().query( ns, Query( "{a:[1,2,3]}" ) )->more() );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( client().query( ns, Query( "{a:{$in:[1,[1,2,3]]}}" ).hint( BSON( "a" << 1 ) ) )->more() ); 
            ASSERT( client().query( ns, Query( "{a:[1,2,3]}" ).hint( BSON( "a" << 1 ) ) )->more() ); // SERVER-146 
        }
    };

    class InsideArray : public ClientBase {
    public:
        ~InsideArray() {
            client().dropCollection( "unittests.querytests.InsideArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.InsideArray";
            client().insert( ns, fromjson( "{a:[[1],2]}" ) );
            check( "$natural" );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            check( "a" ); // SERVER-146 
        }
    private:
        void check( const string &hintField ) {
            const char *ns = "unittests.querytests.InsideArray";
            ASSERT( client().query( ns, Query( "{a:[[1],2]}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{a:[1]}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{a:2}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( !client().query( ns, Query( "{a:1}" ).hint( BSON( hintField << 1 ) ) )->more() );
        }
    };

    class IndexInsideArrayCorrect : public ClientBase {
    public:
        ~IndexInsideArrayCorrect() {
            client().dropCollection( "unittests.querytests.IndexInsideArrayCorrect" );
        }
        void run() {
            const char *ns = "unittests.querytests.IndexInsideArrayCorrect";
            client().insert( ns, fromjson( "{'_id':1,a:[1]}" ) );
            client().insert( ns, fromjson( "{'_id':2,a:[[1]]}" ) );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT_EQUALS( 1, client().query( ns, Query( "{a:[1]}" ).hint( BSON( "a" << 1 ) ) )->next().getIntField( "_id" ) );
        }
    };

    class SubobjArr : public ClientBase {
    public:
        ~SubobjArr() {
            client().dropCollection( "unittests.querytests.SubobjArr" );
        }
        void run() {
            const char *ns = "unittests.querytests.SubobjArr";
            client().insert( ns, fromjson( "{a:[{b:[1]}]}" ) );
            check( "$natural" );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            check( "a" );
        }
    private:
        void check( const string &hintField ) {
            const char *ns = "unittests.querytests.SubobjArr";
            ASSERT( client().query( ns, Query( "{'a.b':1}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{'a.b':[1]}" ).hint( BSON( hintField << 1 ) ) )->more() );
        }
    };

    class MinMax : public ClientBase {
    public:
        MinMax() : ns( "unittests.querytests.MinMax" ) {}
        ~MinMax() {
            client().dropCollection( "unittests.querytests.MinMax" );
        }
        void run() {
            client().ensureIndex( ns, BSON( "a" << 1 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 1 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 1 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 2 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 2 << "b" << 2 ) );

            ASSERT_EQUALS( 4, count( client().query( ns, BSONObj() ) ) );
            BSONObj hints[] = { BSONObj(), BSON( "a" << 1 << "b" << 1 ) };
            for( int i = 0; i < 2; ++i ) {
                check( 0, 0, 3, 3, 4, hints[ i ] );
                check( 1, 1, 2, 2, 3, hints[ i ] );
                check( 1, 2, 2, 2, 2, hints[ i ] );
                check( 1, 2, 2, 1, 1, hints[ i ] );

                auto_ptr< DBClientCursor > c = query( 1, 2, 2, 2, hints[ i ] );
                BSONObj obj = c->next();
                ASSERT_EQUALS( 1, obj.getIntField( "a" ) );
                ASSERT_EQUALS( 2, obj.getIntField( "b" ) );
                obj = c->next();
                ASSERT_EQUALS( 2, obj.getIntField( "a" ) );
                ASSERT_EQUALS( 1, obj.getIntField( "b" ) );
                ASSERT( !c->more() );
            }
        }
    private:
        auto_ptr< DBClientCursor > query( int minA, int minB, int maxA, int maxB, const BSONObj &hint ) {
            Query q;
            q = q.minKey( BSON( "a" << minA << "b" << minB ) ).maxKey( BSON( "a" << maxA << "b" << maxB ) );
            if ( !hint.isEmpty() )
                q.hint( hint );
            return client().query( ns, q );
        }
        void check( int minA, int minB, int maxA, int maxB, int expectedCount, const BSONObj &hint = empty_ ) {
            ASSERT_EQUALS( expectedCount, count( query( minA, minB, maxA, maxB, hint ) ) );
        }
        int count( auto_ptr< DBClientCursor > c ) {
            int ret = 0;
            while( c->more() ) {
                ++ret;
                c->next();
            }
            return ret;
        }
        const char *ns;
        static BSONObj empty_;
    };
    BSONObj MinMax::empty_;

    class DirectLocking : public ClientBase {
    public:
        void run() {
            dblock lk;
            Client::Context ctx( "unittests.DirectLocking" );
            client().remove( "a.b", BSONObj() );
            ASSERT_EQUALS( "unittests", cc().database()->name );
        }
        const char *ns;
    };

    class FastCountIn : public ClientBase {
    public:
        ~FastCountIn() {
            client().dropCollection( "unittests.querytests.FastCountIn" );
        }
        void run() {
            const char *ns = "unittests.querytests.FastCountIn";
            client().insert( ns, BSON( "i" << "a" ) );
            client().ensureIndex( ns, BSON( "i" << 1 ) );
            ASSERT_EQUALS( 1U, client().count( ns, fromjson( "{i:{$in:['a']}}" ) ) );
        }
    };

    class EmbeddedArray : public ClientBase {
    public:
        ~EmbeddedArray() {
            client().dropCollection( "unittests.querytests.EmbeddedArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.EmbeddedArray";
            client().insert( ns, fromjson( "{foo:{bar:['spam']}}" ) );
            client().insert( ns, fromjson( "{foo:{bar:['spam','eggs']}}" ) );
            client().insert( ns, fromjson( "{bar:['spam']}" ) );
            client().insert( ns, fromjson( "{bar:['spam','eggs']}" ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSON( "bar" << "spam" ) ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSON( "foo.bar" << "spam" ) ) );
        }
    };

    class DifferentNumbers : public ClientBase {
    public:
        ~DifferentNumbers(){
            client().dropCollection( "unittests.querytests.DifferentNumbers" );
        }
        void t( const char * ns ){
            auto_ptr< DBClientCursor > cursor = client().query( ns, Query().sort( "7" ) );
            while ( cursor->more() ){
                BSONObj o = cursor->next();
                assert( o.valid() );
                //cout << " foo " << o << endl;
            }

        }
        void run() {
            const char *ns = "unittests.querytests.DifferentNumbers";
            { BSONObjBuilder b; b.append( "7" , (int)4 ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.append( "7" , (long long)2 ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.appendNull( "7" ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.append( "7" , "b" ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.appendNull( "8" ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.append( "7" , (double)3.7 ); client().insert( ns , b.obj() ); }

            t(ns);
            client().ensureIndex( ns , BSON( "7" << 1 ) );
            t(ns);
        }
    };
    
    class CollectionBase : public ClientBase {
    public:
    
        CollectionBase( string leaf ){
            _ns = "unittests.querytests.";
            _ns += leaf;
            client().dropCollection( ns() );
        }
        
        virtual ~CollectionBase(){
            client().dropCollection( ns() );
        }
        
        int count(){
            return (int) client().count( ns() );
        }

        const char * ns(){
            return _ns.c_str();
        }
        
    private:
        string _ns;
    };

    class SymbolStringSame : public CollectionBase {
    public:
        SymbolStringSame() : CollectionBase( "symbolstringsame" ){}

        void run(){
            { BSONObjBuilder b; b.appendSymbol( "x" , "eliot" ); b.append( "z" , 17 ); client().insert( ns() , b.obj() ); }
            ASSERT_EQUALS( 17 , client().findOne( ns() , BSONObj() )["z"].number() );
            {
                BSONObjBuilder b;
                b.appendSymbol( "x" , "eliot" );
                ASSERT_EQUALS( 17 , client().findOne( ns() , b.obj() )["z"].number() );
            }
            ASSERT_EQUALS( 17 , client().findOne( ns() , BSON( "x" << "eliot" ) )["z"].number() );
            client().ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 17 , client().findOne( ns() , BSON( "x" << "eliot" ) )["z"].number() );
        }
    };

    class TailableCappedRaceCondition : public CollectionBase {
    public:
        
        TailableCappedRaceCondition() : CollectionBase( "tailablecappedrace" ){
            client().dropCollection( ns() );
            _n = 0;
        }
        void run(){
            string err;

            writelock lk("");            
            Client::Context ctx( "unittests" );

            ASSERT( userCreateNS( ns() , fromjson( "{ capped : true , size : 2000 }" ) , err , false ) );
            for ( int i=0; i<100; i++ ){
                insertNext();
                ASSERT( count() < 45 );
            }
            
            int a = count();
            
            auto_ptr< DBClientCursor > c = client().query( ns() , QUERY( "i" << GT << 0 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, QueryOption_CursorTailable );
            int n=0;
            while ( c->more() ){
                BSONObj z = c->next();
                n++;
            }
            
            ASSERT_EQUALS( a , n );

            insertNext();
            ASSERT( c->more() );

            for ( int i=0; i<50; i++ ){
                insertNext();
            }

            while ( c->more() ){ c->next(); }
            ASSERT( c->isDead() );
        }
        
        void insertNext(){
            insert( ns() , BSON( "i" << _n++ ) );
        }

        int _n;
    };

    class HelperTest : public CollectionBase {
    public:
        
        HelperTest() : CollectionBase( "helpertest" ){
        }

        void run(){
            writelock lk("");
            Client::Context ctx( "unittests" );
            
            for ( int i=0; i<50; i++ ){
                insert( ns() , BSON( "_id" << i << "x" << i * 2 ) );
            }

            ASSERT_EQUALS( 50 , count() );
            
            BSONObj res;
            ASSERT( Helpers::findOne( ns() , BSON( "_id" << 20 ) , res , true ) );
            ASSERT_EQUALS( 40 , res["x"].numberInt() );
            
            ASSERT( Helpers::findById( cc(), ns() , BSON( "_id" << 20 ) , res ) );
            ASSERT_EQUALS( 40 , res["x"].numberInt() );

            ASSERT( ! Helpers::findById( cc(), ns() , BSON( "_id" << 200 ) , res ) );

            unsigned long long slow , fast;
            
            int n = 10000;
            {
                Timer t;
                for ( int i=0; i<n; i++ ){
                    ASSERT( Helpers::findOne( ns() , BSON( "_id" << 20 ) , res , true ) );
                }
                slow = t.micros();
            }
            {
                Timer t;
                for ( int i=0; i<n; i++ ){
                    ASSERT( Helpers::findById( cc(), ns() , BSON( "_id" << 20 ) , res ) );
                }
                fast = t.micros();
            }
            
            cout << "HelperTest  slow:" << slow << " fast:" << fast << endl;
            
            {
                auto_ptr<CursorIterator> i = Helpers::find( ns() );
                int n = 0;
                while ( i->hasNext() ){
                    BSONObj o = i->next();
                    n++;
                }
                ASSERT_EQUALS( 50 , n );

                i = Helpers::find( ns() , BSON( "_id" << 20 ) );
                n = 0;
                while ( i->hasNext() ){
                    BSONObj o = i->next();
                    n++;
                }
                ASSERT_EQUALS( 1 , n );
            }
            
        }
    };

    class HelperByIdTest : public CollectionBase {
    public:
        
        HelperByIdTest() : CollectionBase( "helpertestbyid" ){
        }

        void run(){
            writelock lk("");
            Client::Context ctx( "unittests" );

            for ( int i=0; i<1000; i++ ){
                insert( ns() , BSON( "_id" << i << "x" << i * 2 ) );
            }
            for ( int i=0; i<1000; i+=2 ){
                client_.remove( ns() , BSON( "_id" << i ) );
            }

            BSONObj res;            
            for ( int i=0; i<1000; i++ ){
                bool found = Helpers::findById( cc(), ns() , BSON( "_id" << i ) , res );
                ASSERT_EQUALS( i % 2 , int(found) );
            }

        }
    };

    class ClientCursorTest : public CollectionBase{
        ClientCursorTest() : CollectionBase( "clientcursortest" ){
        }

        void run(){
            writelock lk("");
            Client::Context ctx( "unittests" );
            
            for ( int i=0; i<1000; i++ ){
                insert( ns() , BSON( "_id" << i << "x" << i * 2 ) );
            }

            
        }
    };

    class FindingStart : public CollectionBase {
    public:
        FindingStart() : CollectionBase( "findingstart" ), _old( __findingStartInitialTimeout ) {
            __findingStartInitialTimeout = 0;
        }
        ~FindingStart() {
            __findingStartInitialTimeout = _old;
        }
        
        void run() {
            BSONObj info;
            ASSERT( client().runCommand( "unittests", BSON( "create" << "querytests.findingstart" << "capped" << true << "size" << 1000 << "$nExtents" << 5 << "autoIndexId" << false ), info ) );
            
            int i = 0;
            for( int oldCount = -1;
                count() != oldCount;
                oldCount = count(), client().insert( ns(), BSON( "ts" << i++ ) ) );

            for( int k = 0; k < 5; ++k ) {
                client().insert( ns(), BSON( "ts" << i++ ) );
                int min = client().query( ns(), Query().sort( BSON( "$natural" << 1 ) ) )->next()[ "ts" ].numberInt();            
                for( int j = -1; j < i; ++j ) {
                    auto_ptr< DBClientCursor > c = client().query( ns(), QUERY( "ts" << GTE << j ), 0, 0, 0, QueryOption_OplogReplay );
                    ASSERT( c->more() );
                    BSONObj next = c->next();
                    ASSERT( !next[ "ts" ].eoo() );
                    ASSERT_EQUALS( ( j > min ? j : min ), next[ "ts" ].numberInt() );
                }
            }
        }
        
    private:
        int _old;
    };

    class FindingStartPartiallyFull : public CollectionBase {
    public:
        FindingStartPartiallyFull() : CollectionBase( "findingstart" ), _old( __findingStartInitialTimeout ) {
            __findingStartInitialTimeout = 0;
        }
        ~FindingStartPartiallyFull() {
            __findingStartInitialTimeout = _old;
        }
        
        void run() {
            BSONObj info;
            ASSERT( client().runCommand( "unittests", BSON( "create" << "querytests.findingstart" << "capped" << true << "size" << 10000 << "$nExtents" << 5 << "autoIndexId" << false ), info ) );
            
            int i = 0;
            for( ; i < 150; client().insert( ns(), BSON( "ts" << i++ ) ) );
            
            for( int k = 0; k < 5; ++k ) {
                client().insert( ns(), BSON( "ts" << i++ ) );
                int min = client().query( ns(), Query().sort( BSON( "$natural" << 1 ) ) )->next()[ "ts" ].numberInt();            
                for( int j = -1; j < i; ++j ) {
                    auto_ptr< DBClientCursor > c = client().query( ns(), QUERY( "ts" << GTE << j ), 0, 0, 0, QueryOption_OplogReplay );
                    ASSERT( c->more() );
                    BSONObj next = c->next();
                    ASSERT( !next[ "ts" ].eoo() );
                    ASSERT_EQUALS( ( j > min ? j : min ), next[ "ts" ].numberInt() );
                }
            }
        }
        
    private:
        int _old;
    };
        
    
    class WhatsMyUri : public CollectionBase {
    public:
        WhatsMyUri() : CollectionBase( "whatsmyuri" ) {}
        void run() {
            BSONObj result;
            client().runCommand( "admin", BSON( "whatsmyuri" << 1 ), result );
            ASSERT_EQUALS( unknownAddress.toString(), result[ "you" ].str() );
        }
    };
    
    namespace parsedtests {
        class basic1 {
        public:
            void _test( const BSONObj& in ){
                ParsedQuery q( "a.b" , 5 , 6 , 9 , in , BSONObj() );
                ASSERT_EQUALS( BSON( "x" << 5 ) , q.getFilter() );
            }
            void run(){
                _test( BSON( "x" << 5 ) );
                _test( BSON( "query" << BSON( "x" << 5 ) ) );
                _test( BSON( "$query" << BSON( "x" << 5 ) ) );

                {
                    ParsedQuery q( "a.b" , 5 , 6 , 9 , BSON( "x" << 5 ) , BSONObj() );
                    ASSERT_EQUALS( 6 , q.getNumToReturn() );
                    ASSERT( q.wantMore() );
                }
                {
                    ParsedQuery q( "a.b" , 5 , -6 , 9 , BSON( "x" << 5 ) , BSONObj() );
                    ASSERT_EQUALS( 6 , q.getNumToReturn() );
                    ASSERT( ! q.wantMore() );
                }
            }
        };
    };

    namespace queryobjecttests {
        class names1 {
        public:
            void run(){
                ASSERT_EQUALS( BSON( "x" << 1 ) , QUERY( "query" << BSON( "x" << 1 ) ).getFilter() );
                ASSERT_EQUALS( BSON( "x" << 1 ) , QUERY( "$query" << BSON( "x" << 1 ) ).getFilter() );
            }
            
        };
    }

    class OrderingTest {
    public:
        void run(){
            {
                Ordering o = Ordering::make( BSON( "a" << 1 << "b" << -1 << "c" << 1 ) );
                ASSERT_EQUALS( 1 , o.get(0) );
                ASSERT_EQUALS( -1 , o.get(1) );
                ASSERT_EQUALS( 1 , o.get(2) );
                
                ASSERT( ! o.descending( 1 ) );
                ASSERT( o.descending( 1 << 1 ) );
                ASSERT( ! o.descending( 1 << 2 ) );
            }

            {
                Ordering o = Ordering::make( BSON( "a.d" << 1 << "a" << 1 << "e" << -1 ) );
                ASSERT_EQUALS( 1 , o.get(0) );
                ASSERT_EQUALS( 1 , o.get(1) );
                ASSERT_EQUALS( -1 , o.get(2) );
                
                ASSERT( ! o.descending( 1 ) );
                ASSERT( ! o.descending( 1 << 1 ) );
                ASSERT(  o.descending( 1 << 2 ) );
            }

        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query" ) {
        }

        void setupTests(){
            add< CountBasic >();
            add< CountQuery >();
            add< CountFields >();
            add< CountQueryFields >();
            add< CountIndexedRegex >();
            add< FindOne >();
            add< BoundedKey >();
            add< GetMore >();
            add< PositiveLimit >();
            add< ReturnOneOfManyAndTail >();
            add< TailNotAtEnd >();
            add< EmptyTail >();
            add< TailableDelete >();
            add< TailableInsertDelete >();
            add< TailCappedOnly >();
            add< TailableQueryOnId >();
            add< OplogReplayMode >();
            add< ArrayId >();
            add< UnderscoreNs >();
            add< EmptyFieldSpec >();
            add< MultiNe >();
            add< EmbeddedNe >();
            add< EmbeddedNumericTypes >();
            add< AutoResetIndexCache >();
            add< UniqueIndex >();
            add< UniqueIndexPreexistingData >();
            add< SubobjectInArray >();
            add< Size >();
            add< FullArray >();
            add< InsideArray >();
            add< IndexInsideArrayCorrect >();
            add< SubobjArr >();
            add< MinMax >();
            add< DirectLocking >();
            add< FastCountIn >();
            add< EmbeddedArray >();
            add< DifferentNumbers >();
            add< SymbolStringSame >();
            add< TailableCappedRaceCondition >();
            add< HelperTest >();
            add< HelperByIdTest >();
            add< FindingStart >();
            add< FindingStartPartiallyFull >();
            add< WhatsMyUri >();
            
            add< parsedtests::basic1 >();
            
            add< queryobjecttests::names1 >();

            add< OrderingTest >();
        }
    } myall;
    
} // namespace QueryTests

