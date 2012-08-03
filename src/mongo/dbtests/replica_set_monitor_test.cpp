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
 * This file contains test for ReplicaSetMonitor::Node,
 * ReplicaSetMonitor::selectNode and TagSet
 */

#include <vector>

#include "mongo/client/dbclient_rs.h"
#include "mongo/dbtests/dbtests.h"

namespace {
    using std::vector;
    using boost::scoped_ptr;

    using mongo::BSONObj;
    using mongo::ReplicaSetMonitor;
    using mongo::HostAndPort;
    using mongo::ReadPreference;
    using mongo::TagSet;

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

    class SameValueDiffKeyTest {
    public:
        void run() {
            ReplicaSetMonitor::Node node( HostAndPort(), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT( !node.matchesTag( BSON( "datacenter" << "NYC" )));
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

    class PriNodeCompatibleTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = true;
            node.secondary = false;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "NYC" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class SecNodeCompatibleTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = false;
            node.secondary = true;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "NYC" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class PriNodeNotCompatibleTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = true;
            node.secondary = false;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "SF" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class SecNodeNotCompatibleTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = false;
            node.secondary = true;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "SF" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class PriNodeCompatiblMultiTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = true;
            node.secondary = false;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "RP" ));
            builder.append( BSON( "dc" << "NYC" << "p" << "2" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class SecNodeCompatibleMultiTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = false;
            node.secondary = true;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "RP" ));
            builder.append( BSON( "dc" << "NYC" << "p" << "2" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class PriNodeNotCompatibleMultiTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = true;
            node.secondary = false;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "sf" ));
            builder.append( BSON( "dc" << "NYC" << "P" << "4" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_Nearest, &tags ));
        }
    };

    class SecNodeNotCompatibleMultiTagTest {
    public:
        void run(){
            ReplicaSetMonitor::Node node( HostAndPort( "dummy", 3 ), NULL );
            node.lastIsMaster = SampleIsMasterDoc.copy();

            node.ok = true;
            node.ismaster = false;
            node.secondary = true;

            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "sf" ));
            builder.append( BSON( "dc" << "NYC" << "P" << "4" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !node.isCompatible( ReadPreference_PrimaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_PrimaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryPreferred, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_SecondaryOnly, &tags ));
            ASSERT( !node.isCompatible( ReadPreference_Nearest, &tags ));
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

        nodes[0].lastIsMaster = BSON( "tags" << BSON( "dc" << "nyc" << "p" << "1" ));
        nodes[1].lastIsMaster = BSON( "tags" << BSON( "dc" << "sf" ));
        nodes[2].lastIsMaster = BSON( "tags" << BSON( "dc" << "nyc" << "p" << "2" ));

        return nodes;
    }

    class TagSetFixtures {
    public:
        static BSONArray getDefaultSet();
        static BSONArray getP2Tag();
        static BSONArray getSingleNoMatchTag();
        static BSONArray getMultiNoMatchTag();
    };

    BSONArray TagSetFixtures::getDefaultSet() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append( BSONObj() );
        return arrayBuilder.arr();
    }

    BSONArray TagSetFixtures::getP2Tag() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append( BSON( "p" << "2" ) );
        return arrayBuilder.arr();
    }

    BSONArray TagSetFixtures::getSingleNoMatchTag() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append( BSON( "k" << "x" ) );
        return arrayBuilder.arr();
    }

    BSONArray TagSetFixtures::getMultiNoMatchTag() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append( BSON( "mongo" << "db" ) );
        arrayBuilder.append( BSON( "by" << "10gen" ) );
        return arrayBuilder.arr();
    }

    class PrimaryOnlyTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[0].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PrimaryOnlyPriNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[0].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, &tags, 3,  &lastHost );

            ASSERT( host.empty() );
        }
    };

    class PrimaryMissingTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[0].addr;

            nodes[1].ismaster = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class PriPrefWithPriOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();

            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[0].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 1, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefWithPriNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 1, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecOnlyTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, &tags, 1, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecOnlyOnlyPriOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[0].ok = false;
            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, &tags, 1, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecPrefTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 1, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecPrefWithNoSecOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[0].ok = false;
            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 1, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
            ASSERT_EQUALS("b", lastHost.host());
        }
    };

    class SecPrefWithNoNodeOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[0].ok = false;
            nodes[1].ok = false;
            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 1, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class NearestTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[0].addr;

            nodes[0].pingTimeMillis = 1;
            nodes[1].pingTimeMillis = 2;
            nodes[2].pingTimeMillis = 3;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
            ASSERT_EQUALS("b", lastHost.host());
        }
    };

    class NearestNoLocalTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getDefaultSet() );
            HostAndPort lastHost = nodes[0].addr;

            nodes[0].pingTimeMillis = 10;
            nodes[1].pingTimeMillis = 20;
            nodes[2].pingTimeMillis = 30;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT( !host.empty() );
        }
    };

    class PriOnlyWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getP2Tag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, &tags, 3, &lastHost );

            // Note: PrimaryOnly ignores tag
            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefPriNotOkWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getP2Tag() );
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class PriPrefPriOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getSingleNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefPriNotOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getSingleNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecOnlyWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getP2Tag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, &tags, 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecOnlyWithTagsMatchOnlyPriTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[2].addr;

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "dc" << "sf" ));
            TagSet tags( arrayBuilder.arr() );

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryOnly, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecPrefWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getP2Tag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecPrefSecNotOkWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[1].addr;

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "dc" << "nyc" ));
            TagSet tags( arrayBuilder.arr() );

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecPrefPriOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getSingleNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class SecPrefPriNotOkWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getSingleNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecPrefPriOkWithSecNotMatchTagTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getSingleNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class NearestWithTagsTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[1].addr;

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "p" << "1" ));
            TagSet tags( arrayBuilder.arr() );

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class NearestWithTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getSingleNoMatchTag() );
            HostAndPort lastHost = nodes[1].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class MultiPriOnlyTagTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[1].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
            ASSERT_EQUALS("b", lastHost.host());
        }
    };

    class MultiPriOnlyPriNotOkTagTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[1].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryOnly, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class PriPrefPriOkWithMultiTags {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "p" << "1" ));
            arrayBuilder.append( BSON( "p" << "2" ));

            TagSet tags( arrayBuilder.arr() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class MultiTagsMatchesFirstTest {
    public:
        MultiTagsMatchesFirstTest() {
            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "p" << "1" ));
            arrayBuilder.append( BSON( "p" << "2" ));

            tags.reset( new TagSet( arrayBuilder.arr() ));
        }

        virtual ~MultiTagsMatchesFirstTest() {}

        vector<ReplicaSetMonitor::Node> getNodes() const {
            return NodeSetFixtures::getThreeMemberWithTags();
        }

        TagSet* getTagSet() {
            return tags.get();
        }

    private:
        scoped_ptr<TagSet> tags;
    };

    class MultiTagsMatchesSecondTest {
    public:
        MultiTagsMatchesSecondTest() {
            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "p" << "3" ));
            arrayBuilder.append( BSON( "p" << "2" ));
            arrayBuilder.append( BSON( "p" << "1" ));

            tags.reset( new TagSet( arrayBuilder.arr() ));
        }

        virtual ~MultiTagsMatchesSecondTest() {};

        vector<ReplicaSetMonitor::Node> getNodes() const {
            return NodeSetFixtures::getThreeMemberWithTags();
        }

        TagSet* getTagSet() {
            return tags.get();
        }

    private:
        scoped_ptr<TagSet> tags;
    };

    class MultiTagsMatchesLastTest {
    public:
        MultiTagsMatchesLastTest() {
            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "p" << "12" ));
            arrayBuilder.append( BSON( "p" << "23" ));
            arrayBuilder.append( BSON( "p" << "19" ));
            arrayBuilder.append( BSON( "p" << "34" ));
            arrayBuilder.append( BSON( "p" << "1" ));

            tags.reset( new TagSet( arrayBuilder.arr() ));
        }

        virtual ~MultiTagsMatchesLastTest() {}

        vector<ReplicaSetMonitor::Node> getNodes() const {
            return NodeSetFixtures::getThreeMemberWithTags();
        }

        TagSet* getTagSet() {
            return tags.get();
        }

    private:
        scoped_ptr<TagSet> tags;
    };

    class MultiTagsMatchesPriTest {
    public:
        MultiTagsMatchesPriTest() {
            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "dc" << "sf" ));
            arrayBuilder.append( BSON( "p" << "1" ));
            tags.reset( new TagSet( arrayBuilder.arr() ));
        }

        virtual ~MultiTagsMatchesPriTest() {};

        vector<ReplicaSetMonitor::Node> getNodes() const {
            return NodeSetFixtures::getThreeMemberWithTags();
        }

        TagSet* getTagSet() {
            return tags.get();
        }

    private:
        scoped_ptr<TagSet> tags;
    };

    class PriPrefPriNotOkWithMultiTagsMatchesFirstTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class PriPrefPriNotOkWithMultiTagsMatchesFirstNotOkTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;
            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class PriPrefPriNotOkWithMultiTagsMatchesSecondTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class PriPrefPriNotOkWithMultiTagsMatchesSecondNotOkTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;
            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class PriPrefPriNotOkWithMultiTagsMatchesLastTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class PriPrefPriNotOkWithMultiTagsMatchesLastNotOkTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;
            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class PriPrefPriOkWithMultiTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();

            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class PriPrefPriNotOkWithMultiTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_PrimaryPreferred, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecOnlyWithMultiTagsMatchesFirstTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecOnlyWithMultiTagsMatchesFirstNotOkTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecOnlyWithMultiTagsMatchesSecondTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecOnlyWithMultiTagsMatchesSecondNotOkTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecOnlyWithMultiTagsMatchesLastTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecOnlyWithMultiTagsMatchesLastNotOkTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecOnlyMultiTagsWithPriMatchTest : public MultiTagsMatchesPriTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryOnly, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecOnlyMultiTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryOnly, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SecPrefWithMultiTagsMatchesFirstTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecPrefWithMultiTagsMatchesFirstNotOkTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecPrefWithMultiTagsMatchesSecondTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class SecPrefWithMultiTagsMatchesSecondNotOkTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecPrefWithMultiTagsMatchesLastTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecPrefWithMultiTagsMatchesLastNotOkTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class SecPrefMultiTagsWithPriMatchTest : public MultiTagsMatchesPriTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryPreferred, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class SecPrefMultiTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
        }
    };

    class SecPrefMultiTagsNoMatchPriNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            nodes[1].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_SecondaryPreferred, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class NearestWithMultiTagsMatchesFirstTest : public MultiTagsMatchesFirstTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class NearestWithMultiTagsMatchesFirstNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = NodeSetFixtures::getThreeMemberWithTags();

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "p" << "1" ));
            arrayBuilder.append( BSON( "dc" << "sf" ));

            TagSet tags( arrayBuilder.arr() );
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
            ASSERT_EQUALS("b", lastHost.host());
        }
    };

    class NearestWithMultiTagsMatchesSecondTest : public MultiTagsMatchesSecondTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "c", host.host() );
            ASSERT_EQUALS("c", lastHost.host());
        }
    };

    class NearestWithMultiTagsMatchesSecondNotOkTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[2].addr;

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append( BSON( "z" << "2" ));
            arrayBuilder.append( BSON( "p" << "2" ));
            arrayBuilder.append( BSON( "dc" << "sf" ));

            TagSet tags( arrayBuilder.arr() );

            nodes[2].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
            ASSERT_EQUALS("b", lastHost.host());
        }
    };

    class NearestWithMultiTagsMatchesLastTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "a", host.host() );
            ASSERT_EQUALS("a", lastHost.host());
        }
    };

    class NeatestWithMultiTagsMatchesLastNotOkTest : public MultiTagsMatchesLastTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes = getNodes();
            HostAndPort lastHost = nodes[2].addr;

            nodes[0].ok = false;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                ReadPreference_Nearest, getTagSet(), 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class NearestMultiTagsWithPriMatchTest : public MultiTagsMatchesPriTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_Nearest, getTagSet(), 3, &lastHost );

            ASSERT_EQUALS( "b", host.host() );
            ASSERT_EQUALS("b", lastHost.host());
        }
    };

    class NearestMultiTagsNoMatchTest {
    public:
        void run() {
            vector<ReplicaSetMonitor::Node> nodes =
                    NodeSetFixtures::getThreeMemberWithTags();
            TagSet tags( TagSetFixtures::getMultiNoMatchTag() );
            HostAndPort lastHost = nodes[2].addr;

            HostAndPort host = ReplicaSetMonitor::selectNode( nodes,
                 ReadPreference_Nearest, &tags, 3, &lastHost );

            ASSERT( host.empty() );
        }
    };

    class SingleTagSetTest {
    public:
        void run(){
            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "nyc" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !tags.isExhausted() );
            ASSERT( tags.getCurrentTag().equal( BSON( "dc" << "nyc" )) );

            ASSERT( !tags.isExhausted() );
            tags.next();

            ASSERT( tags.isExhausted() );
#if !(defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF))
            // TODO: remove this guard once SERVER-6317 is fixed
            ASSERT_THROWS( tags.getCurrentTag(), AssertionException );
#endif
        }
    };

    class MultiTagSetTest {
    public:
        void run(){
            BSONArrayBuilder builder;
            builder.append( BSON( "dc" << "nyc" ));
            builder.append( BSON( "dc" << "sf" ));
            builder.append( BSON( "dc" << "ma" ));

            TagSet tags( BSONArray( builder.done() ));

            ASSERT( !tags.isExhausted() );
            ASSERT( tags.getCurrentTag().equal( BSON( "dc" << "nyc" )) );

            ASSERT( !tags.isExhausted() );
            tags.next();
            ASSERT( tags.getCurrentTag().equal( BSON( "dc" << "sf" )) );

            ASSERT( !tags.isExhausted() );
            tags.next();
            ASSERT( tags.getCurrentTag().equal( BSON( "dc" << "ma" )) );

            ASSERT( !tags.isExhausted() );
            tags.next();

            ASSERT( tags.isExhausted() );
#if !(defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF))
            // TODO: remove this guard once SERVER-6317 is fixed
            ASSERT_THROWS( tags.getCurrentTag(), AssertionException );
#endif
        }
    };

    class EmptyArrayTagsTest {
    public:
        void run() {
            BSONArray emptyArray;
            TagSet tags( emptyArray );

            ASSERT( tags.isExhausted() );
#if !(defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF))
            // TODO: remove this guard once SERVER-6317 is fixed
            ASSERT_THROWS( tags.getCurrentTag(), AssertionException );
#endif
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
            add< SameValueDiffKeyTest >();
            add< SimpleToStringTest >();
            add< SimpleToStringWithNoTagTest >();

            add< PriNodeCompatibleTagTest >();
            add< SecNodeCompatibleTagTest >();
            add< PriNodeNotCompatibleTagTest >();
            add< SecNodeNotCompatibleTagTest >();
            add< PriNodeCompatiblMultiTagTest >();
            add< SecNodeCompatibleMultiTagTest >();
            add< PriNodeNotCompatibleMultiTagTest >();
            add< SecNodeNotCompatibleMultiTagTest >();
        }
    } allNode;

    class AllNodeSelectorSuite : public Suite {
    public:
        AllNodeSelectorSuite() : Suite( "replicaSetMonitor_select_node" ){
        }

        void setupTests() {
            add< PrimaryOnlyTest >();
            add< PrimaryOnlyPriNotOkTest >();
            add< PriOnlyWithTagsNoMatchTest >();
            add< PrimaryMissingTest >();
            add< MultiPriOnlyTagTest >();
            add< MultiPriOnlyPriNotOkTagTest >();
            
            add< PriPrefWithPriOkTest >();
            add< PriPrefWithPriNotOkTest >();
            add< PriPrefPriNotOkWithTagsTest >();
            add< PriPrefPriOkWithTagsNoMatchTest >();
            add< PriPrefPriNotOkWithTagsNoMatchTest >();
            add< PriPrefPriOkWithMultiTags >();

            add< PriPrefPriNotOkWithMultiTagsMatchesFirstTest >();
            add< PriPrefPriNotOkWithMultiTagsMatchesFirstNotOkTest >();
            add< PriPrefPriNotOkWithMultiTagsMatchesSecondTest >();
            add< PriPrefPriNotOkWithMultiTagsMatchesSecondNotOkTest >();
            add< PriPrefPriNotOkWithMultiTagsMatchesLastTest >();
            add< PriPrefPriNotOkWithMultiTagsMatchesLastNotOkTest >();
            add< PriPrefPriOkWithMultiTagsNoMatchTest >();
            add< PriPrefPriNotOkWithMultiTagsNoMatchTest >();
            
            add< SecOnlyTest >();
            add< SecOnlyOnlyPriOkTest >();
            add< SecOnlyWithTagsTest >();
            add< SecOnlyWithTagsMatchOnlyPriTest >();

            add< SecOnlyWithMultiTagsMatchesFirstTest >();
            add< SecOnlyWithMultiTagsMatchesFirstNotOkTest >();
            add< SecOnlyWithMultiTagsMatchesSecondTest >();
            add< SecOnlyWithMultiTagsMatchesSecondNotOkTest >();
            add< SecOnlyWithMultiTagsMatchesLastTest >();
            add< SecOnlyWithMultiTagsMatchesLastNotOkTest >();
            add< SecOnlyMultiTagsWithPriMatchTest >();
            add< SecOnlyMultiTagsNoMatchTest >();

            add< SecPrefTest >();
            add< SecPrefWithNoSecOkTest >();
            add< SecPrefWithNoNodeOkTest >();
            add< SecPrefWithTagsTest >();
            add< SecPrefSecNotOkWithTagsTest >();
            add< SecPrefPriOkWithTagsNoMatchTest >();
            add< SecPrefPriNotOkWithTagsNoMatchTest >();
            add< SecPrefPriOkWithSecNotMatchTagTest >();

            add< SecPrefWithMultiTagsMatchesFirstTest >();
            add< SecPrefWithMultiTagsMatchesFirstNotOkTest >();
            add< SecPrefWithMultiTagsMatchesSecondTest >();
            add< SecPrefWithMultiTagsMatchesSecondNotOkTest >();
            add< SecPrefWithMultiTagsMatchesLastTest >();
            add< SecPrefWithMultiTagsMatchesLastNotOkTest >();
            add< SecPrefMultiTagsWithPriMatchTest >();
            add< SecPrefMultiTagsNoMatchTest >();
            add< SecPrefMultiTagsNoMatchPriNotOkTest >();

            add< NearestTest >();
            add< NearestNoLocalTest >();
            add< NearestWithTagsTest >();
            add< NearestWithTagsNoMatchTest >();

            add< NearestWithMultiTagsMatchesFirstTest >();
            add< NearestWithMultiTagsMatchesFirstNotOkTest >();
            add< NearestWithMultiTagsMatchesSecondTest >();
            add< NearestWithMultiTagsMatchesSecondNotOkTest >();
            add< NearestWithMultiTagsMatchesLastTest >();
            add< NearestMultiTagsWithPriMatchTest >();
            add< NearestMultiTagsNoMatchTest >();
        }
    } allNodeSelectorSuite;

    class TagSetSuite : public Suite {
    public:
        TagSetSuite() : Suite( "tagSet" ) {
        }

        void setupTests() {
            add< SingleTagSetTest >();
            add< MultiTagSetTest >();
            add< EmptyArrayTagsTest >();
        }
    } tagSetSuite;
}

