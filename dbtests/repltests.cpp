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

#include "../db/repl.h"

#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"

#include "dbtests.h"

namespace mongo {
    void createOplog();
}

namespace ReplTests {

    class Base {
    public:
        Base() {
            master = true;
            createOplog();
            dblock lk;
            setClient( ns() );
        }
        ~Base() {
            try {
                master = false;
                deleteAll( ns() );
                deleteAll( logNs() );
            } catch ( ... ) {
                FAIL( "Exception while cleaning up test" );
            }
        }
    protected:
        static const char *ns() {
            return "dbtests.repltests";
        }
        static const char *logNs() {
            return "local.oplog.$main";
        }
        DBClientInterface *client() const { return &client_; }
        BSONObj one( const BSONObj &query = emptyObj ) const {
            return client()->findOne( ns(), query );            
        }
        void checkOne( const BSONObj &o ) const {
            ASSERT( !one( o ).woCompare( o ) );
        }
        void checkAll( const BSONObj &o ) const {
            auto_ptr< DBClientCursor > c = client()->query( ns(), o );
            assert( c->more() );
            while( c->more() ) {
                ASSERT( !o.woCompare( c->next() ) );
            }
        }
        BSONObj oneOp() const { 
            return client()->findOne( logNs(), emptyObj );
        }
        int count() const {
            int count = 0;
            dblock lk;
            setClient( ns() );
            auto_ptr< Cursor > c = theDataFileMgr.findAll( ns() );
            for(; c->ok(); c->advance(), ++count );
            return count;
        }
        static void applyAllOperations() {
            class Applier : public ReplSource {
            public:
                static void apply( const BSONObj &op ) {
                    ReplSource::applyOperation( op );
                }
            };
            dblock lk;
            setClient( logNs() );
            vector< BSONObj > ops;
            for( auto_ptr< Cursor > c = theDataFileMgr.findAll( logNs() ); c->ok(); c->advance() )
                ops.push_back( c->current() );
            setClient( ns() );
            for( vector< BSONObj >::iterator i = ops.begin(); i != ops.end(); ++i )
                Applier::apply( *i );
        }
        // These deletes don't get logged.
        static void deleteAll( const char *ns ) {
            dblock lk;
            setClient( ns );
            auto_ptr< Cursor > c = theDataFileMgr.findAll( ns );
            vector< DiskLoc > toDelete;
            for(; c->ok(); c->advance() ) {
                toDelete.push_back( c->currLoc() );
            }
            for( vector< DiskLoc >::iterator i = toDelete.begin(); i != toDelete.end(); ++i ) {
                theDataFileMgr.deleteRecord( ns, i->rec(), *i, true );            
            }
        }
        static void insert( const BSONObj &o ) {
            dblock lk;
            setClient( ns() );
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize() );
        }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient Base::client_;
    
    class LogBasic : public Base {
    public:
        void run() {
            ASSERT( !database->haveLogged() );
            ASSERT( oneOp().isEmpty() );
            
            client()->insert( ns(), fromjson( "{\"a\":\"b\"}" ) );

            ASSERT( database->haveLogged() );
            ASSERT( !oneOp().isEmpty() );
        }
    };
    
    namespace Idempotence {
        
        class Base : public ReplTests::Base {
        public:
            void run() {
                reset();
                doIt();
                check();
                applyAllOperations();
                check();
                
                reset();
                applyAllOperations();
                check();
                applyAllOperations();
                check();
            }
        protected:
            virtual void doIt() const = 0;
            virtual void check() const = 0;
            virtual void reset() const = 0;
        };
        
        class Insert : public Base {
        public:
            Insert() : o_( fromjson( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
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
            BSONObj o_;
        };

        class InsertWithId : public Insert {
        public:
            InsertWithId() {
                o_ = fromjson( "{\"_id\":ObjectId(\"0f0f0f0f0f0f0f0f0f0f0f0f\"),\"a\":\"b\"}" );
            }
        };
        
        class InsertTwo : public Base {
        public:
            InsertTwo() : 
            o_( fromjson( "{\"a\":\"b\"}" ) ),
            t_( fromjson( "{\"c\":\"d\"}" ) ) {}
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
                checkAll( o_ );
            }
            void reset() const {
                deleteAll( ns() );
            }
        private:
            BSONObj o_;            
        };

        // TODO Maybe make this a test suite?
        class UpdateBase {
        public:
            void run() {
                runOne< Update >();
                runOne< UpsertInsert >();
                runOne< UpsertNoInsert >();
            }
        protected:
            UpdateBase( const BSONObj &q, const BSONObj &u ) : q_( q ), u_( u ) {} 
        private:
            template< class T >
            void runOne() const {
                T test( q_, u_ );
                test.run();
            }
            class Update : public Base {
            public:
                Update( const BSONObj &q, const BSONObj &u ) : q_( q ), u_( u ) {}
            protected:
                void doIt() const {
                    client()->update( ns(), q_, u_ );
                }
                void check() const {
                    ASSERT_EQUALS( 1, count() );
                }
                void reset() const {
                    deleteAll( ns() );
                    insert( q_ );                    
                }
                BSONObj q_, u_;
            };
            class UpsertInsert : public Update {
            public:
                UpsertInsert( const BSONObj &q, const BSONObj &u ) :
                Update( q, u ) {}
                void doIt() const {
                    client()->update( ns(), q_, u_, true );
                }
                void reset() const {
                    deleteAll( ns() );
                }
            };
            class UpsertNoInsert : public Update {
            public:
                UpsertNoInsert( const BSONObj &q, const BSONObj &u ) :
                Update( q, u ) {}
                void doIt() const {
                    client()->update( ns(), q_, u_, true );
                }                
            };
            BSONObj q_, u_;
        };

        class UpdateSameField : public UpdateBase {
        public:
            UpdateSameField() :
            UpdateBase( fromjson( "{\"a\":\"b\"}" ),
                       fromjson( "{\"a\":\"c\"}" ) ) {}
        };

        class UpdateDifferentField : public UpdateBase {
        public:
            UpdateDifferentField() :
            UpdateBase( fromjson( "{\"a\":\"b\"}" ),
                       fromjson( "{\"a\":\"b\",\"x\":\"y\"}" ) ) {}            
        };
             
        class Remove : public Base {
        public:
            Remove() : o_( fromjson( "{\"a\":\"b\"}" ) ) {}
            void doIt() const {
                client()->remove( ns(), o_ );
            }
            void check() const {
                ASSERT_EQUALS( 0, count() );
            }
            void reset() const {
                deleteAll( ns() );
                insert( o_ );
                insert( o_ );
            }
        protected:
            BSONObj o_;            
        };
        
        class RemoveOne : public Remove {
            void doIt() const {
                client()->remove( ns(), o_, true );
            }            
            void check() const {
                ASSERT_EQUALS( 1, count() );
            }
        };
                
    } // namespace Idempotence
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< LogBasic >();
            add< Idempotence::Insert >();
            add< Idempotence::InsertWithId >();
            add< Idempotence::InsertTwo >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::InsertTwoIdentical >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::UpdateSameField >();
            add< Idempotence::UpdateDifferentField >();
            add< Idempotence::Remove >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::RemoveOne >();
        }
    };
    
} // namespace ReplTests

UnitTest::TestPtr replTests() {
    return UnitTest::createSuite< ReplTests::All >();
}