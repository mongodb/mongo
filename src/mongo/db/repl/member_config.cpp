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

#include "mongo/db/repl/member_config.h"

#include <boost/algorithm/string.hpp>

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

const std::string MemberConfig::kInternalVoterTagName = "$voter";
const std::string MemberConfig::kInternalElectableTagName = "$electable";
const std::string MemberConfig::kInternalAllTagName = "$all";
const std::string MemberConfig::kConfigAllTagName = "$configAll";
const std::string MemberConfig::kConfigVoterTagName = "$configVoter";

MemberConfig::MemberConfig(const BSONObj& mcfg, ReplSetTagConfig* tagConfig) {
    parseProtected(IDLParserErrorContext("MemberConfig"), mcfg);

    std::string hostAndPortString = getHost().toString();
    boost::trim(hostAndPortString);
    HostAndPort host;
    uassertStatusOK(host.initialize(hostAndPortString));
    if (!host.hasPort()) {
        // Make port explicit even if default.
        host = HostAndPort(host.host(), host.port());
    }

    if (getNewlyAdded()) {
        uassert(
            ErrorCodes::InvalidReplicaSetConfig,
            str::stream() << kNewlyAddedFieldName
                          << " field cannot be specified if enableAutomaticReconfig is turned off",
            enableAutomaticReconfig);
    }

    _splitHorizon = SplitHorizon(host, getHorizons());

    //
    // Parse "tags" field.
    //
    if (getTags()) {
        for (auto&& tag : getTags().get()) {
            if (tag.type() != String) {
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "tags." << tag.fieldName()
                              << " field has non-string value of type " << typeName(tag.type()));
            }
            _tags.push_back(tagConfig->makeTag(tag.fieldNameStringData(), tag.valueStringData()));
        }
    }

    //
    // Add internal tags based on other member properties.
    //

    // Add a voter tag if this non-arbiter member votes; use _id for uniquity.
    const std::string id = std::to_string(getId().getData());
    if (isVoter() && !isArbiter()) {
        _tags.push_back(tagConfig->makeTag(kInternalVoterTagName, id));
    }

    // Add an electable tag if this member is electable.
    if (isElectable()) {
        _tags.push_back(tagConfig->makeTag(kInternalElectableTagName, id));
    }

    // Add a tag for generic counting of this node.
    if (!isArbiter()) {
        _tags.push_back(tagConfig->makeTag(kInternalAllTagName, id));
    }

    // Add a config voter tag if this node counts towards the config majority for reconfig.
    // This excludes non-voting members but does include arbiters.
    if (isVoter()) {
        _tags.push_back(tagConfig->makeTag(kConfigVoterTagName, id));
    }

    // Add a tag for every node, including arbiters.
    _tags.push_back(tagConfig->makeTag(kConfigAllTagName, id));


    if (isArbiter()) {
        if (MemberConfigBase::getPriority() == 1.0) {
            setPriority(0);
        }

        if (!isVoter()) {
            uasserted(ErrorCodes::BadValue, "Arbiter must vote (cannot have 0 votes)");
        }
        // Arbiters have two internal tags.
        if (_tags.size() != 2) {
            uasserted(ErrorCodes::BadValue, "Cannot set tags on arbiters.");
        }

        if (isNewlyAdded()) {
            uasserted(ErrorCodes::BadValue, "Arbiter cannot have newlyAdded field set");
        }
    }

    // Check for additional electable requirements, when priority is non zero.
    if (getPriority() != 0) {
        if (!isVoter()) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when non-voting (votes:0)");
        }
        if (getSlaveDelay() > Seconds(0)) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when slaveDelay is used");
        }
        if (isHidden()) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when hidden=true");
        }
        if (!shouldBuildIndexes()) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when buildIndexes=false");
        }
    }
}

bool MemberConfig::hasTags(const ReplSetTagConfig& tagConfig) const {
    for (std::vector<ReplSetTag>::const_iterator tag = _tags.begin(); tag != _tags.end(); tag++) {
        std::string tagKey = tagConfig.getTagKey(*tag);
        if (tagKey[0] == '$') {
            // Filter out internal tags
            continue;
        }
        return true;
    }
    return false;
}

BSONObj MemberConfig::toBSON(const ReplSetTagConfig& tagConfig, bool omitNewlyAddedField) const {
    BSONObjBuilder configBuilder;
    configBuilder.append(kIdFieldName, getId().getData());
    configBuilder.append(kHostFieldName, _host().toString());
    configBuilder.append(kArbiterOnlyFieldName, getArbiterOnly());

    if (!omitNewlyAddedField && getNewlyAdded()) {
        // We should never have _newlyAdded if automatic reconfigs aren't enabled.
        invariant(enableAutomaticReconfig);
        invariant(getNewlyAdded().get());
        configBuilder.append(kNewlyAddedFieldName, getNewlyAdded().get());
    }

    configBuilder.append(kBuildIndexesFieldName, getBuildIndexes());
    configBuilder.append(kHiddenFieldName, getHidden());
    configBuilder.append(kPriorityFieldName, MemberConfigBase::getPriority());

    BSONObjBuilder tags(configBuilder.subobjStart(kTagsFieldName));
    for (std::vector<ReplSetTag>::const_iterator tag = _tags.begin(); tag != _tags.end(); tag++) {
        std::string tagKey = tagConfig.getTagKey(*tag);
        if (tagKey[0] == '$') {
            // Filter out internal tags
            continue;
        }
        tags.append(tagKey, tagConfig.getTagValue(*tag));
    }
    tags.done();

    _splitHorizon.toBSON(configBuilder);

    configBuilder.append(kSlaveDelaySecsFieldName, getSlaveDelaySecs());
    configBuilder.append(kVotesFieldName, MemberConfigBase::getVotes() ? 1 : 0);
    return configBuilder.obj();
}

}  // namespace repl
}  // namespace mongo
