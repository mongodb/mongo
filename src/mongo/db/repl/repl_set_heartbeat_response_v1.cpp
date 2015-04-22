/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_set_heartbeat_response_v1.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

    const std::string kOkFieldName = "ok";
    const std::string kIsReplSetFieldName = "rs";
    const std::string kReplSetFieldName = "set";
    const std::string kMemberStateFieldName = "state";
    const std::string kOpTimeFieldName = "opTime";
    const std::string kSyncSourceFieldName = "syncingTo";
    const std::string kConfigVersionFieldName = "configVersion";
    const std::string kPrimaryIdFieldName = "primaryId";
    const std::string kLastOpCommitFieldName = "lastOpCommitted";
    const std::string kTermFieldName = "term";
    const std::string kConfigFieldName = "config";

    const std::string kLegalHeartbeatFieldNames[] = {
        kOkFieldName,
        kIsReplSetFieldName,
        kReplSetFieldName,
        kMemberStateFieldName,
        kOpTimeFieldName,
        kSyncSourceFieldName,
        kConfigVersionFieldName,
        kPrimaryIdFieldName,
        kLastOpCommitFieldName,
        kTermFieldName,
        kConfigFieldName,
    };

}  // namespace

    void ReplSetHeartbeatResponseV1::addToBSON(BSONObjBuilder* builder) const {
        builder->append(kOkFieldName, 1.0);
        builder->append(kIsReplSetFieldName, _isReplSet);
        builder->append(kReplSetFieldName, _setName);
        builder->append(kMemberStateFieldName, _state.s);
        builder->append(kOpTimeFieldName, _opTime);
        builder->append(kSyncSourceFieldName, _syncingTo);
        builder->append(kConfigVersionFieldName, _configVersion);
        builder->append(kPrimaryIdFieldName, _primaryId);
        builder->append(kLastOpCommitFieldName, _lastOpCommitted);
        builder->append(kTermFieldName, _term);
        if (_configSet) {
            builder->append(kConfigFieldName, _config.toBSON());
        }
    }

    BSONObj ReplSetHeartbeatResponseV1::toBSON() const {
        BSONObjBuilder builder;
        addToBSON(&builder);
        return builder.obj();
    }

    Status ReplSetHeartbeatResponseV1::initialize(const BSONObj& doc) {
        Status status = bsonCheckOnlyHasFields("ReplSetHeartbeatResponse",
                                               doc,
                                               kLegalHeartbeatFieldNames);
        if (!status.isOK())
            return status;

        status = bsonExtractBooleanField(doc, kIsReplSetFieldName, &_isReplSet);
        if (!status.isOK())
            return status;

        status = bsonExtractStringField(doc, kReplSetFieldName, &_setName);
        if (!status.isOK())
            return status;

        long long stateInt;
        status = bsonExtractIntegerField(doc, kMemberStateFieldName, &stateInt);
        if (!status.isOK())
            return status;
        if (stateInt < 0 || stateInt > MemberState::RS_MAX) {
            return Status(ErrorCodes::BadValue, str::stream() << "Value for \"" <<
                          kMemberStateFieldName << "\" in response to replSetHeartbeat is "
                          "out of range; legal values are non-negative and no more than " <<
                          MemberState::RS_MAX);
        }
        _state = MemberState(static_cast<int>(stateInt));

        status = bsonExtractTimestampField(doc, kOpTimeFieldName, &_opTime);
        if (!status.isOK())
            return status;

        status = bsonExtractStringField(doc, kSyncSourceFieldName, &_syncingTo);
        if (!status.isOK())
            return status;

        status = bsonExtractIntegerField(doc, kConfigVersionFieldName, &_configVersion);
        if (!status.isOK())
            return status;

        status = bsonExtractIntegerField(doc, kPrimaryIdFieldName, &_primaryId);
        if (!status.isOK())
            return status;

        status = bsonExtractTimestampField(doc, kLastOpCommitFieldName, &_lastOpCommitted);
        if (!status.isOK())
            return status;

        status = bsonExtractIntegerField(doc, kTermFieldName, &_term);
        if (!status.isOK())
            return status;

        const BSONElement rsConfigElement = doc[kConfigFieldName];
        if (rsConfigElement.eoo()) {
            _configSet = false;
            _config = ReplicaSetConfig();
            return Status::OK();
        }
        else if (rsConfigElement.type() != Object) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << "Expected \"" <<
                          kConfigFieldName << "\" in response to replSetHeartbeat to have type "
                          "Object, but found " << typeName(rsConfigElement.type()));
        }
        _configSet = true;
        return _config.initialize(rsConfigElement.Obj());
    }

    ReplicaSetConfig ReplSetHeartbeatResponseV1::getConfig() const {
        invariant(_configSet);
        return _config;
    }
} // namespace repl
} // namespace mongo
