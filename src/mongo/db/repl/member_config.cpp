// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/member_config.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/algorithm/string/trim.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

const std::string MemberConfig::kInternalVoterTagName = "$voter";
const std::string MemberConfig::kInternalElectableTagName = "$electable";
const std::string MemberConfig::kInternalAllTagName = "$all";
const std::string MemberConfig::kConfigAllTagName = "$configAll";
const std::string MemberConfig::kConfigVoterTagName = "$configVoter";

/* static */
MemberConfig MemberConfig::parseFromBSON(const BSONObj& mcfg) {
    try {
        return MemberConfig(mcfg);
    } catch (const DBException& e) {
        uassertStatusOK(e.toStatus().withContext(str::stream() << "member: " << mcfg));
        MONGO_UNREACHABLE;
    }
}

MemberConfig::MemberConfig(const BSONObj& mcfg) {
    parseProtected(mcfg, IDLParserContext("MemberConfig"));

    std::string hostAndPortString = std::string{getHost()};
    boost::trim(hostAndPortString);
    HostAndPort host;
    uassertStatusOK(host.initialize(hostAndPortString));
    if (!host.hasPort()) {
        // Make port explicit even if default.
        host = HostAndPort(host.host(), host.port());
    }

    _splitHorizon = SplitHorizon(host, getHorizons());

    if (isArbiter()) {
        if (MemberConfigBase::getPriority() == 1.0) {
            setPriority(0);
        }

        if (!isVoter()) {
            uasserted(ErrorCodes::BadValue, "Arbiter must vote (cannot have 0 votes)");
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
        if (getSecondaryDelay() > Seconds(0)) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when secondaryDelaySecs is used");
        }
        if (isHidden()) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when hidden=true");
        }
        if (!shouldBuildIndexes()) {
            uasserted(ErrorCodes::BadValue, "priority must be 0 when buildIndexes=false");
        }
    }
}

MemberConfig::MemberConfig(const BSONObj& mcfg, ReplSetTagConfig* tagConfig) : MemberConfig(mcfg) {
    addTagInfo(tagConfig);
}

void MemberConfig::addTagInfo(ReplSetTagConfig* tagConfig) {
    // When a ReplSetConfig is created from a MutableReplSetConfig, the MemberConfig objects
    // may have tags from the original configuration, so we need to clear them before adding
    // the tags from the modified configuration.
    _tags.clear();
    //
    // Parse "tags" field.
    //
    if (getTags()) {
        for (auto&& tag : getTags().value()) {
            if (tag.type() != BSONType::string) {
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
        // Arbiters have two internal tags.
        if (_tags.size() != 2) {
            uasserted(ErrorCodes::BadValue, "Cannot set tags on arbiters.");
        }
    }
}

bool MemberConfig::hasTags() const {
    return getTags() && !getTags()->isEmpty();
}

// Changing these members may change the tags, so invalidate them.  The tags will be rebuilt
// when addTagInfo is called.
void MemberConfig::setNewlyAdded(boost::optional<bool> newlyAdded) {
    _tags.clear();
    MemberConfigBase::setNewlyAdded(newlyAdded);
}

void MemberConfig::setArbiterOnly(bool arbiterOnly) {
    _tags.clear();
    MemberConfigBase::setArbiterOnly(arbiterOnly);
}

void MemberConfig::setVotes(int64_t votes) {
    _tags.clear();
    MemberConfigBase::setVotes(votes);
}

void MemberConfig::setPriority(double priority) {
    _tags.clear();
    MemberConfigBase::setPriority(priority);
}

BSONObj MemberConfig::toBSON(bool omitNewlyAddedField) const {
    BSONObjBuilder configBuilder;
    configBuilder.append(kIdFieldName, getId().getData());
    configBuilder.append(kHostFieldName, _host().toString());
    if (getPriorityPort()) {
        configBuilder.append(kPriorityPortFieldName, *getPriorityPort());
    }
    configBuilder.append(kArbiterOnlyFieldName, getArbiterOnly());

    if (!omitNewlyAddedField && getNewlyAdded()) {
        // We should never have _newlyAdded if automatic reconfigs aren't enabled.
        invariant(getNewlyAdded().value());
        configBuilder.append(kNewlyAddedFieldName, getNewlyAdded().value());
    }

    configBuilder.append(kBuildIndexesFieldName, getBuildIndexes());
    configBuilder.append(kHiddenFieldName, getHidden());
    configBuilder.append(kPriorityFieldName, MemberConfigBase::getPriority());

    // For historical reasons we always emit a tag field; some jstests expect it.
    configBuilder.append(kTagsFieldName, getTags() ? *getTags() : BSONObj());

    _splitHorizon.toBSON(configBuilder);

    if (getSecondaryDelaySecs()) {
        configBuilder.append(kSecondaryDelaySecsFieldName, getSecondaryDelaySecs().value());
    }

    configBuilder.append(kVotesFieldName, MemberConfigBase::getVotes() ? 1 : 0);
    return configBuilder.obj();
}

}  // namespace repl
}  // namespace mongo
