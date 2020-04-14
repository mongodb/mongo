/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/request_types/set_shard_version_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/query/query_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

const char kCmdName[] = "setShardVersion";
const char kConfigServer[] = "configdb";
const char kShardName[] = "shard";
const char kShardConnectionString[] = "shardHost";
const char kForceRefresh[] = "forceRefresh";
const char kAuthoritative[] = "authoritative";
const char kNoConnectionVersioning[] = "noConnectionVersioning";

}  // namespace

constexpr StringData SetShardVersionRequest::kVersion;

SetShardVersionRequest::SetShardVersionRequest(ConnectionString configServer,
                                               ShardId shardName,
                                               ConnectionString shardConnectionString,
                                               NamespaceString nss,
                                               ChunkVersion version,
                                               bool isAuthoritative,
                                               bool forceRefresh)
    : _isAuthoritative(isAuthoritative),
      _forceRefresh(forceRefresh),
      _configServer(std::move(configServer)),
      _shardName(std::move(shardName)),
      _shardCS(std::move(shardConnectionString)),
      _nss(std::move(nss)),
      _version(std::move(version)) {}

SetShardVersionRequest::SetShardVersionRequest() = default;

StatusWith<SetShardVersionRequest> SetShardVersionRequest::parseFromBSON(const BSONObj& cmdObj) {
    SetShardVersionRequest request;

    {
        std::string shardName;
        Status status = bsonExtractStringField(cmdObj, kShardName, &shardName);
        request._shardName = ShardId(shardName);

        if (!status.isOK())
            return status;
    }

    {
        std::string shardCS;
        Status status = bsonExtractStringField(cmdObj, kShardConnectionString, &shardCS);
        if (!status.isOK())
            return status;

        auto shardCSStatus = ConnectionString::parse(shardCS);
        if (!shardCSStatus.isOK())
            return shardCSStatus.getStatus();

        request._shardCS = std::move(shardCSStatus.getValue());
    }

    {
        Status status = bsonExtractBooleanFieldWithDefault(
            cmdObj, kForceRefresh, false, &request._forceRefresh);
        if (!status.isOK())
            return status;
    }

    {
        Status status = bsonExtractBooleanFieldWithDefault(
            cmdObj, kAuthoritative, false, &request._isAuthoritative);
        if (!status.isOK())
            return status;
    }

    {
        std::string ns;
        Status status = bsonExtractStringField(cmdObj, kCmdName, &ns);
        if (!status.isOK())
            return status;

        NamespaceString nss(ns);

        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << ns << " is not a valid namespace"};
        }

        request._nss = std::move(nss);
    }

    {
        auto versionStatus = ChunkVersion::parseLegacyWithField(cmdObj, kVersion);
        if (!versionStatus.isOK())
            return versionStatus.getStatus();

        request._version = versionStatus.getValue();
    }

    return request;
}

BSONObj SetShardVersionRequest::toBSON() const {
    BSONObjBuilder cmdBuilder;

    cmdBuilder.append(kCmdName, _nss.get().ns());
    cmdBuilder.append(kForceRefresh, _forceRefresh);
    cmdBuilder.append(kAuthoritative, _isAuthoritative);
    // TODO (SERVER-47440): Remove adding config server to BSON once v4.4 parsing is
    // removed.
    cmdBuilder.append(kConfigServer, _configServer.toString());
    cmdBuilder.append(kShardName, _shardName.toString());
    cmdBuilder.append(kShardConnectionString, _shardCS.toString());

    _version->appendLegacyWithField(&cmdBuilder, kVersion);

    return cmdBuilder.obj();
}

const NamespaceString& SetShardVersionRequest::getNS() const {
    return _nss.get();
}

const ChunkVersion SetShardVersionRequest::getNSVersion() const {
    return _version.get();
}

}  // namespace mongo
