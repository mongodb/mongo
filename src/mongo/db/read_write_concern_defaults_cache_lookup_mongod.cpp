/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/read_write_concern_defaults_cache_lookup_mongod.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failRWCDefaultsLookup);

BSONObj getPersistedDefaultRWConcernDocument(OperationContext* opCtx) {
    uassert(51762,
            "Failing read/write concern persisted defaults lookup because of fail point",
            !MONGO_unlikely(failRWCDefaultsLookup.shouldFail()));

    DBDirectClient client(opCtx);
    return client.findOne(NamespaceString::kConfigSettingsNamespace,
                          BSON("_id" << ReadWriteConcernDefaults::kPersistedDocumentId));
}

}  // namespace

boost::optional<RWConcernDefault> readWriteConcernDefaultsCacheLookupMongoD(
    OperationContext* opCtx) {
    // Note that a default constructed RWConcern is returned if no document is found instead of
    // boost::none. This is to avoid excessive lookups when there is no defaults document, because
    // otherwise every attempt to get the defaults from the RWC cache would trigger a lookup.
    return RWConcernDefault::parse(IDLParserContext("ReadWriteConcernDefaultsCacheLookupMongoD"),
                                   getPersistedDefaultRWConcernDocument(opCtx));
}

void readWriteConcernDefaultsMongodStartupChecks(OperationContext* opCtx) {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        DBDirectClient client(opCtx);
        const auto numPersistedDocuments =
            client.count(NamespaceString::kConfigSettingsNamespace,
                         BSON("_id" << ReadWriteConcernDefaults::kPersistedDocumentId));
        if (numPersistedDocuments != 0) {
            LOGV2_OPTIONS(
                4615613,
                {logv2::LogTag::kStartupWarnings},
                "This node is running as a shard server, but persisted Read/Write Concern (RWC) "
                "defaults are present in {configSettingsNamespace}. This node was likely "
                "previously in an unsharded replica set or a config server. The RWC defaults on "
                "this node will not be used",
                "This node is running as a shard server, but persisted Read/Write Concern (RWC) "
                "defaults are present. This node was likely previously in an unsharded replica set "
                "or a config server. The RWC defaults on this node will not be used",
                "configSettingsNamespace"_attr = NamespaceString::kConfigSettingsNamespace);
        }
    }
}

}  // namespace mongo
