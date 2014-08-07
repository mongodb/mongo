/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/member_config.h"

#include <boost/algorithm/string.hpp>

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

    const std::string MemberConfig::kIdFieldName = "_id";
    const std::string MemberConfig::kVotesFieldName = "votes";
    const std::string MemberConfig::kPriorityFieldName = "priority";
    const std::string MemberConfig::kHostFieldName = "host";
    const std::string MemberConfig::kHiddenFieldName = "hidden";
    const std::string MemberConfig::kSlaveDelayFieldName = "slaveDelay";
    const std::string MemberConfig::kArbiterOnlyFieldName = "arbiterOnly";
    const std::string MemberConfig::kBuildIndexesFieldName = "buildIndexes";
    const std::string MemberConfig::kTagsFieldName = "tags";

namespace {
    const std::string kLegalMemberConfigFieldNames[] = {
        MemberConfig::kIdFieldName,
        MemberConfig::kVotesFieldName,
        MemberConfig::kPriorityFieldName,
        MemberConfig::kHostFieldName,
        MemberConfig::kHiddenFieldName,
        MemberConfig::kSlaveDelayFieldName,
        MemberConfig::kArbiterOnlyFieldName,
        MemberConfig::kBuildIndexesFieldName,
        MemberConfig::kTagsFieldName
    };

    const int kVotesFieldDefault = 1;
    const double kPriorityFieldDefault = 1.0;
    const Seconds kSlaveDelayFieldDefault(0);
    const bool kArbiterOnlyFieldDefault = false;
    const bool kHiddenFieldDefault = false;
    const bool kBuildIndexesFieldDefault = true;

