/**
 *    Copyright (C) 2014 BongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "bongo/platform/basic.h"

#include "bongo/db/conn_pool_options.h"

#include "bongo/base/init.h"
#include "bongo/client/connpool.h"
#include "bongo/client/global_conn_pool.h"
#include "bongo/db/server_parameters.h"
#include "bongo/s/client/shard_connection.h"

namespace bongo {

int ConnPoolOptions::maxConnsPerHost(200);
int ConnPoolOptions::maxShardedConnsPerHost(200);

namespace {

ExportedServerParameter<int, ServerParameterType::kStartupOnly>  //
    maxConnsPerHostParameter(ServerParameterSet::getGlobal(),
                             "connPoolMaxConnsPerHost",
                             &ConnPoolOptions::maxConnsPerHost);

ExportedServerParameter<int, ServerParameterType::kStartupOnly>  //
    maxShardedConnsPerHostParameter(ServerParameterSet::getGlobal(),
                                    "connPoolMaxShardedConnsPerHost",
                                    &ConnPoolOptions::maxShardedConnsPerHost);

BONGO_INITIALIZER(InitializeConnectionPools)(InitializerContext* context) {
    // Initialize the sharded and unsharded outgoing connection pools
    // NOTES:
    // - All bongods and bongoses have both pools
    // - The connection hooks for sharding are added on startup (bongos) or on first sharded
    //   operation (bongod)

    globalConnPool.setName("connection pool");
    globalConnPool.setMaxPoolSize(ConnPoolOptions::maxConnsPerHost);

    shardConnectionPool.setName("sharded connection pool");
    shardConnectionPool.setMaxPoolSize(ConnPoolOptions::maxShardedConnsPerHost);

    return Status::OK();
}
}
}
