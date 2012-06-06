/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/dbtests/dbtests.h"
#include "mongo/client/dbclient_rs.h"

namespace {
    using mongo::BSONObj;
    using mongo::ReplicaSetMonitor;
    using mongo::HostAndPort;

    const BSONObj SampleIsMasterDoc = BSON( "tags"
                                            << BSON( "dc" << "NYC"
                                                     << "p" << "2"
                                                     << "region" << "NA" ));
    const BSONObj NoTagIsMasterDoc = BSON( "isMaster" << true );

    class SimpleGoodMatchTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = BSON( "tags" << BSON( "dc" << "sf" ));
            ASSERT( node.matchesTag( BSON( "dc" << "sf" )));
        }
    };

    class SimpleBadMatchTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = BSON( "tags" << BSON( "dc" << "nyc" ));
            ASSERT( !node.matchesTag( BSON( "dc" << "sf" )));
        }
    };
    
    class ExactMatchTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( node.matchesTag( SampleIsMasterDoc["tags"].Obj() ));
        }
    };

    class EmptyTagTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( node.matchesTag( BSONObj() ));
        }
    };

    class MemberNoTagMatchesEmptyTagTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = NoTagIsMasterDoc;
            ASSERT( node.matchesTag( BSONObj() ));
        }
    };

    class MemberNoTagDoesNotMatchTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = NoTagIsMasterDoc.copy();
            ASSERT( !node.matchesTag( BSON( "dc" << "NYC" ) ));
        }
    };

    class IncompleteMatchTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( !node.matchesTag( BSON( "dc" << "NYC"
                                            << "p" << "2"
                                            << "hello" << "world" ) ));
        }
    };

    class PartialMatchTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( node.matchesTag( BSON( "dc" << "NYC"
                                           << "p" << "2" )));
        }
    };

    class SingleTagCritTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( node.matchesTag( BSON( "p" << "2" )));
        }
    };

    class BadSingleTagCritTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( !node.matchesTag( BSON( "dc" << "SF" )));
        }
    };

    class NonExistingFieldTagTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( !node.matchesTag( BSON( "noSQL" << "Mongo" )));
        }
    };

    class UnorederedMatchingTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( node.matchesTag( BSON( "p" << "2" << "dc" << "NYC" )));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "replicaSetMonitor" ){
        }

        void setupTests(){
            add< SimpleGoodMatchTest >();
            add< SimpleBadMatchTest >();
            add< ExactMatchTest >();
            add< EmptyTagTest >();
            add< MemberNoTagMatchesEmptyTagTest >();
            add< MemberNoTagDoesNotMatchTest >();
            add< IncompleteMatchTest >();
            add< PartialMatchTest >();
            add< SingleTagCritTest >();
            add< BadSingleTagCritTest >();
            add< NonExistingFieldTagTest >();
            add< UnorederedMatchingTest >();
        }
    } myAll;
}

