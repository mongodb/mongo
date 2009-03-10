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
            update( ns(), emptyObj, fromjson( "{$set:{'_id':4}}" ) );
        }
    };
    
    class ModNonmodMix : public Fail {
        void doIt() {
            update( ns(), emptyObj, fromjson( "{$set:{a:4},z:3}" ) );
        }        
    };
    
    class InvalidMod : public Fail {
        void doIt() {
            update( ns(), emptyObj, fromjson( "{$awk:{a:4}}" ) );
        }        
    };

    class ModNotFirst : public Fail {
        void doIt() {
            update( ns(), emptyObj, fromjson( "{z:3,$set:{a:4}}" ) );
        }        
    };
    
    class ModDuplicateFieldSpec : public Fail {
        void doIt() {
            update( ns(), emptyObj, fromjson( "{$set:{a:4},$inc:{a:1}}" ) );
        }        
    };

    class ModNonNumber : public Fail {
        void doIt() {
            update( ns(), emptyObj, fromjson( "{$set:{a:'d'}}" ) );
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
        void run() {
            const char *ns = "querytests.GetMore";
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            insert( ns, BSON( "a" << 3 ) );
            auto_ptr< DBClientCursor > cursor = client().query( ns, emptyObj, 2 );
            long long cursorId = cursor->getCursorId();
            cursor->decouple();
            cursor.reset();
            cursor = client().getMore( ns, cursorId );
            ASSERT( cursor->more() );
            ASSERT_EQUALS( 3, cursor->next().getIntField( "a" ) );
        }
    };
    
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
            add< ModNonNumber >();
            add< BoundedKey >();
            add< GetMore >();
        }
    };
    
} // namespace QueryTests

UnitTest::TestPtr queryTests() {
    return UnitTest::createSuite< QueryTests::All >();
}
