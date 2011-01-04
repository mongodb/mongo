// pairingtests.cpp : Pairing unit tests.
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
#include "../db/replpair.h"
#include "dbtests.h"
#include "mockdbclient.h"
#include "../db/cmdline.h"

namespace mongo {
    extern PairSync *pairSync;
} // namespace mongo

namespace PairingTests {
    class Base {
    protected:
        Base() {
            backup = pairSync;
            setSynced();
        }
        ~Base() {
            pairSync = backup;
            dblock lk;
            Helpers::emptyCollection( "local.pair.sync" );
            if ( pairSync->initialSyncCompleted() ) {
                // save to db
                pairSync->setInitialSyncCompleted();
            }
        }
        static void setSynced() {
            init();
            pairSync = synced;
            pairSync->setInitialSyncCompletedLocking();
            ASSERT( pairSync->initialSyncCompleted() );
        }
        static void setNotSynced() {
            init();
            pairSync = notSynced;
            ASSERT( !pairSync->initialSyncCompleted() );
        }
        static void flipSync() {
            if ( pairSync->initialSyncCompleted() )
                setNotSynced();
            else
                setSynced();
        }
    private:
        static void init() {
            dblock lk;
            Helpers::emptyCollection( "local.pair.sync" );
            if ( synced != 0 && notSynced != 0 )
                return;
            notSynced = new PairSync();
            notSynced->init();
            synced = new PairSync();
            synced->init();
            synced->setInitialSyncCompleted();
            Helpers::emptyCollection( "local.pair.sync" );
        }
        PairSync *backup;
        static PairSync *synced;
        static PairSync *notSynced;
    };
    PairSync *Base::synced = 0;
    PairSync *Base::notSynced = 0;

    namespace ReplPairTests {
        class Create : public Base {
        public:
            void run() {
                ReplPair rp1( "foo", "bar" );
                checkFields( rp1, "foo", "foo", CmdLine::DefaultDBPort, "bar" );

                ReplPair rp2( "foo:1", "bar" );
                checkFields( rp2, "foo:1", "foo", 1, "bar" );

                // FIXME Should we accept this input?
                ReplPair rp3( "", "bar" );
                checkFields( rp3, "", "", CmdLine::DefaultDBPort, "bar" );

                ASSERT_EXCEPTION( ReplPair( "foo:", "bar" ),
                                  UserException );

                ASSERT_EXCEPTION( ReplPair( "foo:0", "bar" ),
                                  UserException );

                ASSERT_EXCEPTION( ReplPair( "foo:10000000", "bar" ),
                                  UserException );

                ASSERT_EXCEPTION( ReplPair( "foo", "" ),
                                  UserException );
            }
        private:
            void checkFields( const ReplPair &rp,
                              const string &remote,
                              const string &remoteHost,
                              int remotePort,
                              const string &arbHost ) {
                ASSERT( rp.state == ReplPair::State_Negotiating );
                ASSERT_EQUALS( remote, rp.remote );
                ASSERT_EQUALS( remoteHost, rp.remoteHost );
                ASSERT_EQUALS( remotePort, rp.remotePort );
                ASSERT_EQUALS( arbHost, rp.arbHost );
            }
        };

        class Dominant : public Base {
        public:
            Dominant() : oldPort_( cmdLine.port ) {
                cmdLine.port = 10;
            }
            ~Dominant() {
                cmdLine.port = oldPort_;
            }
            void run() {
                ASSERT( ReplPair( "b:9", "-" ).dominant( "b" ) );
                ASSERT( !ReplPair( "b:10", "-" ).dominant( "b" ) );
                ASSERT( ReplPair( "b", "-" ).dominant( "c" ) );
                ASSERT( !ReplPair( "b", "-" ).dominant( "a" ) );
            }
        private:
            int oldPort_;
        };

        class SetMaster {
        public:
            void run() {
                ReplPair rp( "a", "b" );
                rp.setMaster( ReplPair::State_CantArb, "foo" );
                ASSERT( rp.state == ReplPair::State_CantArb );
                ASSERT_EQUALS( rp.info , "foo" );
                rp.setMaster( ReplPair::State_Confused, "foo" );
                ASSERT( rp.state == ReplPair::State_Confused );
            }
        };

        class Negotiate : public Base {
        public:
            void run() {
                ReplPair rp( "a", "b" );
                MockDBClientConnection cc;

                cc.one( res( 0, 0 ) );
                rp.negotiate( &cc, "dummy" );
                ASSERT( rp.state == ReplPair::State_Confused );

                rp.state = ReplPair::State_Negotiating;
                cc.one( res( 1, 2 ) );
                rp.negotiate( &cc, "dummy" );
                ASSERT( rp.state == ReplPair::State_Negotiating );

                cc.one( res( 1, ReplPair::State_Slave ) );
                rp.negotiate( &cc, "dummy" );
                ASSERT( rp.state == ReplPair::State_Slave );

                cc.one( res( 1, ReplPair::State_Master ) );
                rp.negotiate( &cc, "dummy" );
                ASSERT( rp.state == ReplPair::State_Master );
            }
        private:
            BSONObj res( int ok, int youAre ) {
                BSONObjBuilder b;
                b.append( "ok", ok );
                b.append( "you_are", youAre );
                return b.obj();
            }
        };

