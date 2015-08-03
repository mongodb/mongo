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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/dbtests/config_server_fixture.h"

#include <list>

#include "mongo/dbtests/dbtests.h"
#include "mongo/db/service_context.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/legacy/legacy_dist_lock_manager.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::list;
using std::string;

ConfigServerFixture::ConfigServerFixture() : _client(&_txn), _connectHook(NULL) {}

string ConfigServerFixture::shardName() {
    return "TestShardName";
}

void ConfigServerFixture::setUp() {
    shardConnectionPool.clear();
    DBException::traceExceptions = true;

    // Make all connections redirect to the direct client
    _connectHook = new CustomConnectHook(&_txn);
    ConnectionString::setConnectionHook(_connectHook);

    // Create the default config database before querying, necessary for direct connections
    clearServer();
    _client.insert("config.test",
                   BSON("hello"
                        << "world"));
    _client.dropCollection("config.test");

    // Create an index over the chunks, to allow correct diffing
    ASSERT_OK(
        dbtests::createIndex(&_txn,
                             ChunkType::ConfigNS,
                             BSON(ChunkType::ns() << 1 << ChunkType::DEPRECATED_lastmod() << 1)));

    const ConnectionString connStr(uassertStatusOK(ConnectionString::parse("$dummy:10000")));

    ShardingState::get(getGlobalServiceContext())->initialize(connStr.toString());
    ShardingState::get(getGlobalServiceContext())->setShardName(shardName());
}

void ConfigServerFixture::clearServer() {
    _client.dropDatabase("config");
}

void ConfigServerFixture::clearVersion() {
    _client.dropCollection(VersionType::ConfigNS);
}

void ConfigServerFixture::dumpServer() {
    log() << "Dumping virtual config server to log...";

    list<string> collectionNames(_client.getCollectionNames("config"));

    for (list<string>::iterator it = collectionNames.begin(); it != collectionNames.end(); ++it) {
        const string& collection = *it;

        unique_ptr<DBClientCursor> cursor(_client.query(collection, BSONObj()).release());
        ASSERT(cursor.get() != NULL);

        log() << "Dumping collection " << collection;

        while (cursor->more()) {
            BSONObj obj = cursor->nextSafe();
            log() << obj.toString();
        }
    }
}

void ConfigServerFixture::tearDown() {
    ShardingState::get(getGlobalServiceContext())->clearCollectionMetadata();
    clearServer();

    // Make all connections redirect to the direct client
    ConnectionString::setConnectionHook(NULL);
    delete _connectHook;
    _connectHook = NULL;

    DBException::traceExceptions = false;
}

}  // namespace mongo