    const Seconds kMaxSlaveDelay(3600 * 24 * 366);

}  // namespace

    Status MemberConfig::initialize(const BSONObj& mcfg, ReplicaSetTagConfig* tagConfig) {
        Status status = bsonCheckOnlyHasFields(
            "replica set member configuration", mcfg, kLegalMemberConfigFieldNames);
        if (!status.isOK())
            return status;

        //
        // Parse _id field.
        //
        BSONElement idElement = mcfg[kIdFieldName];
        if (idElement.eoo()) {
            return Status(ErrorCodes::NoSuchKey, str::stream() << kIdFieldName <<
                          " field is missing");
        }
        if (!idElement.isNumber()) {
            return Status(ErrorCodes::TypeMismatch, str::stream() << kIdFieldName <<
                          " field has non-numeric type " << typeName(idElement.type()));
        }
        _id = idElement.numberInt();

        //
        // Parse h field.
        //
        std::string hostAndPortString;
        status = bsonExtractStringField(mcfg, kHostFieldName, &hostAndPortString);
        if (!status.isOK())
            return status;
        boost::trim(hostAndPortString);
        status = _host.initialize(hostAndPortString);
        if (!status.isOK())
            return status;
        if (!_host.hasPort()) {
            // make port explicit even if default.
            _host = HostAndPort(_host.host(), _host.port());
        }

        //
        // Parse votes field.
        //
        BSONElement votesElement = mcfg[kVotesFieldName];
        int votes;
        if (votesElement.eoo()) {
            votes = kVotesFieldDefault;
        }
        else if (votesElement.isNumber()) {
            votes = votesElement.numberInt();
        }
        else {
            return Status(ErrorCodes::TypeMismatch, str::stream() << kVotesFieldName <<
                          " field value has non-numeric type " <<
                          typeName(votesElement.type()));
        }
        if (votes != 0 && votes != 1) {
            return Status(ErrorCodes::BadValue, str::stream() << kVotesFieldName <<
                          " field value is " << votesElement.numberInt() << " but must be 0 or 1");
        }
        _isVoter = bool(votes);

        //
        // Parse priority field.
        //
        BSONElement priorityElement = mcfg[kPriorityFieldName];
        if (priorityElement.eoo()) {
            _priority = kPriorityFieldDefault;
        }
        else if (priorityElement.isNumber()) {
            _priority = priorityElement.numberDouble();
        }
        else {
            return Status(ErrorCodes::TypeMismatch, str::stream() << kPriorityFieldName <<
                          " field has non-numeric type " << typeName(priorityElement.type()));
        }

        //
        // Parse arbiterOnly field.
        //
        status = bsonExtractBooleanFieldWithDefault(mcfg,
                                                    kArbiterOnlyFieldName,
                                                    kArbiterOnlyFieldDefault,
                                                    &_arbiterOnly);
        if (!status.isOK())
            return status;

        //
        // Parse slaveDelay field.
        //
        BSONElement slaveDelayElement = mcfg[kSlaveDelayFieldName];
        if (slaveDelayElement.eoo()) {
            _slaveDelay = kSlaveDelayFieldDefault;
        }
        else if (slaveDelayElement.isNumber()) {
            _slaveDelay = Seconds(slaveDelayElement.numberInt());
        }
        else {
            return Status(ErrorCodes::TypeMismatch, str::stream() << kSlaveDelayFieldName <<
                          " field value has non-numeric type " <<
                          typeName(slaveDelayElement.type()));
        }

        //
        // Parse hidden field.
        //
        status = bsonExtractBooleanFieldWithDefault(mcfg,
                                                    kHiddenFieldName,
                                                    kHiddenFieldDefault,
                                                    &_hidden);
        if (!status.isOK())
            return status;

        //
        // Parse buildIndexes field.
        //
        status = bsonExtractBooleanFieldWithDefault(mcfg,
                                                    kBuildIndexesFieldName,
                                                    kBuildIndexesFieldDefault,
                                                    &_buildIndexes);
        if (!status.isOK())
            return status;

        //
        // Parse "tags" field.
        //
        _tags.clear();
        BSONElement tagsElement;
        status = bsonExtractTypedField(mcfg, kTagsFieldName, Object, &tagsElement);
        if (status.isOK()) {
            for (BSONObj::iterator tagIter(tagsElement.Obj()); tagIter.more();) {
                const BSONElement& tag = tagIter.next();
                if (tag.type() != String) {
                    return Status(ErrorCodes::TypeMismatch, str::stream() << "tags." <<
                                  tag.fieldName() << " field has non-string value of type " <<
                                  typeName(tag.type()));
                }
                _tags.push_back(tagConfig->makeTag(tag.fieldNameStringData(),
                                                   tag.valueStringData()));
            }
        }
        else if (ErrorCodes::NoSuchKey != status) {
            return status;
        }

        return Status::OK();
    }

    Status MemberConfig::validate() const {
        if (_id < 0 || _id > 255) {
            return Status(ErrorCodes::BadValue, str::stream() << kIdFieldName <<
                          " field value of " << _id << " is out of range.");
        }

        if (_priority < 0 || _priority > 1000) {
            return Status(ErrorCodes::BadValue, str::stream() << kPriorityFieldName <<
                          " field value of " << _priority << " is out of range");
        }
        if (_arbiterOnly) {
            if (!_tags.empty()) {
                return Status(ErrorCodes::BadValue, "Cannot set tags on arbiters.");
            }
            if (!_isVoter) {
                return Status(ErrorCodes::BadValue, "Arbiter must vote (cannot have 0 votes)");
            }
        }
        if (_slaveDelay < Seconds(0) || _slaveDelay > kMaxSlaveDelay) {
            return Status(ErrorCodes::BadValue, str::stream() << kSlaveDelayFieldName <<
                          " field value of " << _slaveDelay.total_seconds() <<
                          " seconds is out of range");
        }
        if (_slaveDelay > Seconds(0) && _priority != 0) {
            return Status(ErrorCodes::BadValue, "slaveDelay requires priority be zero");
        }
        if (_hidden && _priority != 0) {
            return Status(ErrorCodes::BadValue, "priority must be 0 when hidden=true");
        }
        if (!_buildIndexes && _priority != 0) {
            return Status(ErrorCodes::BadValue, "priority must be 0 when buildIndexes=false");
        }
        return Status::OK();
    }

    BSONObj MemberConfig::toBSON(const ReplicaSetTagConfig& tagConfig) const {
        BSONObjBuilder configBuilder;
        configBuilder.append("_id", _id);
        configBuilder.append("host", _host.toString());
        configBuilder.append("arbiterOnly", _arbiterOnly);
        configBuilder.append("buildIndexes", _buildIndexes);
        configBuilder.append("hidden", _hidden);
        configBuilder.append("priority", _priority);

        BSONObjBuilder tags(configBuilder.subobjStart("tags"));
        for (std::vector<ReplicaSetTag>::const_iterator tag = _tags.begin();
                tag != _tags.end();
                tag++) {
            tags.append(tagConfig.getTagKey(*tag), tagConfig.getTagValue(*tag));
        }
        tags.done();

        configBuilder.append("slaveDelay", _slaveDelay.total_seconds());
        configBuilder.append("votes", getNumVotes());
        return configBuilder.obj();
    }

}  // namespace repl
}  // namespace mongo
