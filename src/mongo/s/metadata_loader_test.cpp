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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
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
    using mongo::CollectionManager;
    using mongo::CollectionType;
    using mongo::ConnectionString;
    using mongo::Date_t;
    using mongo::HostAndPort;
    using mongo::MAXKEY;
    using mongo::MINKEY;
    using mongo::MetadataLoader;
    using mongo::OID;
    using mongo::MockConnRegistry;
    using mongo::MockRemoteDBServer;

    // XXX
    // We'd move all the test involving building a new manager from config data and
    // from an oldManager.
    //
    // Cloning tests belong into collection_manager_test

    class ConfigServerFixture : public mongo::unittest::Test {
    protected:
        void setUp() {
            _dummyConfig.reset(new MockRemoteDBServer("$dummy_config"));
            mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
            MockConnRegistry::get()->addServer(_dummyConfig.get());

            BSONObj collFoo =  BSON(CollectionType::ns("test.foo") <<
                                    CollectionType::keyPattern(BSON("a" << 1)) <<
                                    CollectionType::unique(false) <<
                                    CollectionType::updatedAt(1ULL) <<
                                    CollectionType::epoch(OID::gen()));
            // XXX Awaiting mock review
            //_dummyConfig.setQueryReply("config.collections", BSONArray(collFoo));

            BSONObj fooSingle = BSON(ChunkType::name("test.foo-a_MinKey") <<
                                     ChunkType::ns("test.foo") <<
                                     ChunkType::min(BSON("a" << MINKEY)) <<
                                     ChunkType::max(BSON("a" << MAXKEY)) <<
                                     ChunkType::DEPRECATED_lastmod(Date_t(1)) <<
                                     ChunkType::shard("shard0000"));
            // XXX Awaiting mock review
            //_dummyConfig.setQueryReply("config.chunks", BSONArray(fooSingle));
        }

        void tearDown() {
            MockConnRegistry::get()->removeServer(_dummyConfig->getServerAddress());
        }

    private:
        scoped_ptr<MockRemoteDBServer> _dummyConfig;
    };

    TEST(ConfigServerFixture, SingleChunk) {

        // Load from mock server.
        ConnectionString configLoc(HostAndPort("$dummy_config"));
        MetadataLoader loader(configLoc);
        //CollectionManager* manager = loader.makeCollectionManager("shard0000",
        //                                                          "test.foo",
        //                                                         NULL, /* no old manager */
        //                                                         NULL  /* no need for errMsg */ );
        //ASSERT_TRUE(manager != NULL);
    }

} // unnamed namespace