        class Arbitrate : public Base {
        public:
            void run() {
                ReplPair rp1( "a", "-" );
                rp1.arbitrate();
                ASSERT( rp1.state == ReplPair::State_Master );

                TestableReplPair rp2( false, BSONObj() );
                rp2.arbitrate();
                ASSERT( rp2.state == ReplPair::State_CantArb );

                TestableReplPair rp3( true, fromjson( "{ok:0}" ) );
                rp3.arbitrate();
                ASSERT_EQUALS( rp3.state , ReplPair::State_Confused );

                TestableReplPair rp4( true, fromjson( "{ok:1,you_are:1}" ) );
                rp4.arbitrate();
                ASSERT( rp4.state == ReplPair::State_Master );

                TestableReplPair rp5( true, fromjson( "{ok:1,you_are:0}" ) );
                rp5.arbitrate();
                ASSERT( rp5.state == ReplPair::State_Slave );

                TestableReplPair rp6( true, fromjson( "{ok:1,you_are:-1}" ) );
                rp6.arbitrate();
                // unchanged from initial value
                ASSERT( rp6.state == ReplPair::State_Negotiating );
            }
        private:
            class TestableReplPair : public ReplPair {
            public:
                TestableReplPair( bool connect, const BSONObj &one ) :
                    ReplPair( "a", "z" ),
                    connect_( connect ),
                    one_( one ) {
                }
                virtual
                DBClientConnection *newClientConnection() const {
                    MockDBClientConnection * c = new MockDBClientConnection();
                    c->connect( connect_ );
                    c->one( one_ );
                    return c;
                }
            private:
                bool connect_;
                BSONObj one_;
            };
        };
    } // namespace ReplPairTests

    class DirectConnectBase : public Base {
    public:
        virtual ~DirectConnectBase() {}
    protected:
        void negotiate( ReplPair &a, ReplPair &b ) {
            auto_ptr< DBClientConnection > c( new DirectDBClientConnection( &b, cc() ) );
            a.negotiate( c.get(), "dummy" );
        }
        virtual DirectDBClientConnection::ConnectionCallback *cc() {
            return 0;
        }
        void checkNegotiation( const char *host1, const char *arb1, int state1, int newState1,
                               const char *host2, const char *arb2, int state2, int newState2 ) {
            ReplPair one( host1, arb1 );
            one.state = state1;
            ReplPair two( host2, arb2 );
            two.state = state2;
            negotiate( one, two );
            ASSERT( one.state == newState1 );
            ASSERT( two.state == newState2 );
        }
    };

    class Negotiate : public DirectConnectBase {
    public:
        void run() {
            checkNegotiation( "a", "-", ReplPair::State_Negotiating, ReplPair::State_Negotiating,
                              "b", "-", ReplPair::State_Negotiating, ReplPair::State_Negotiating );
            checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Slave,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Master );

            checkNegotiation( "b", "-", ReplPair::State_Master, ReplPair::State_Master,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );

            // No change when negotiate() called on a.
            checkNegotiation( "a", "-", ReplPair::State_Master, ReplPair::State_Master,
                              "b", "-", ReplPair::State_Master, ReplPair::State_Master );
            // Resolve Master - Master.
            checkNegotiation( "b", "-", ReplPair::State_Master, ReplPair::State_Slave,
                              "a", "-", ReplPair::State_Master, ReplPair::State_Master );

            // FIXME Move from negotiating to master?
            checkNegotiation( "b", "-", ReplPair::State_Slave, ReplPair::State_Slave,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Master );
        }
    };

    class NegotiateWithCatchup : public DirectConnectBase {
    public:
        void run() {
            // a caught up, b not
            setNotSynced();
            checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Slave,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Master );
            // b caught up, a not
            setSynced();
            checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Master,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );

            // a caught up, b not
            setNotSynced();
            checkNegotiation( "b", "-", ReplPair::State_Slave, ReplPair::State_Slave,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Master );
            // b caught up, a not
            setSynced();
            checkNegotiation( "b", "-", ReplPair::State_Slave, ReplPair::State_Master,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );
        }
    private:
        class NegateCatchup : public DirectDBClientConnection::ConnectionCallback {
            virtual void beforeCommand() {
                Base::flipSync();
            }
            virtual void afterCommand() {
                Base::flipSync();
            }
        };
        virtual DirectDBClientConnection::ConnectionCallback *cc() {
            return &cc_;
        }
        NegateCatchup cc_;
    };

    class NobodyCaughtUp : public DirectConnectBase {
    public:
        void run() {
            setNotSynced();
            checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Negotiating,
                              "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "pairing" ) {
        }

        void setupTests() {
            add< ReplPairTests::Create >();
            add< ReplPairTests::Dominant >();
            add< ReplPairTests::SetMaster >();
            add< ReplPairTests::Negotiate >();
            add< ReplPairTests::Arbitrate >();
            add< Negotiate >();
            add< NegotiateWithCatchup >();
            add< NobodyCaughtUp >();
        }
    } myall;
} // namespace PairingTests

