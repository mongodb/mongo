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

#include "mongo/dbtests/config_server_fixture.h"

#include <list>

#include "mongo/client/distlock.h"
#include "mongo/s/type_changelog.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_config_version.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_mongos.h"
#include "mongo/s/type_shard.h"

namespace mongo {

    using std::list;

    void ConfigServerFixture::setUp() {
        DBException::traceExceptions = true;

        // Make all connections redirect to the direct client
        _connectHook = new CustomConnectHook();
        ConnectionString::setConnectionHook(_connectHook);
        // Disable the lock pinger
        setLockPingerEnabled(false);

        // Create the default config database before querying, necessary for direct connections
        clearServer();
        client().insert("config.test", BSON( "hello" << "world" ));
        client().dropCollection("config.test");

        // Create an index over the chunks, to allow correct diffing
        client().ensureIndex( ChunkType::ConfigNS, // br
                              BSON( ChunkType::ns() << 1 << // br
                                      ChunkType::DEPRECATED_lastmod() << 1 ) );
    }

    void ConfigServerFixture::clearServer() {
        client().dropDatabase("config");
    }

    void ConfigServerFixture::clearVersion() {
        client().dropCollection(VersionType::ConfigNS);
    }

    void ConfigServerFixture::clearShards() {
        client().dropCollection(ShardType::ConfigNS);
    }

    void ConfigServerFixture::clearDatabases() {
        client().dropCollection(DatabaseType::ConfigNS);
    }

    void ConfigServerFixture::clearCollections() {
        client().dropCollection(CollectionType::ConfigNS);
    }

    void ConfigServerFixture::clearChunks() {
        client().dropCollection(ChunkType::ConfigNS);
    }

    void ConfigServerFixture::clearPings() {
        client().dropCollection(MongosType::ConfigNS);
    }

    void ConfigServerFixture::clearChangelog() {
        client().dropCollection(ChangelogType::ConfigNS);
    }

    void ConfigServerFixture::dumpServer() {

        log() << "Dumping virtual config server to log..." << endl;

        list<string> collectionNames(client().getCollectionNames("config"));

        for (list<string>::iterator it = collectionNames.begin(); it != collectionNames.end(); ++it)
        {
            const string& collection = *it;

            scoped_ptr<DBClientCursor> cursor(client().query(collection, BSONObj()).release());
            ASSERT(cursor.get() != NULL);

            log() << "Dumping collection " << collection << endl;

            while (cursor->more()) {
                BSONObj obj = cursor->nextSafe();
                log() << obj.toString() << endl;
            }
        }
    }

    void ConfigServerFixture::tearDown() {

        clearServer();

        // Reset the pinger
        setLockPingerEnabled(true);

        // Make all connections redirect to the direct client
        ConnectionString::setConnectionHook(NULL);
        delete _connectHook;
        _connectHook = NULL;

        DBException::traceExceptions = false;
    }

}
