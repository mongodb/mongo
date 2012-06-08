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

/**
 * This file contains test for ReplicaSetMonitor::Node and
 * ReplicaSetMonitor::NodeSelector
 */

#include <vector>

#include "mongo/client/dbclient_rs.h"
#include "mongo/dbtests/dbtests.h"

namespace {
    using std::vector;
    
    using mongo::BSONObj;
    using mongo::ReplicaSetMonitor;
    using mongo::HostAndPort;
    using mongo::ReadPreference;

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

    class SimpleToStringTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            // Should not throw any exceptions
            ASSERT( !node.toString().empty() );
        }
    };

    class SimpleToStringWithNoTagTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = NoTagIsMasterDoc.copy();

            // Should not throw any exceptions
            ASSERT( !node.toString().empty() );
        }
    };

    class NodeSetFixtures {
    public:
        static vector<ReplicaSetMonitor::Node> getThreeMemberWithTags();
    };

    vector<ReplicaSetMonitor::Node> NodeSetFixtures::getThreeMemberWithTags() {
        vector<ReplicaSetMonitor::Node> nodes;

        nodes.push_back( ReplicaSetMonitor::Node( HostAndPort( "a" ), NULL ));
        nodes.push_back( ReplicaSetMonitor::Node( HostAndPort( "b" ), NULL ));
        nodes.push_back( ReplicaSetMonitor::Node( HostAndPort( "c" ), NULL ));

        nodes[0].ok = true;
        nodes[1].ok = true;
        nodes[2].ok = true;

        nodes[0].secondary = true;
        nodes[1].ismaster = true;
        nodes[2].secondary = true;

        nodes[0].lastIsMaster = BSON( "tags" << BSON( "dc" << "nyc"
                                                      << "p" << "1" ));
        nodes[1].lastIsMaster = BSON( "tags" << BSON( "dc" << "sf" ));
        nodes[2].lastIsMaster = BSON( "tags" << BSON( "dc" << "nyc"
                                                      << "p" << "2" ));
        return nodes;
    }

    class PrimaryOnlyTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int dummyIdx = 0;

            HostAndPort host =
                ReplicaSetMonitor::selectNode( nodes,
                    ReadPreference_PrimaryOnly,
                    BSONObj(), 1, 1, dummyIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PrimaryOnlyPriNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int dummyIdx = 0;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, BSONObj(), 1, 1, dummyIdx );

            ASSERT( host.empty() );
        }
    };

    class PriPrefWithPriOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int dummyIdx = 0;

            HostAndPort host =
                ReplicaSetMonitor::selectNode( nodes,
                    ReadPreference_PrimaryPreferred,
                    BSONObj(), 1, 1, dummyIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefWithPriNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 1;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred,
                BSONObj(), 1, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS( 2, nextSecondaryIdx );
        }
    };

    class SecOnlyTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 0;

            nodes[2].ok = false;

            HostAndPort host =
                ReplicaSetMonitor::selectNode( nodes,
                    ReadPreference_SecondaryOnly,
                    BSONObj(), false, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS( 0, nextSecondaryIdx );
        }
    };

    class SecOnlyOnlyPriOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 0;

            nodes[0].ok = false;
            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, BSONObj(), 1, 1, nextSecondaryIdx );

            ASSERT( host.empty() );
        }
    };

    class SecPrefTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 0;

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred,
                BSONObj(), false, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS( 0, nextSecondaryIdx );
        }
    };

    class SecPrefWithNoSecOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 0;

            nodes[0].ok = false;
            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, BSONObj(),
                1, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class NearestTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 0;

            nodes[0].pingTimeMillis = 1;
            nodes[1].pingTimeMillis = 2;
            nodes[2].pingTimeMillis = 3;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, BSONObj(), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class NearestNoLocalTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 0;

            nodes[0].pingTimeMillis = 10;
            nodes[1].pingTimeMillis = 20;
            nodes[2].pingTimeMillis = 30;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, BSONObj(), 3, 1, nextSecondaryIdx );

            ASSERT( !host.empty() );
        }
    };

    class PriOnlyWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, BSON( "p" << "2" ),
                3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefPriNotOkWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred,
                BSON( "p" << "2" ), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS( 2, nextSecondaryIdx );
        }
    };

    class PriPrefPriOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred,
                BSON( "k" << "x" ), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefPriNotOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred,
                BSON( "k" << "x" ), 3, 1, nextSecondaryIdx );

            ASSERT( host.empty() );
        }
    };
    
    class SecOnlyWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly,
                BSON( "p" << "2" ), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS( 2, nextSecondaryIdx );
        }
    };

    class SecOnlyWithTagsMatchOnlyPriTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryOnly,
                 BSON( "dc" << "sf" ), 3, 1, nextSecondaryIdx );

            ASSERT( host.empty() );
        }
    };

    class SecPrefWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred,
                BSON( "p" << "2" ), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS( 2, nextSecondaryIdx );
        }
    };

    class SecPrefSecNotOkWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 1;

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred,
                BSON( "p" << "2" ), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class SecPrefPriOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred,
                BSON( "k" << "x" ), 3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class SecPrefPriNotOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred,
                BSON( "k" << "x" ), 3, 1, nextSecondaryIdx );

            ASSERT( host.empty() );
        }
    };

    class NearestWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 1;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, BSON( "p" << "1" ),
                3, 1, nextSecondaryIdx );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS( 0, nextSecondaryIdx );
        }
    };

    class NearestWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            int nextSecondaryIdx = 2;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, BSON( "c" << "2" ),
                3, 1, nextSecondaryIdx );

            ASSERT( host.empty() );
        }
    };

    class AllNodeSuite : public Suite {
    public:
        AllNodeSuite() : Suite( "replicaSetMonitor_node" ){
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
            add< SimpleToStringTest >();
            add< SimpleToStringWithNoTagTest >();
        }
    } allNode;

    class AllNodeSelectorSuite : public Suite {
    public:
        AllNodeSelectorSuite() : Suite( "replicaSetMonitor_node_selector" ){
        }

        void setupTests() {
            add< PrimaryOnlyTest >();
            add< PrimaryOnlyPriNotOkTest >();
            add< PriOnlyWithTagsNoMatchTest >();
            
            add< PriPrefWithPriOkTest >();
            add< PriPrefWithPriNotOkTest >();
            add< PriPrefPriNotOkWithTagsTest >();
            add< PriPrefPriOkWithTagsNoMatchTest >();
            add< PriPrefPriNotOkWithTagsNoMatchTest >();
            
            add< SecOnlyTest >();
            add< SecOnlyOnlyPriOkTest >();
            add< SecOnlyWithTagsTest >();
            add< SecOnlyWithTagsMatchOnlyPriTest >();

            add< SecPrefTest >();
            add< SecPrefWithNoSecOkTest >();
            add< SecPrefWithTagsTest >();
            add< SecPrefSecNotOkWithTagsTest >();
            add< SecPrefPriOkWithTagsNoMatchTest >();
            add< SecPrefPriNotOkWithTagsNoMatchTest >();

            add< NearestTest >();
            add< NearestNoLocalTest >();
            add< NearestWithTagsTest >();
            add< NearestWithTagsNoMatchTest >();
        }
    } allNodeSelectorSuite;
}

