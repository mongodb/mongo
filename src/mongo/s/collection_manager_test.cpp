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

#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/collection_manager.h"
#include "mongo/s/metadata_loader.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace {

    using boost::scoped_ptr;
    using mongo::BSONObj;
    using mongo::BSONArray;
    using mongo::ChunkType;
    using mongo::ConnectionString;
    using mongo::CollectionManager;
    using mongo::CollectionType;
    using mongo::Date_t;
    using mongo::HostAndPort;
    using mongo::MAXKEY;
    using mongo::MetadataLoader;
    using mongo::MINKEY;
    using mongo::OID;
    using mongo::ChunkVersion;
    using mongo::MockConnRegistry;
    using mongo::MockRemoteDBServer;
    using std::string;
    using std::vector;

    // XXX
    // We'd move all the cloning tests here.
    //
    // The "make()" tests belong into the config_reader_test

    class SingleChunkFixture : public mongo::unittest::Test {
    protected:
        scoped_ptr<CollectionManager> manager;
        scoped_ptr<MockRemoteDBServer> dummyConfig;

        void setUp() {
            dummyConfig.reset(new MockRemoteDBServer("$dummy_config"));
            mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
            MockConnRegistry::get()->addServer(dummyConfig.get());

            BSONObj collFoo =  BSON(CollectionType::ns("test.foo") <<
                                    CollectionType::keyPattern(BSON("a" << 1)) <<
                                    CollectionType::unique(false) <<
                                    CollectionType::updatedAt(1ULL) <<
                                    CollectionType::epoch(OID::gen()));
            // XXX Awaiting mock review
            //dummyConfig.setQueryReply("config.collections", BSONArray(collFoo));

            BSONObj fooSingle = BSON(ChunkType::name("test.foo-a_MinKey") <<
                                     ChunkType::ns("test.foo") <<
                                     ChunkType::min(BSON("a" << 10)) <<
                                     ChunkType::max(BSON("a" << 20)) <<
                                     ChunkType::DEPRECATED_lastmod(Date_t(1)) <<
                                     ChunkType::shard("shard0000"));
            // XXX Awaiting mock review
            //dummyConfig.setQueryReply("config.chunks", BSONArray(fooSingle));

            ConnectionString configLoc(HostAndPort("$dummy_config"));
            MetadataLoader loader(configLoc);
            //manager.reset(loader.makeCollectionManager("shard0000", "test.foo", NULL, NULL));
        }

        void tearDown() {
            MockConnRegistry::get()->removeServer(dummyConfig->getServerAddress());
        }
    };

    TEST_F(SingleChunkFixture, CloneSplit) {
        // Determine split point.
        ChunkType chunk;
        chunk.setMin(BSON("a" << 10));
        chunk.setMax(BSON("a" << 20));
        vector<BSONObj> splitKeys;
        splitKeys.push_back(BSON("a" << 15));

        // Setup version to use on splitting.
        //ChunkVersion nextVersion = manager->getMaxShardVersion();
        //nextVersion.incMinor();

        //string errMsg;
        //CollectionManager* cloned = manager->cloneSplit(chunk, splitKeys, nextVersion, &errMsg);
        //ASSERT_TRUE(cloned != NULL);
    }

} // unnamed namespace
