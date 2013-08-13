// repltests.cpp : Unit tests for replication
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/db.h"
#include "mongo/db/index_update.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/query_plan.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/repl/finding_start_cursor.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/ops/update.h"

#include "mongo/dbtests/dbtests.h"

namespace ReplTests {

    BSONObj f( const char *s ) {
        return fromjson( s );
    }

    class Base {
        Lock::GlobalWrite lk;
        Client::Context _context;
    public:
        Base() : _context( ns() ) {
            oldRepl();
            cmdLine._replSet = "";
            cmdLine.oplogSize = 5 * 1024 * 1024;
            replSettings.master = true;
            createOplog();
            ensureHaveIdIndex( ns(), false );
        }
        ~Base() {
            try {
                replSettings.master = false;
                deleteAll( ns() );
                deleteAll( cllNS() );
            }
            catch ( ... ) {
                FAIL( "Exception while cleaning up test" );
            }
        }
    protected:
        static const char *ns() {
            return "unittests.repltests";
        }
        static const char *cllNS() {
            return "local.oplog.$main";
        }
        DBDirectClient *client() const { return &client_; }
        BSONObj one( const BSONObj &query = BSONObj() ) const {
            return client()->findOne( ns(), query );
        }
        void checkOne( const BSONObj &o ) const {
            check( o, one( o ) );
        }
        void checkAll( const BSONObj &o ) const {
            auto_ptr< DBClientCursor > c = client()->query( ns(), o );
            verify( c->more() );
            while( c->more() ) {
                check( o, c->next() );
            }
        }
        void check( const BSONObj &expected, const BSONObj &got ) const {
            if ( expected.woCompare( got ) ) {
                out() << "expected: " << expected.toString()
                      << ", got: " << got.toString() << endl;
            }
            ASSERT_EQUALS( expected , got );
        }
        BSONObj oneOp() const {
            return client()->findOne( cllNS(), BSONObj() );
        }
        int count() const {
            int count = 0;
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns() );
            for(; c->ok(); c->advance(), ++count ) {
//                cout << "obj: " << c->current().toString() << endl;
            }
            return count;
        }
        static int opCount() {
            Lock::GlobalWrite lk;
            Client::Context ctx( cllNS() );
            int count = 0;
            for( boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( cllNS() ); c->ok(); c->advance() )
                ++count;
            return count;
        }
        static void applyAllOperations() {
            Lock::GlobalWrite lk;
            vector< BSONObj > ops;
            {
                Client::Context ctx( cllNS() );
                for( boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( cllNS() ); c->ok(); c->advance() )
                    ops.push_back( c->current() );
            }
            {
                Client::Context ctx( ns() );
                BSONObjBuilder b;
                b.append("host", "localhost");
                b.appendTimestamp("syncedTo", 0);
                ReplSource a(b.obj());
                for( vector< BSONObj >::iterator i = ops.begin(); i != ops.end(); ++i ) {
                    if ( 0 ) {
                        mongo::unittest::log() << "op: " << *i << endl;
                    }
                    a.applyOperation( *i );
                }
            }
        }
        static void printAll( const char *ns ) {
            Lock::GlobalWrite lk;
            Client::Context ctx( ns );
            boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns );
            vector< DiskLoc > toDelete;
            out() << "all for " << ns << endl;
            for(; c->ok(); c->advance() ) {
                out() << c->current().toString() << endl;
            }
        }
        // These deletes don't get logged.
        static void deleteAll( const char *ns ) {
            Lock::GlobalWrite lk;
            Client::Context ctx( ns );
            boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns );
            vector< DiskLoc > toDelete;
            for(; c->ok(); c->advance() ) {
                toDelete.push_back( c->currLoc() );
            }
            for( vector< DiskLoc >::iterator i = toDelete.begin(); i != toDelete.end(); ++i ) {
                theDataFileMgr.deleteRecord( ns, i->rec(), *i, true );
            }
        }
        static void insert( const BSONObj &o, bool god = false ) {
            Lock::GlobalWrite lk;
            Client::Context ctx( ns() );
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize(), false, god );
        }
        static BSONObj wid( const char *json ) {
            class BSONObjBuilder b;
            OID id;
            id.init();
            b.appendOID( "_id", &id );
            b.appendElements( fromjson( json ) );
            return b.obj();
        }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient Base::client_;

    class LogBasic : public Base {
    public:
        void run() {
            ASSERT_EQUALS( 1, opCount() );
            client()->insert( ns(), fromjson( "{\"a\":\"b\"}" ) );
            ASSERT_EQUALS( 2, opCount() );
        }
    };

    namespace Idempotence {

        class Base : public ReplTests::Base {
        public:
            virtual ~Base() {}
            void run() {
                reset();
                doIt();
                int nOps = opCount();
                check();
                applyAllOperations();
                check();
                ASSERT_EQUALS( nOps, opCount() );

                reset();
                applyAllOperations();
                check();
                ASSERT_EQUALS( nOps, opCount() );
                applyAllOperations();
                check();
                ASSERT_EQUALS( nOps, opCount() );
            }
        protected:
            virtual void doIt() const = 0;
            virtual void check() const = 0;
            virtual void reset() const = 0;
        };

        class InsertTimestamp : public Base {
        public:
            void doIt() const {
                BSONObjBuilder b;
                b.append( "a", 1 );
                b.appendTimestamp( "t" );
                client()->insert( ns(), b.done() );
                date_ = client()->findOne( ns(), QUERY( "a" << 1 ) ).getField( "t" ).date();
            }
            void check() const {
                BSONObj o = client()->findOne( ns(), QUERY( "a" << 1 ) );
                ASSERT( 0 != o.getField( "t" ).date() );
                ASSERT_EQUALS( date_, o.getField( "t" ).date() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            mutable Date_t date_;
        };

        class InsertAutoId : public Base {
        public:
            InsertAutoId() : o_( fromjson( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->insert( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj o_;
        };

        class InsertWithId : public InsertAutoId {
        public:
            InsertWithId() {
                o_ = fromjson( "{\"_id\":ObjectId(\"0f0f0f0f0f0f0f0f0f0f0f0f\"),\"a\":\"b\"}" );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( o_ );
            }
        };

        class InsertTwo : public Base {
        public:
            InsertTwo() :
                o_( fromjson( "{'_id':1,a:'b'}" ) ),
                t_( fromjson( "{'_id':2,c:'d'}" ) ) {}
            void doIt() const {
                vector< BSONObj > v;
                v.push_back( o_ );
                v.push_back( t_ );
                client()->insert( ns(), v );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                checkOne( o_ );
                checkOne( t_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            BSONObj o_;
            BSONObj t_;
        };

        class InsertTwoIdentical : public Base {
        public:
            InsertTwoIdentical() : o_( fromjson( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->insert( ns(), o_ );
                client()->insert( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            BSONObj o_;
        };

        class UpdateTimestamp : public Base {
        public:
            void doIt() const {
                BSONObjBuilder b;
                b.append( "_id", 1 );
                b.appendTimestamp( "t" );
                client()->update( ns(), BSON( "_id" << 1 ), b.done() );
                date_ = client()->findOne( ns(), QUERY( "_id" << 1 ) ).getField( "t" ).date();
            }
            void check() const {
                BSONObj o = client()->findOne( ns(), QUERY( "_id" << 1 ) );
                ASSERT( 0 != o.getField( "t" ).date() );
                ASSERT_EQUALS( date_, o.getField( "t" ).date() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 1 ) );
            }
        private:
            mutable Date_t date_;
        };

        class UpdateSameField : public Base {
        public:
            UpdateSameField() :
                q_( fromjson( "{a:'b'}" ) ),
                o1_( wid( "{a:'b'}" ) ),
                o2_( wid( "{a:'b'}" ) ),
                u_( fromjson( "{a:'c'}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                ASSERT( !client()->findOne( ns(), q_ ).isEmpty() );
                ASSERT( !client()->findOne( ns(), u_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o1_ );
                insert( o2_ );
            }
        private:
            BSONObj q_, o1_, o2_, u_;
        };

        class UpdateSameFieldWithId : public Base {
        public:
            UpdateSameFieldWithId() :
                o_( fromjson( "{'_id':1,a:'b'}" ) ),
                q_( fromjson( "{a:'b'}" ) ),
                u_( fromjson( "{'_id':1,a:'c'}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                ASSERT( !client()->findOne( ns(), q_ ).isEmpty() );
                ASSERT( !client()->findOne( ns(), u_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
                insert( fromjson( "{'_id':2,a:'b'}" ) );
            }
        private:
            BSONObj o_, q_, u_;
        };

        class UpdateSameFieldExplicitId : public Base {
        public:
            UpdateSameFieldExplicitId() :
                o_( fromjson( "{'_id':1,a:'b'}" ) ),
                u_( fromjson( "{'_id':1,a:'c'}" ) ) {}
            void doIt() const {
                client()->update( ns(), o_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( u_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, u_;
        };

        class UpdateDifferentFieldExplicitId : public Base {
        public:
            UpdateDifferentFieldExplicitId() :
                o_( fromjson( "{'_id':1,a:'b'}" ) ),
                q_( fromjson( "{'_id':1}" ) ),
                u_( fromjson( "{'_id':1,a:'c'}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( u_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_;
        };

        class UpsertUpdateNoMods : public UpdateDifferentFieldExplicitId {
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
        };

        class UpsertInsertNoMods : public InsertAutoId {
            void doIt() const {
                client()->update( ns(), fromjson( "{a:'c'}" ), o_, true );
            }
        };

        class UpdateSet : public Base {
        public:
            UpdateSet() :
                o_( fromjson( "{'_id':1,a:5}" ) ),
                q_( fromjson( "{a:5}" ) ),
                u_( fromjson( "{$set:{a:7}}" ) ),
                ou_( fromjson( "{'_id':1,a:7}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };

        class UpdateInc : public Base {
        public:
            UpdateInc() :
                o_( fromjson( "{'_id':1,a:5}" ) ),
                q_( fromjson( "{a:5}" ) ),
                u_( fromjson( "{$inc:{a:3}}" ) ),
                ou_( fromjson( "{'_id':1,a:8}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };

        class UpdateInc2 : public Base {
        public:
            UpdateInc2() :
                o_( fromjson( "{'_id':1,a:5}" ) ),
                q_( fromjson( "{a:5}" ) ),
                u_( fromjson( "{$inc:{a:3},$set:{x:5}}" ) ),
                ou_( fromjson( "{'_id':1,a:8,x:5}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };

        class IncEmbedded : public Base {
        public:
            IncEmbedded() :
                o_( fromjson( "{'_id':1,a:{b:3},b:{b:1}}" ) ),
                q_( fromjson( "{'_id':1}" ) ),
                u_( fromjson( "{$inc:{'a.b':1,'b.b':1}}" ) ),
                ou_( fromjson( "{'_id':1,a:{b:4},b:{b:2}}" ) )
            {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };

        class IncCreates : public Base {
        public:
            IncCreates() :
                o_( fromjson( "{'_id':1}" ) ),
                q_( fromjson( "{'_id':1}" ) ),
                u_( fromjson( "{$inc:{'a':1}}" ) ),
                ou_( fromjson( "{'_id':1,a:1}") )
            {}
            void doIt() const {
                client()->update( ns(), q_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };


        class UpsertInsertIdMod : public Base {
        public:
            UpsertInsertIdMod() :
                q_( fromjson( "{'_id':5,a:4}" ) ),
                u_( fromjson( "{$inc:{a:3}}" ) ),
                ou_( fromjson( "{'_id':5,a:7}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( ou_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj q_, u_, ou_;
        };

        class UpsertInsertSet : public Base {
        public:
            UpsertInsertSet() :
                q_( fromjson( "{a:5}" ) ),
                u_( fromjson( "{$set:{a:7}}" ) ),
                ou_( fromjson( "{a:7}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                ASSERT( !client()->findOne( ns(), ou_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':7,a:7}" ) );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };

        class UpsertInsertInc : public Base {
        public:
            UpsertInsertInc() :
                q_( fromjson( "{a:5}" ) ),
                u_( fromjson( "{$inc:{a:3}}" ) ),
                ou_( fromjson( "{a:8}" ) ) {}
            void doIt() const {
                client()->update( ns(), q_, u_, true );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                ASSERT( !client()->findOne( ns(), ou_ ).isEmpty() );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj o_, q_, u_, ou_;
        };

        class MultiInc : public Base {
        public:

            string s() const {
                stringstream ss;
                auto_ptr<DBClientCursor> cc = client()->query( ns() , Query().sort( BSON( "_id" << 1 ) ) );
                bool first = true;
                while ( cc->more() ) {
                    if ( first ) first = false;
                    else ss << ",";

                    BSONObj o = cc->next();
                    ss << o["x"].numberInt();
                }
                return ss.str();
            }

            void doIt() const {
                client()->insert( ns(), BSON( "_id" << 1 << "x" << 1 ) );
                client()->insert( ns(), BSON( "_id" << 2 << "x" << 5 ) );

                ASSERT_EQUALS( "1,5" , s() );

                client()->update( ns() , BSON( "_id" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
                ASSERT_EQUALS( "2,5" , s() );

                client()->update( ns() , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) );
                ASSERT_EQUALS( "3,5" , s() );

                client()->update( ns() , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) , false , true );
                check();
            }

            void check() const {
                ASSERT_EQUALS( "4,6" , s() );
            }

            void reset() const {
                deleteAll( ns() );
            }
        };

        class UpdateWithoutPreexistingId : public Base {
        public:
            UpdateWithoutPreexistingId() :
                o_( fromjson( "{a:5}" ) ),
                u_( fromjson( "{a:5}" ) ),
                ot_( fromjson( "{b:4}" ) ) {}
            void doIt() const {
                client()->update( ns(), o_, u_ );
            }
            void check() const {
                ASSERT_EQUALS( 2, count() );
                checkOne( u_ );
                checkOne( ot_ );
            }
            void reset() const {
                deleteAll( ns() );
                insert( ot_, true );
                insert( o_, true );
            }
        protected:
            BSONObj o_, u_, ot_;
        };

        class Remove : public Base {
        public:
            Remove() :
                o1_( f( "{\"_id\":\"010101010101010101010101\",\"a\":\"b\"}" ) ),
                o2_( f( "{\"_id\":\"010101010101010101010102\",\"a\":\"b\"}" ) ),
                q_( f( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->remove( ns(), q_ );
            }
            void check() const {
                ASSERT_EQUALS( 0, count() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o1_ );
                insert( o2_ );
            }
        protected:
            BSONObj o1_, o2_, q_;
        };

        class RemoveOne : public Remove {
            void doIt() const {
                client()->remove( ns(), q_, true );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
            }
        };

        class FailingUpdate : public Base {
        public:
            FailingUpdate() :
                o_( fromjson( "{'_id':1,a:'b'}" ) ),
                u_( fromjson( "{'_id':1,c:'d'}" ) ) {}
            void doIt() const {
                client()->update( ns(), o_, u_ );
                client()->insert( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( o_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        protected:
            BSONObj o_, u_;
        };

        class SetNumToStr : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$set" << BSON( "a" << "bcd" ) ) );
            }
            void check() const {
                ASSERT_EQUALS( 1, count() );
                checkOne( BSON( "_id" << 0 << "a" << "bcd" ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 << "a" << 4.0 ) );
            }
        };

        class Push : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }
        };

        class PushUpsert : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) ), true );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }
        };

        class MultiPush : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) << "$push" << BSON( "b.c" << 6.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5],b:{c:[6]}}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }
        };

        class EmptyPush : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$push" << BSON( "a" << 5.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0}" ) );
            }
        };

        class EmptyPushSparseIndex : public EmptyPush {
        public:
            EmptyPushSparseIndex() {
                client()->insert( "unittests.system.indexes",
                                 BSON( "ns" << ns() << "key" << BSON( "a" << 1 ) <<
                                      "name" << "foo" << "sparse" << true ) );
            }
            ~EmptyPushSparseIndex() {
                client()->dropIndexes( ns() );
            }
        };

        class PushAll : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pushAll:{a:[5.0,6.0]}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }
        };

        class PushWithDollarSigns : public Base {
            void doIt() const {
                client()->update( ns(),
                                  BSON( "_id" << 0),
                                  BSON( "$push" << BSON( "a" << BSON( "$foo" << 1 ) ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0, a:[0, {'$foo':1}]}"), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 << "a" << BSON_ARRAY( 0 ) ) );
            }
        };

        class PushSlice : public Base {
            void doIt() const {
                client()->update( ns(),
                                  BSON( "_id" << 0),
                                  BSON( "$push" <<
                                        BSON( "a" <<
                                              BSON( "$each" << BSON_ARRAY(3) <<
                                                    "$slice" << -2 ) ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0, a:[2,3]}"), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 << 2 ) ) );
            }
        };

        class PushSliceInitiallyInexistent : public Base {
            void doIt() const {
                client()->update( ns(),
                                  BSON( "_id" << 0),
                                  BSON( "$push" <<
                                        BSON( "a" <<
                                              BSON( "$each" << BSON_ARRAY(1<<2) <<
                                                    "$slice" << -2 ) ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0, a:[1,2] }"), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 ) );
            }
        };

        class PushSliceToZero : public Base {
            void doIt() const {
                client()->update( ns(),
                                  BSON( "_id" << 0),
                                  BSON( "$push" <<
                                        BSON( "a" <<
                                              BSON( "$each" << BSON_ARRAY(3) <<
                                                    "$slice" << 0 ) ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0, a:[]}"), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 ) );
            }
        };

        class PushAllUpsert : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pushAll:{a:[5.0,6.0]}}" ), true );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4]}" ) );
            }
        };

        class EmptyPushAll : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pushAll:{a:[5.0,6.0]}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0}" ) );
            }
        };

        class Pull : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$pull" << BSON( "a" << 4.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5]}" ) );
            }
        };

        class PullNothing : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), BSON( "$pull" << BSON( "a" << 6.0 ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5]}" ) );
            }
        };

        class PullAll : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pullAll:{a:[4,5]}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5,6]}" ) );
            }
        };

        class Pop : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pop:{a:1}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[4,5]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5,6]}" ) );
            }
        };

        class PopReverse : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$pop:{a:-1}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0,a:[5,6]}" ), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[4,5,6]}" ) );
            }
        };

        class BitOp : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$bit:{a:{and:2,or:8}}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( BSON( "_id" << 0 << "a" << ( ( 3 & 2 ) | 8 ) ) , one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:3}" ) );
            }
        };

        class Rename : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$rename:{a:'b'}}" ) );
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$set:{a:50}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                ASSERT_EQUALS(
                    mutablebson::unordered( BSON( "_id" << 0 << "a" << 50 << "b" << 3 ) ),
                    mutablebson::unordered( one( fromjson( "{'_id':0}" ) ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:3}" ) );
            }
        };

        class RenameReplace : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$rename:{a:'b'}}" ) );
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$set:{a:50}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                ASSERT_EQUALS(
                    mutablebson::unordered( BSON( "_id" << 0 << "a" << 50 << "b" << 3 ) ),
                    mutablebson::unordered( one( fromjson( "{'_id':0}" ) ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:3,b:100}" ) );
            }
        };

        class RenameOverwrite : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$rename:{a:'b'}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                ASSERT_EQUALS(
                    mutablebson::unordered( BSON( "_id" << 0 << "b" << 3 << "z" << 1 ) ),
                    mutablebson::unordered( one( fromjson( "{'_id':0}" ) ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,z:1,a:3}" ) );
            }
        };

        class NoRename : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson( "{$rename:{c:'b'},$set:{z:1}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( BSON( "_id" << 0 << "a" << 3 << "z" << 1 ) , one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:3}" ) );
            }
        };

        class NestedNoRename : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ),
                                  fromjson( "{$rename:{'a.b':'c.d'},$set:{z:1}}"
                                      ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( BSON( "_id" << 0 << "z" << 1 ) , one( fromjson("{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0}" ) );
            }
        };

        class SingletonNoRename : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSONObj(), fromjson("{$rename:{a:'b'}}" ) );

            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{_id:0,z:1}" ), one(fromjson("{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,z:1}" ) );
            }
        };

        class IndexedSingletonNoRename : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSONObj(), fromjson("{$rename:{a:'b'}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{_id:0,z:1}" ), one(fromjson("{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                // Add an index on 'a'.  This prevents the update from running 'in place'.
                client()->ensureIndex( ns(), BSON( "a" << 1 ) );
                insert( fromjson( "{'_id':0,z:1}" ) );
            }
        };

        class AddToSetEmptyMissing : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSON( "_id" << 0 ), fromjson(
                                      "{$addToSet:{a:{$each:[]}}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{_id:0,a:[]}" ), one( fromjson("{'_id':0}" ) )
                    );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0}" ) );
            }
        };

        class AddToSetWithDollarSigns : public Base {
            void doIt() const {
                client()->update( ns(),
                                  BSON( "_id" << 0),
                                  BSON( "$addToSet" << BSON( "a" << BSON( "$foo" << 1 ) ) ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{'_id':0, a:[0, {'$foo':1}]}"), one( fromjson( "{'_id':0}" ) ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( BSON( "_id" << 0 << "a" << BSON_ARRAY( 0 ) ) );
            }
        };

        //
        // replay cases
        //

        class ReplaySetPreexistingNoOpPull : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSONObj(), fromjson( "{$unset:{z:1}}" ));

                // This is logged as {$set:{'a.b':[]},$set:{z:1}}, which might not be
                // replayable against future versions of a document (here {_id:0,a:1,z:1}) due
                // to SERVER-4781. As a result the $set:{z:1} will not be replayed in such
                // cases (and also an exception may abort replication). If this were instead
                // logged as {$set:{z:1}}, SERVER-4781 would not be triggered.
                client()->update( ns(), BSONObj(), fromjson( "{$pull:{'a.b':1}, $set:{z:1}}" ) );
                client()->update( ns(), BSONObj(), fromjson( "{$set:{a:1}}" ) );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{_id:0,a:1,z:1}" ), one( fromjson("{'_id':0}") ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:{b:[]},z:1}" ) );
            }
        };

        class ReplayArrayFieldNotAppended : public Base {
        public:
            void doIt() const {
                client()->update( ns(), BSONObj(), fromjson( "{$push:{'a.0.b':2}}" ) );
                client()->update( ns(), BSONObj(), fromjson( "{$set:{'a.0':1}}") );
            }
            using ReplTests::Base::check;
            void check() const {
                ASSERT_EQUALS( 1, count() );
                check( fromjson( "{_id:0,a:[1,{b:[1]}]}" ), one(fromjson("{'_id':0}") ) );
            }
            void reset() const {
                deleteAll( ns() );
                insert( fromjson( "{'_id':0,a:[{b:[0]},{b:[1]}]}" ) );
            }
        };

    } // namespace Idempotence

    class DeleteOpIsIdBased : public Base {
    public:
        void run() {
            insert( BSON( "_id" << 0 << "a" << 10 ) );
            insert( BSON( "_id" << 1 << "a" << 11 ) );
            insert( BSON( "_id" << 3 << "a" << 10 ) );
            client()->remove( ns(), BSON( "a" << 10 ) );
            ASSERT_EQUALS( 1U, client()->count( ns(), BSONObj() ) );
            insert( BSON( "_id" << 0 << "a" << 11 ) );
            insert( BSON( "_id" << 2 << "a" << 10 ) );
            insert( BSON( "_id" << 3 << "a" << 10 ) );

            applyAllOperations();
            ASSERT_EQUALS( 2U, client()->count( ns(), BSONObj() ) );
            ASSERT( !one( BSON( "_id" << 1 ) ).isEmpty() );
            ASSERT( !one( BSON( "_id" << 2 ) ).isEmpty() );
        }
    };

    class DatabaseIgnorerBasic {
    public:
        void run() {
            DatabaseIgnorer d;
            ASSERT( !d.ignoreAt( "a", OpTime( 4, 0 ) ) );
            d.doIgnoreUntilAfter( "a", OpTime( 5, 0 ) );
            ASSERT( d.ignoreAt( "a", OpTime( 4, 0 ) ) );
            ASSERT( !d.ignoreAt( "b", OpTime( 4, 0 ) ) );
            ASSERT( d.ignoreAt( "a", OpTime( 4, 10 ) ) );
            ASSERT( d.ignoreAt( "a", OpTime( 5, 0 ) ) );
            ASSERT( !d.ignoreAt( "a", OpTime( 5, 1 ) ) );
            // Ignore state is expired.
            ASSERT( !d.ignoreAt( "a", OpTime( 4, 0 ) ) );
        }
    };

    class DatabaseIgnorerUpdate {
    public:
        void run() {
            DatabaseIgnorer d;
            d.doIgnoreUntilAfter( "a", OpTime( 5, 0 ) );
            d.doIgnoreUntilAfter( "a", OpTime( 6, 0 ) );
            ASSERT( d.ignoreAt( "a", OpTime( 5, 5 ) ) );
            ASSERT( d.ignoreAt( "a", OpTime( 6, 0 ) ) );
            ASSERT( !d.ignoreAt( "a", OpTime( 6, 1 ) ) );

            d.doIgnoreUntilAfter( "a", OpTime( 5, 0 ) );
            d.doIgnoreUntilAfter( "a", OpTime( 6, 0 ) );
            d.doIgnoreUntilAfter( "a", OpTime( 6, 0 ) );
            d.doIgnoreUntilAfter( "a", OpTime( 5, 0 ) );
            ASSERT( d.ignoreAt( "a", OpTime( 5, 5 ) ) );
            ASSERT( d.ignoreAt( "a", OpTime( 6, 0 ) ) );
            ASSERT( !d.ignoreAt( "a", OpTime( 6, 1 ) ) );
        }
    };

    /**
     * Check against oldest document in the oplog before scanning backward
     * from the newest document.
     */
    class FindingStartCursorStale : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                client()->insert( ns(), BSON( "_id" << i ) );
            }
            Lock::GlobalWrite lk;
            Client::Context ctx( cllNS() );
            NamespaceDetails *nsd = nsdetails( cllNS() );
            BSONObjBuilder b;
            b.appendTimestamp( "$gte" );
            BSONObj query = BSON( "ts" << b.obj() );
            FieldRangeSetPair frsp( cllNS(), query );
            BSONObj order = BSON( "$natural" << 1 );
            scoped_ptr<QueryPlan> qp( QueryPlan::make( nsd, -1, frsp, &frsp, query, order ) );
            scoped_ptr<FindingStartCursor> fsc( FindingStartCursor::make( *qp ) );
            ASSERT( fsc->done() );
            ASSERT_EQUALS( 0, fsc->cursor()->current()[ "o" ].Obj()[ "_id" ].Int() );
        }
    };

    /** Check unsuccessful yield recovery with FindingStartCursor */
    class FindingStartCursorYield : public Base {
    public:
        void run() {
            for( int i = 0; i < 10; ++i ) {
                client()->insert( ns(), BSON( "_id" << i ) );
            }
            Date_t ts = client()->query( "local.oplog.$main", Query().sort( BSON( "$natural" << 1 ) ), 1, 4 )->next()[ "ts" ].date();
            Client::Context ctx( cllNS() );
            NamespaceDetails *nsd = nsdetails( cllNS() );
            BSONObjBuilder b;
            b.appendDate( "$gte", ts );
            BSONObj query = BSON( "ts" << b.obj() );
            FieldRangeSetPair frsp( cllNS(), query );
            BSONObj order = BSON( "$natural" << 1 );
            scoped_ptr<QueryPlan> qp( QueryPlan::make( nsd, -1, frsp, &frsp, query, order ) );
            scoped_ptr<FindingStartCursor> fsc( FindingStartCursor::make( *qp ) );
            ASSERT( !fsc->done() );
            fsc->next();
            ASSERT( !fsc->done() );
            ASSERT( fsc->prepareToYield() );
            ClientCursor::invalidate( "local.oplog.$main" );
            ASSERT_THROWS( fsc->recoverFromYield(), MsgAssertionException );
        }
    };

    class FindingStartExtentTraversalBase : public Base {
    public:
        FindingStartExtentTraversalBase() {
            client()->dropCollection( ns() );
        }
        virtual ~FindingStartExtentTraversalBase() {
            client()->dropCollection( ns() );
        }
        void run() {
            BSONObj info;
            // Create a capped collection ...
            ASSERT( client()->runCommand( "unittests",
                                          BSON( "create" << "repltests.findingstart" <<
                                                "capped" << true <<
                                                // ... with the specified extent sizes.
                                                "$nExtents" << extentSizes() <<
                                                "autoIndexId" << false ), info ) );
            // Populate documents.
            for( int i = 0; i < numDocs(); ++i ) {
                client()->insert( ns(), BSON( "ts" << i << "payload" << payload8k() ) );
            }
            NamespaceDetails *nsd = nsdetails( ns() );
            BSONObj query = BSON( "ts" << BSON( "$gte" << 0 ) );
            FieldRangeSetPair frsp( ns(), query );
            BSONObj order = BSON( "$natural" << 1 );
            // Create a QueryPlan.
            scoped_ptr<QueryPlan> qp( QueryPlan::make( nsd, -1, frsp, &frsp, query, order ) );
            // Create a FindingStartCursor.
            scoped_ptr<FindingStartCursor> fsc( FindingStartCursor::make( *qp ) );
            // Check the result of FindingStartCursor::prevExtentFirstLoc() called on the specified
            // target().
            ASSERT_EQUALS( expectedPrevExtentFirstLoc().toString(),
                           fsc->prevExtentFirstLoc( target() ).toString() );
        }
    protected:
        static const char* ns() { return "unittests.repltests.findingstart"; }
        static const NamespaceDetails* nsd() { return nsdetails( ns() ); }
        /** Document payload. */
        static string payload8k() { return string( 8*1024, 'a' ); }
        /** An extent of this size is too small to contain one document containing payload8k(). */
        static int tooSmall() { return 1*1024; }
        /** An extent of this size fits one document. */
        static int fitsOne() { return 10*1024; }
        /** An extent of this size fits many documents. */
        static int fitsMany() { return 50*1024; }
        virtual BSONArray extentSizes() const = 0;
        virtual int numDocs() const = 0;
        virtual DiskLoc target() const = 0;
        virtual DiskLoc expectedPrevExtentFirstLoc() const = 0;
    };

    class FindingStartEmptyExtentNonLoopedBase : public FindingStartExtentTraversalBase {
        virtual int numDocs() const {
            // Insert two documents.  Empty extents may exist betwen or preceding them depending on
            // the test case.
            return 2;
        }
        /** Target the prevExtentFirstLoc() request on the non looped collection's last record. */
        virtual DiskLoc target() const { return nsd()->lastRecord(); }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return nsd()->firstRecord(); }
    };

    class FindingStartEmptyExtentLoopedBase : public FindingStartExtentTraversalBase {
        virtual int numDocs() const {
            // Insert three documents.  Empty extents may exist betwen or preceding them depending
            // on the test case.  Some documents may overwrite others (it is a capped collection).
            return 3;
        }
        /**
         * Target the prevExtentFirstLoc() request on the looped collection's first record.  (The
         * most recently saved record is the first in the collection due to extent allocation
         * looping.)
         */
        virtual DiskLoc target() const { return nsd()->firstRecord(); }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return nsd()->lastRecord(); }
    };
    
    /**
     * FindingStartCursor properly skips an empty extent in 'FindExtent' mode of a non looped
     * capped collection.
     */
    class FindingStartEmptyExtentNonLooped : public FindingStartEmptyExtentNonLoopedBase {
        virtual BSONArray extentSizes() const {
            // Iteration          2 <--------< 1 <---------< 0
            return BSON_ARRAY( fitsOne() << tooSmall() << fitsOne() );
        }
    };

    /** Properly skips two empty extents of a non looped capped collection. */
    class FindingStartTwoEmptyExtentsNonLooped : public FindingStartEmptyExtentNonLoopedBase {
        virtual BSONArray extentSizes() const {
            // Iteration          3 <--------< 2 <---------< 1 <---------< 0
            return BSON_ARRAY( fitsOne() << tooSmall() << tooSmall() << fitsOne() );
        }
    };

    /** Properly stops iteration at the beginning of a non looped capped collection. */
    class FindingStartTwoEmptyEarlyExtentsNonLooped : public FindingStartEmptyExtentNonLoopedBase {
        virtual BSONArray extentSizes() const {
            // Iteration          2 <---------< 1 <----------< 0
            return BSON_ARRAY( tooSmall() << tooSmall() << fitsMany() );
        }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return DiskLoc(); }
    };

    /** The first record of the previous extent is returned (as opposed to some other record). */
    class FindingStartFirstRecordNonLooped : public FindingStartExtentTraversalBase {
        virtual int numDocs() const { return 10; }
        virtual BSONArray extentSizes() const {
            // Iteration          1 <--------< 0
            return BSON_ARRAY( fitsMany() << fitsMany() );
        }
        virtual DiskLoc target() const { return nsd()->lastRecord(); }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return nsd()->firstRecord(); }
    };

    /** Properly skips an empty extent of a looped capped collection. */
    class FindingStartEmptyExtentLooped : public FindingStartEmptyExtentLoopedBase {
        virtual BSONArray extentSizes() const {
            // Iteration      ---< 0            2 <--------< 1 <----
            return BSON_ARRAY( fitsOne() << fitsOne() << tooSmall() );
        }
    };

    /** Properly skips two empty extents of a looped capped collection. */
    class FindingStartTwoEmptyExtentsLooped : public FindingStartEmptyExtentLoopedBase {
        virtual BSONArray extentSizes() const {
            // Iteration      ---< 0            3 <--------< 2 <---------< 1 <---
            return BSON_ARRAY( fitsOne() << fitsOne() << tooSmall() << tooSmall() );
        }
    };

    /** Properly stops iteration at the loop point of a looped capped collection. */
    class FindingStartTwoEmptyEarlyExtentsLooped : public FindingStartEmptyExtentLoopedBase {
        virtual BSONArray extentSizes() const {
            // Iteration      --< 0  3 <------< 2 <--------< 1 <-----
            return BSON_ARRAY( fitsOne() << tooSmall() << tooSmall() );
        }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return DiskLoc(); }
    };

    /** The first record of the previous extent is returned (as opposed to some other record). */
    class FindingStartFirstRecordLooped : public FindingStartExtentTraversalBase {
        virtual int numDocs() const { return 35; }
        virtual BSONArray extentSizes() const {
            // Iteration          1 <--------< 0
            return BSON_ARRAY( fitsMany() << fitsMany() );
        }
        virtual DiskLoc target() const { return nsd()->lastRecord(); }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return nsd()->firstRecord(); }
    };

    /** Advance from the fresh side of the cap extent to its previous extent. */
    class CapExtentFreshSidePrevExtent : public FindingStartExtentTraversalBase {
        virtual int numDocs() const {
            // Looped allocation with capExtent the first extent.
            return 10;
        }
        virtual BSONArray extentSizes() const {
            // Iteration      ---< 0            1 <----
            return BSON_ARRAY( fitsMany() << fitsOne() );
        }
        virtual DiskLoc target() const {
            DiskLoc lastRecord = nsd()->lastRecord();
            // Return last record in first extent (second to last record overall).
            return lastRecord.rec()->getPrev( lastRecord );
        }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return nsd()->lastRecord(); }
    };
    
    /**
     * Advance from the stale side of the cap extent to its previous extent.  This is not optimal,
     * but good enough.  See comments in FindingStartCursor::extentFirstLoc().
     */
    class CapExtentStaleSidePrevExtent : public CapExtentFreshSidePrevExtent {
        virtual DiskLoc target() const { return nsd()->firstRecord(); }
    };

    /** prevExtentFirstLoc() when there is only one extent (non looped) returns DiskLoc(). */
    class FindingStartSingleExtentNonLooped : public FindingStartExtentTraversalBase {
        virtual int numDocs() const { return 2; }
        virtual BSONArray extentSizes() const { return BSON_ARRAY( fitsMany() ); }
        virtual DiskLoc target() const { return nsd()->lastRecord(); }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return DiskLoc(); }
    };

    /** prevExtentFirstLoc() when there is only one extent (looped) returns DiskLoc(). */
    class FindingStartSingleExtentLooped : public FindingStartExtentTraversalBase {
        virtual int numDocs() const { return 10; }
        virtual BSONArray extentSizes() const { return BSON_ARRAY( fitsMany() ); }
        virtual DiskLoc target() const { return nsd()->lastRecord(); }
        virtual DiskLoc expectedPrevExtentFirstLoc() const { return DiskLoc(); }
    };
    
    /** Check ReplSetConfig::MemberCfg equality */
    class ReplSetMemberCfgEquality : public Base {
    public:
        void run() {
            ReplSetConfig::MemberCfg m1, m2;
            verify(m1 == m2);
            m1.tags["x"] = "foo";
            verify(m1 != m2);
            m2.tags["y"] = "bar";
            verify(m1 != m2);
            m1.tags["y"] = "bar";
            verify(m1 != m2);
            m2.tags["x"] = "foo";
            verify(m1 == m2);
            m1.tags.clear();
            verify(m1 != m2);
        }
    };

    class SyncTest : public Sync {
    public:
        bool returnEmpty;
        SyncTest() : Sync(""), returnEmpty(false) {}
        virtual ~SyncTest() {}
        virtual BSONObj getMissingDoc(const BSONObj& o) {
            if (returnEmpty) {
                BSONObj o;
                return o;
            }
            return BSON("_id" << "on remote" << "foo" << "baz");
        }
    };

    class ShouldRetry : public Base {
    public:
        void run() {
            bool threw = false;
            BSONObj o = BSON("ns" << ns() << "o" << BSON("foo" << "bar") << "o2" << BSON("_id" << "in oplog" << "foo" << "bar"));

            // this should fail because we can't connect
            try {
                Sync badSource("localhost:123");
                badSource.getMissingDoc(o);
            }
            catch (DBException&) {
                threw = true;
            }
            verify(threw);

            // now this should succeed
            SyncTest t;
            verify(t.shouldRetry(o));
            verify(!client()->findOne(ns(), BSON("_id" << "on remote")).isEmpty());

            // force it not to find an obj
            t.returnEmpty = true;
            verify(!t.shouldRetry(o));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "repl" ) {
        }

        void setupTests() {
            add< LogBasic >();
            add< Idempotence::InsertTimestamp >();
            add< Idempotence::InsertAutoId >();
            add< Idempotence::InsertWithId >();
            add< Idempotence::InsertTwo >();
            add< Idempotence::InsertTwoIdentical >();
            add< Idempotence::UpdateTimestamp >();
            add< Idempotence::UpdateSameField >();
            add< Idempotence::UpdateSameFieldWithId >();
            add< Idempotence::UpdateSameFieldExplicitId >();
            add< Idempotence::UpdateDifferentFieldExplicitId >();
            add< Idempotence::UpsertUpdateNoMods >();
            add< Idempotence::UpsertInsertNoMods >();
            add< Idempotence::UpdateSet >();
            add< Idempotence::UpdateInc >();
            add< Idempotence::UpdateInc2 >();
            add< Idempotence::IncEmbedded >(); // SERVER-716
            add< Idempotence::IncCreates >(); // SERVER-717
            add< Idempotence::UpsertInsertIdMod >();
            add< Idempotence::UpsertInsertSet >();
            add< Idempotence::UpsertInsertInc >();
            add< Idempotence::MultiInc >();
            // Don't worry about this until someone wants this functionality.
//            add< Idempotence::UpdateWithoutPreexistingId >();
            add< Idempotence::Remove >();
            add< Idempotence::RemoveOne >();
            add< Idempotence::FailingUpdate >();
            add< Idempotence::SetNumToStr >();
            add< Idempotence::Push >();
            add< Idempotence::PushUpsert >();
            add< Idempotence::MultiPush >();
            add< Idempotence::EmptyPush >();
            add< Idempotence::EmptyPushSparseIndex >();
            add< Idempotence::PushAll >();
            add< Idempotence::PushSlice >();
            add< Idempotence::PushSliceInitiallyInexistent >();
            add< Idempotence::PushSliceToZero >();
            add< Idempotence::PushAllUpsert >();
            add< Idempotence::EmptyPushAll >();
            add< Idempotence::Pull >();
            add< Idempotence::PullNothing >();
            add< Idempotence::PullAll >();
            add< Idempotence::Pop >();
            add< Idempotence::PopReverse >();
            add< Idempotence::BitOp >();
            add< Idempotence::Rename >();
            add< Idempotence::RenameReplace >();
            add< Idempotence::RenameOverwrite >();
            add< Idempotence::NoRename >();
            add< Idempotence::NestedNoRename >();
            add< Idempotence::SingletonNoRename >();
            add< Idempotence::IndexedSingletonNoRename >();
            add< Idempotence::AddToSetEmptyMissing >();
            add< Idempotence::ReplaySetPreexistingNoOpPull >();
            add< Idempotence::ReplayArrayFieldNotAppended >();
            add< DeleteOpIsIdBased >();
            add< DatabaseIgnorerBasic >();
            add< DatabaseIgnorerUpdate >();
            add< FindingStartCursorStale >();
            add< FindingStartCursorYield >();
            add< FindingStartEmptyExtentNonLooped >();
            add< FindingStartTwoEmptyExtentsNonLooped >();
            add< FindingStartTwoEmptyEarlyExtentsNonLooped >();
            add< FindingStartFirstRecordNonLooped >();
            add< FindingStartEmptyExtentLooped >();
            add< FindingStartTwoEmptyExtentsLooped >();
            add< FindingStartTwoEmptyEarlyExtentsLooped >();
            add< FindingStartFirstRecordLooped >();
            add< CapExtentFreshSidePrevExtent >();
            add< CapExtentStaleSidePrevExtent >();
            add< FindingStartSingleExtentNonLooped >();
            add< FindingStartSingleExtentLooped >();
            add< ReplSetMemberCfgEquality >();
            add< ShouldRetry >();
        }
    } myall;

} // namespace ReplTests

