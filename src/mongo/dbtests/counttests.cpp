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

#include <boost/thread/thread.hpp>

#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/commands/count.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context_impl.h"

#include "mongo/dbtests/dbtests.h"

namespace CountTests {

    class Base {
    public:
        Base() : lk(_txn.lockState(), nsToDatabaseSubstring(ns()), newlm::MODE_X),
                 _wunit(&_txn), _context(&_txn, ns()) {
            _database = _context.db();
            _collection = _database->getCollection( &_txn, ns() );
            if ( _collection ) {
                _database->dropCollection( &_txn, ns() );
            }
            _collection = _database->createCollection( &_txn, ns() );

            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                uassertStatusOK( _database->dropCollection( &_txn, ns() ) );
                _wunit.commit();
            }
            catch ( ... ) {
                FAIL( "Exception while cleaning up collection" );
            }
        }
    protected:
        static const char *ns() {
            return "unittests.counttests";
        }
        void addIndex( const BSONObj &key ) {
            Helpers::ensureIndex(&_txn,
                                 _collection,
                                 key,
                                 /*unique=*/ false,
                                 /*name=*/ key.firstElementFieldName());
        }
        void insert( const char *s ) {
            insert( fromjson( s ) );
        }
        void insert( const BSONObj &o ) {
            if ( o["_id"].eoo() ) {
                BSONObjBuilder b;
                OID oid;
                oid.init();
                b.appendOID( "_id", &oid );
                b.appendElements( o );
                _collection->insertDocument( &_txn, b.obj(), false );
            }
            else {
                _collection->insertDocument( &_txn, o, false );
            }
        }
        static BSONObj countCommand( const BSONObj &query ) {
            return BSON( "query" << query );
        }

        OperationContextImpl _txn;

    private:
        Lock::DBLock lk;
        WriteUnitOfWork _wunit;

        Client::Context _context;

        Database* _database;
        Collection* _collection;

    };
    
    class Basic : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            BSONObj cmd = fromjson( "{\"query\":{}}" );
            string err;
            int errCode;
            ASSERT_EQUALS( 1, runCount( &_txn, ns(), cmd, err, errCode ) );
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
            int errCode;
            ASSERT_EQUALS( 2, runCount( &_txn, ns(), cmd, err, errCode ) );
        }
    };
    
    class Fields : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"c\":\"d\"}" );
            BSONObj cmd = fromjson( "{\"query\":{},\"fields\":{\"a\":1}}" );
            string err;
            int errCode;
            ASSERT_EQUALS( 2, runCount( &_txn, ns(), cmd, err, errCode ) );
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
            int errCode;
            ASSERT_EQUALS( 1, runCount( &_txn, ns(), cmd, err, errCode ) );
        }
    };
    
    class IndexedRegex : public Base {
    public:
        void run() {
            insert( "{\"a\":\"b\"}" );
            insert( "{\"a\":\"c\"}" );
            BSONObj cmd = fromjson( "{\"query\":{\"a\":/^b/}}" );
            string err;
            int errCode;
            ASSERT_EQUALS( 1, runCount( &_txn, ns(), cmd, err, errCode ) );
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
        }
    } myall;
    
} // namespace CountTests
