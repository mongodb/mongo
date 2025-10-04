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


#include "mongo/db/global_catalog/type_changelog.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const BSONField<std::string> ChangeLogType::changeId("_id");
const BSONField<std::string> ChangeLogType::server("server");
const BSONField<std::string> ChangeLogType::shard("shard");
const BSONField<std::string> ChangeLogType::clientAddr("clientAddr");
const BSONField<Date_t> ChangeLogType::time("time");
const BSONField<std::string> ChangeLogType::what("what");
const BSONField<BSONObj> ChangeLogType::versionContext("versionContext");
const BSONField<std::string> ChangeLogType::ns("ns");
const BSONField<BSONObj> ChangeLogType::details("details");

const NamespaceString ChangeLogType::ConfigNS(NamespaceString::kConfigChangelogNamespace);

StatusWith<ChangeLogType> ChangeLogType::fromBSON(const BSONObj& source) {
    ChangeLogType changeLog;

    {
        std::string changeLogId;
        Status status = bsonExtractStringField(source, changeId.name(), &changeLogId);
        if (!status.isOK())
            return status;
        changeLog._changeId = changeLogId;
    }

    {
        std::string changeLogServer;
        Status status = bsonExtractStringField(source, server.name(), &changeLogServer);
        if (!status.isOK())
            return status;
        changeLog._server = changeLogServer;
    }

    {
        std::string changeLogShard;
        Status status =
            bsonExtractStringFieldWithDefault(source, shard.name(), "", &changeLogShard);
        if (!status.isOK())
            return status;
        changeLog._shard = changeLogShard;
    }

    {
        std::string changeLogClientAddr;
        Status status = bsonExtractStringField(source, clientAddr.name(), &changeLogClientAddr);
        if (!status.isOK())
            return status;
        changeLog._clientAddr = changeLogClientAddr;
    }

    {
        BSONElement changeLogTimeElem;
        Status status =
            bsonExtractTypedField(source, time.name(), BSONType::date, &changeLogTimeElem);
        if (!status.isOK())
            return status;
        changeLog._time = changeLogTimeElem.date();
    }

    {
        std::string changeLogWhat;
        Status status = bsonExtractStringField(source, what.name(), &changeLogWhat);
        if (!status.isOK())
            return status;
        changeLog._what = changeLogWhat;
    }

    {
        BSONElement changeLogVersionCtxElem;
        Status status = bsonExtractField(source, versionContext.name(), &changeLogVersionCtxElem);
        if (status.isOK())
            changeLog._versionContext = VersionContext{changeLogVersionCtxElem.Obj()};
        else if (status == ErrorCodes::NoSuchKey)
            changeLog._versionContext = boost::none;
        else
            return status;
    }

    {
        std::string changeLogNs;
        Status status = bsonExtractStringFieldWithDefault(source, ns.name(), "", &changeLogNs);
        if (!status.isOK())
            return status;
        changeLog._ns = NamespaceStringUtil::deserialize(
            boost::none, changeLogNs, SerializationContext::stateDefault());
    }

    {
        BSONElement changeLogDetailsElem;
        Status status =
            bsonExtractTypedField(source, details.name(), BSONType::object, &changeLogDetailsElem);
        if (!status.isOK())
            return status;
        changeLog._details = changeLogDetailsElem.Obj().getOwned();
    }

    return changeLog;
}

BSONObj ChangeLogType::toBSON() const {
    BSONObjBuilder builder;

    if (_changeId)
        builder.append(changeId.name(), getChangeId());
    if (_server)
        builder.append(server.name(), getServer());
    if (_shard)
        builder.append(shard.name(), getShard());
    if (_clientAddr)
        builder.append(clientAddr.name(), getClientAddr());
    if (_time)
        builder.append(time.name(), getTime());
    if (_what)
        builder.append(what.name(), getWhat());
    if (_versionContext)
        builder.append(versionContext.name(), getVersionContext()->toBSON());
    if (_ns)
        builder.append(
            ns.name(),
            NamespaceStringUtil::serialize(getNS(), SerializationContext::stateDefault()));
    if (_details)
        builder.append(details.name(), getDetails());

    return builder.obj();
}

void ChangeLogType::setChangeId(const std::string& changeId) {
    _changeId = changeId;
}

void ChangeLogType::setServer(const std::string& server) {
    _server = server;
}

void ChangeLogType::setShard(const std::string& shard) {
    _shard = shard;
}

void ChangeLogType::setClientAddr(const std::string& clientAddr) {
    _clientAddr = clientAddr;
}

void ChangeLogType::setTime(const Date_t& time) {
    _time = time;
}

void ChangeLogType::setWhat(const std::string& what) {
    invariant(!what.empty());
    _what = what;
}

void ChangeLogType::setVersionContext(const VersionContext& vCtx) {
    _versionContext = vCtx;
}

void ChangeLogType::setNS(const NamespaceString& ns) {
    _ns = ns;
}

void ChangeLogType::setDetails(const BSONObj& details) {
    _details = details;
}

std::string ChangeLogType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
