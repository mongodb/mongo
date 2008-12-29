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

#include "stdafx.h"
#include "../db/replset.h"
#include "dbtests.h"
#include "mockdbclient.h"

//extern bool seemCaughtUp;
// Temorary, so that we compile.  Disabling these tests, but will fix them soon.
bool seemCaughtUp;

namespace PairingTests {
struct Base {
    Base() {
        seemCaughtUp = true;
    }
};

namespace ReplPairTests {
class Create : public Base {
public:
    void run() {
        ReplPair rp1( "foo", "bar" );
        checkFields( rp1, "foo", "foo", DBPort, "bar" );

        ReplPair rp2( "foo:1", "bar" );
        checkFields( rp2, "foo:1", "foo", 1, "bar" );

        // FIXME Should we accept this input?
        ReplPair rp3( "", "bar" );
        checkFields( rp3, "", "", DBPort, "bar" );

        ASSERT_EXCEPTION( ReplPair( "foo:", "bar" ),
                          UserAssertionException );

        ASSERT_EXCEPTION( ReplPair( "foo:0", "bar" ),
                          UserAssertionException );

        ASSERT_EXCEPTION( ReplPair( "foo:10000000", "bar" ),
                          UserAssertionException );

        ASSERT_EXCEPTION( ReplPair( "foo", "" ),
                          UserAssertionException );
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
    Dominant() : oldPort_( port ) {
        port = 10;
    }
    ~Dominant() {
        port = oldPort_;
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
        ASSERT_EQUALS( "foo", rp.info );
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
        rp.negotiate( &cc );
        ASSERT( rp.state == ReplPair::State_Confused );

        rp.state = ReplPair::State_Negotiating;
        cc.one( res( 1, 2 ) );
        rp.negotiate( &cc );
        ASSERT( rp.state == ReplPair::State_Negotiating );

        cc.one( res( 1, ReplPair::State_Slave ) );
        rp.negotiate( &cc );
        ASSERT( rp.state == ReplPair::State_Slave );

        cc.one( res( 1, ReplPair::State_Master ) );
        rp.negotiate( &cc );
        ASSERT( rp.state == ReplPair::State_Master );
    }
private:
    BSONObj res( int ok, int youAre ) {
        BSONObjBuilder b;
        b.appendInt( "ok", ok );
        b.appendInt( "you_are", youAre );
        return b.doneAndDecouple();
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

        BSONObjBuilder b;
        b.append( "foo", 1 );
        TestableReplPair rp3( true, b.doneAndDecouple() );
        rp3.arbitrate();
        ASSERT( rp3.state == ReplPair::State_Master );
    }
private:
    class TestableReplPair : public ReplPair {
    public:
        TestableReplPair( bool connect, const BSONObj &res ) :
                ReplPair( "a", "z" ),
                connect_( connect ),
                res_( res ) {
        }
        virtual
        DBClientConnection *newClientConnection() const {
            MockDBClientConnection * c = new MockDBClientConnection();
            c->connect( connect_ );
            c->res( res_ );
            return c;
        }
    private:
        bool connect_;
        BSONObj res_;
    };
};
} // namespace ReplPairTests

class DirectConnectBase : public Base {
protected:
    void negotiate( ReplPair &a, ReplPair &b ) {
        auto_ptr< DBClientConnection > c( new DirectDBClientConnection( &b, cc() ) );
        a.negotiate( c.get() );
    }
    class DirectConnectionReplPair : public ReplPair {
    public:
        DirectConnectionReplPair( ReplPair *dest ) :
                ReplPair( "a", "c" ),
                dest_( dest ) {
        }
        virtual DBClientConnection *newClientConnection() const {
            return new DirectDBClientConnection( dest_ );
        }
    private:
        ReplPair *dest_;
    };
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
        seemCaughtUp = true;
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
        seemCaughtUp = false;
        checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Slave,
                          "a", "-", ReplPair::State_Negotiating, ReplPair::State_Master );
        // b caught up, a not
        seemCaughtUp = true;
        checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Master,
                          "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );

        // a caught up, b not
        seemCaughtUp = false;
        checkNegotiation( "b", "-", ReplPair::State_Slave, ReplPair::State_Slave,
                          "a", "-", ReplPair::State_Negotiating, ReplPair::State_Master );
        // b caught up, a not
        seemCaughtUp = true;
        checkNegotiation( "b", "-", ReplPair::State_Slave, ReplPair::State_Master,
                          "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );
    }
private:
    class NegateCatchup : public DirectDBClientConnection::ConnectionCallback {
        virtual void beforeCommand() {
            seemCaughtUp = !seemCaughtUp;
        }
        virtual void afterCommand() {
            seemCaughtUp = !seemCaughtUp;
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
        seemCaughtUp = false;
        checkNegotiation( "b", "-", ReplPair::State_Negotiating, ReplPair::State_Negotiating,
                          "a", "-", ReplPair::State_Negotiating, ReplPair::State_Slave );
    }
};

class Arbitrate : public DirectConnectBase {
public:
    void run() {
        ReplPair arb( "c", "-" );
        DirectConnectionReplPair m( &arb );
        m.arbitrate();
        ASSERT( m.state == ReplPair::State_Master );

        seemCaughtUp = false;
        m.state = ReplPair::State_Negotiating;
        m.arbitrate();
        ASSERT( m.state == ReplPair::State_Negotiating );
    }
};

class All : public UnitTest::Suite {
public:
    All() {
        add< ReplPairTests::Create >();
        add< ReplPairTests::Dominant >();
        add< ReplPairTests::SetMaster >();
        add< ReplPairTests::Negotiate >();
        add< ReplPairTests::Arbitrate >();
        add< Negotiate >();
        add< NegotiateWithCatchup >();
        add< NobodyCaughtUp >();
        add< Arbitrate >();
    }
};
} // namespace PairingTests

UnitTest::TestPtr pairingTests() {
    return UnitTest::createSuite< PairingTests::All >();
}
