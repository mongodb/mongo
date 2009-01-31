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

    BSONObj f( const char *s ) {
        return fromjson( s );
    }    
    
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
            check( o, one( o ) );
        }
        void checkAll( const BSONObj &o ) const {
            auto_ptr< DBClientCursor > c = client()->query( ns(), o );
            assert( c->more() );
            while( c->more() ) {
                check( o, c->next() );
            }
        }
        void check( const BSONObj &expected, const BSONObj &got ) const {
            if ( expected.woCompare( got ) ) {
                out() << "expected: " << expected.toString()
                    << ", got: " << got.toString() << endl;
            }
            ASSERT( !expected.woCompare( got ) );
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
        static void printAll( const char *ns ) {
            dblock lk;
            setClient( ns );
            auto_ptr< Cursor > c = theDataFileMgr.findAll( ns );
            vector< DiskLoc > toDelete;
            out() << "all for " << ns << endl;
            for(; c->ok(); c->advance() ) {
                out() << c->current().toString() << endl;
            }            
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
                runOne< UpdateMultiple >();
                runOne< UpsertInsert >();
                runOne< UpsertNoInsert >();
            }
            class UpdateSpec {
            public:
                virtual BSONObj o() const = 0;
                virtual BSONObj q() const = 0;                
                virtual BSONObj u() const = 0;
                virtual BSONObj ou() const = 0;
            };
        protected:
            UpdateBase( UpdateSpec *s ) : s_( s ) {}
        private:
            template< class T >
            void runOne() const {
                T test( s_ );
                test.run();
            }
            class Update : public Base {
            public:
                Update( UpdateSpec *s ) : s_( s ) {}
            protected:
                void doIt() const {
                    client()->update( ns(), s_->q(), s_->u() );
                }
                void check() const {
                    ASSERT_EQUALS( 1, count() );
                    checkOne( s_->ou() );
                }
                void reset() const {
                    deleteAll( ns() );
                    insert( s_->o() );                    
                }
                UpdateSpec *s_;
            };
            class UpdateMultiple : public Update {
            public:
                UpdateMultiple( UpdateSpec *s ) : Update( s ) {}
                void check() const {
                    ASSERT_EQUALS( 2, count() );
                    checkOne( s_->ou() );
                    checkOne( s_->o() );
                }
                void reset() const {
                    deleteAll( ns() );
                    insert( s_->o() );                    
                    insert( s_->o() );                    
                }                
            };
            class UpsertInsert : public Update {
            public:
                UpsertInsert( UpdateSpec *s ) : Update( s ) {}
                void doIt() const {
                    client()->update( ns(), s_->q(), s_->u(), true );
                }
                void reset() const {
                    deleteAll( ns() );
                }
            };
            class UpsertNoInsert : public Update {
            public:
                UpsertNoInsert( UpdateSpec *s ) : Update( s ) {}
                void doIt() const {
                    client()->update( ns(), s_->q(), s_->u(), true );
                }                
            };
            UpdateSpec *s_;
        };

        class UpdateSameField : public UpdateBase {
        public:
            class Spec : public UpdateSpec {
                virtual BSONObj o() const { return f( "{\"a\":\"b\",\"m\":\"n\"}" ); }
                virtual BSONObj q() const { return f( "{\"a\":\"b\"}" ); }
                virtual BSONObj u() const { return f( "{\"a\":\"c\"}" ); }
                virtual BSONObj ou() const { return u(); }
            };
            static Spec spec;
            UpdateSameField() :
            UpdateBase( &spec ) {}
        };
        UpdateSameField::Spec UpdateSameField::spec;

        class UpdateDifferentField : public UpdateBase {
        public:
            class Spec : public UpdateSpec {
                virtual BSONObj o() const { return f( "{\"a\":\"b\",\"m\":\"n\",\"x\":\"y\"}" ); }
                virtual BSONObj q() const { return f( "{\"a\":\"b\"}" ); }
                virtual BSONObj u() const { return f( "{\"a\":\"b\",\"x\":\"z\"}" ); }
                virtual BSONObj ou() const { return u(); }
            };
            static Spec spec;
            UpdateDifferentField() :
            UpdateBase( &spec ) {}            
        };
        UpdateDifferentField::Spec UpdateDifferentField::spec;

        class Set : public UpdateBase {
        public:
            class Spec : public UpdateSpec {
                virtual BSONObj o() const { return f( "{\"a\":\"b\",\"m\":1}" ); }
                virtual BSONObj q() const { return f( "{\"a\":\"b\"}" ); }
                virtual BSONObj u() const { return f( "{\"$set\":{\"m\":5}}" ); }
                virtual BSONObj ou() const { return f( "{\"a\":\"b\",\"m\":5}" ); }
            };
            static Spec spec;
            Set() :
            UpdateBase( &spec ) {}                        
        };
        Set::Spec Set::spec;
        
        class SetSame : public UpdateBase {
        public:
            class Spec : public UpdateSpec {
                virtual BSONObj o() const { return f( "{\"a\":10,\"m\":1}" ); }
                virtual BSONObj q() const { return f( "{\"a\":10}" ); }
                virtual BSONObj u() const { return f( "{\"$set\":{\"a\":11}}" ); }
                virtual BSONObj ou() const { return f( "{\"a\":11,\"m\":1}" ); }
            };
            static Spec spec;
            SetSame() :
            UpdateBase( &spec ) {}                        
        };
        SetSame::Spec SetSame::spec;

        class Inc : public UpdateBase {
        public:
            class Spec : public UpdateSpec {
                virtual BSONObj o() const { return f( "{\"a\":\"b\",\"m\":0}" ); }
                virtual BSONObj q() const { return f( "{\"a\":\"b\"}" ); }
                virtual BSONObj u() const { return f( "{\"$inc\":{\"m\":5}}" ); }
                virtual BSONObj ou() const { return f( "{\"a\":\"b\",\"m\":5}" ); }
            };
            static Spec spec;
            Inc() :
            UpdateBase( &spec ) {}                        
        };
        Inc::Spec Inc::spec;

        class IncSame : public UpdateBase {
        public:
            class Spec : public UpdateSpec {
                virtual BSONObj o() const { return f( "{\"a\":0,\"m\":\"n\"}" ); }
                virtual BSONObj q() const { return f( "{\"a\":0}" ); }
                virtual BSONObj u() const { return f( "{\"$inc\":{\"a\":2}}" ); }
                virtual BSONObj ou() const { return f( "{\"a\":2,\"m\":\"n\"}" ); }
            };
            static Spec spec;
            IncSame() :
            UpdateBase( &spec ) {}                        
        };
        IncSame::Spec IncSame::spec;

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
            o_( fromjson( "{\"a\":\"b\"}" ) ),
            u_( fromjson( "{\"c\":\"d\"}" ) ) {}
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
        
    } // namespace Idempotence
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< LogBasic >();
//             add< Idempotence::Insert >();
            add< Idempotence::InsertWithId >();
//             add< Idempotence::InsertTwo >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::InsertTwoIdentical >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::UpdateSameField >();
//            add< Idempotence::UpdateDifferentField >();
//             add< Idempotence::Set >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::SetSame >();
//             add< Idempotence::Inc >();
            // FIXME Decide what is correct & uncomment
//            add< Idempotence::IncSame >();
            add< Idempotence::Remove >();
            add< Idempotence::RemoveOne >();
//             add< Idempotence::FailingUpdate >();
        }
    };
    
} // namespace ReplTests

UnitTest::TestPtr replTests() {
    return UnitTest::createSuite< ReplTests::All >();
}
