// counttests.cpp : count.{h,cpp} unit tests.

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

#include "../db/ops/count.h"

#include "../db/cursor.h"
#include "../db/pdfile.h"
#include "mongo/db/db.h"
#include "mongo/db/json.h"

#include "dbtests.h"

namespace CountTests {

    class Base {
        Lock::DBWrite lk;
        Client::Context _context;
    public:
        Base() : lk(ns()), _context( ns() ) {
            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns() );
                vector<DiskLoc> toDelete;
                for(; c->ok(); c->advance() )
                    toDelete.push_back( c->currLoc() );
                for( vector<DiskLoc>::iterator i = toDelete.begin(); i != toDelete.end(); ++i )
                    theDataFileMgr.deleteRecord( ns(), i->rec(), *i, false );
                DBDirectClient cl;
                cl.dropIndexes( ns() );
            }
            catch ( ... ) {
                FAIL( "Exception while cleaning up collection" );
            }
        }
    protected:
        static const char *ns() {
            return "unittests.counttests";
        }
        static void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", key.firstElementFieldName() );
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
        static BSONObj countCommand( const BSONObj &query ) {
            return BSON( "query" << query );
        }
    };
    
    class Basic : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            BSONObj cmd = fromjson( "{\"query\":{}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }
    };
    
    class Query : public Base {
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
    
    class Fields : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"c\":\"d\"}" );
            BSONObj cmd = fromjson( "{\"query\":{},\"fields\":{\"a\":1}}" );
            string err;
            ASSERT_EQUALS( 2, runCount( ns(), cmd, err ) );
        }
    };
    
    class QueryFields : public Base {
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
    
    class IndexedRegex : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"c\"}" );
            BSONObj cmd = fromjson( "{\"query\":{\"a\":/^b/}}" );
            string err;
            ASSERT_EQUALS( 1, runCount( ns(), cmd, err ) );
        }
    };

    /** Set a value or await an expected value. */
    class PendingValue {
    public:
        PendingValue( int initialValue ) :
            _value( initialValue ),
            _mutex( "CountTests::PendingValue::_mutex" ) {
        }
        void set( int newValue ) {
            scoped_lock lk( _mutex );
            _value = newValue;
            _condition.notify_all();
        }
        void await( int expectedValue ) const {
            scoped_lock lk( _mutex );
            while( _value != expectedValue ) {
                _condition.wait( lk.boost() );
            }
        }
    private:
        int _value;
        mutable mongo::mutex _mutex;
        mutable boost::condition _condition;
    };

    /** A writer client will be registered for the lifetime of an object of this class. */
    class WriterClientScope {
    public:
        WriterClientScope() :
            _state( Initial ),
            _dummyWriter( boost::bind( &WriterClientScope::runDummyWriter, this ) ) {
            _state.await( Ready );
        }
        ~WriterClientScope() {
            // Terminate the writer thread even on exception.
            _state.set( Finished );
            DESTRUCTOR_GUARD( _dummyWriter.join() );
        }
    private:
        enum State {
            Initial,
            Ready,
            Finished
        };
        void runDummyWriter() {
            Client::initThread( "dummy writer" );
            // Register a write lock request.
            cc().curop()->waitingForLock( 'W' );
            _state.set( Ready );
            _state.await( Finished );
            cc().shutdown();
        }
        PendingValue _state;
        boost::thread _dummyWriter;
    };
    
    /**
     * The runCount() function yields deterministically with sufficient cursor iteration and a
     * mutually exclusive thread awaiting its mutex.  SERVER-5428
     */
    class Yield : public Base {
    public:
        void run() {
            // Insert enough documents that counting them will exceed the iteration threshold
            // to trigger a yield.
            for( int i = 0; i < 1000; ++i ) {
                insert( BSON( "a" << 1 ) );
            }
            
            // Call runCount() under a read lock.
            dbtemprelease release;
            Client::ReadContext ctx( ns() );

            int numYieldsBeforeCount = numYields();
            
            string err;
            ASSERT_EQUALS( 1000, runCount( ns(), countCommand( BSON( "a" << 1 ) ), err ) );
            ASSERT_EQUALS( "", err );

            int numYieldsAfterCount = numYields();
            int numYieldsDuringCount = numYieldsAfterCount - numYieldsBeforeCount;

            // The runCount() function yieled.
            ASSERT_NOT_EQUALS( 0, numYieldsDuringCount );
            ASSERT( 0 < numYieldsDuringCount );
        }
    private:
        int numYields() const {
            return cc().curop()->infoNoauth()[ "numYields" ].Int();
        }
        // A writer client is registered while the test runs, causing runCount() to yield.
        WriterClientScope _writer;
    };
    
    class All : public Suite {
    public:
        All() : Suite( "count" ) {
        }
        
        void setupTests() {
            add<Basic>();
            add<Query>();
            add<Fields>();
            add<QueryFields>();
            add<IndexedRegex>();
            add<Yield>();
        }
    } myall;
    
} // namespace CountTests
