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

#include "mongo/db/repl/repl_set_config.h"

#include <algorithm>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <functional>

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config_params_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

// Allow the heartbeat interval to be forcibly overridden on this node.
MONGO_FAIL_POINT_DEFINE(forceHeartbeatIntervalMS);

const Milliseconds ReplSetConfig::kInfiniteCatchUpTimeout(-1);
const Milliseconds ReplSetConfig::kCatchUpDisabled(0);
const Milliseconds ReplSetConfig::kCatchUpTakeoverDisabled(-1);

const Milliseconds ReplSetConfig::kDefaultHeartbeatInterval(2000);
const Seconds ReplSetConfig::kDefaultHeartbeatTimeoutPeriod(10);
const Milliseconds ReplSetConfig::kDefaultElectionTimeoutPeriod(10000);
const Milliseconds ReplSetConfig::kDefaultCatchUpTimeoutPeriod(kInfiniteCatchUpTimeout);
const bool ReplSetConfig::kDefaultChainingAllowed(true);
const Milliseconds ReplSetConfig::kDefaultCatchUpTakeoverDelay(30000);

namespace {

const std::string kStepDownCheckWriteConcernModeName = "$stepDownCheck";

bool isValidCIDRRange(StringData host) {
    return CIDR::parse(host).isOK();
}

}  // namespace

/* static */
ReplSetConfig ReplSetConfig::parse(const BSONObj& cfg,
                                   boost::optional<long long> forceTerm,
                                   OID defaultReplicaSetId) {
    return ReplSetConfig(cfg, false /* forInitiate */, forceTerm, defaultReplicaSetId);
}

/* static */
ReplSetConfig ReplSetConfig::parseForInitiate(const BSONObj& cfg, OID newReplicaSetId) {
    uassert(
        4709000, "A replica set ID must be provided to parseForInitiate", newReplicaSetId.isSet());
    auto result = ReplSetConfig(
        cfg, true /* forInitiate */, OpTime::kInitialTerm /* forceTerm*/, newReplicaSetId);
    uassert(ErrorCodes::InvalidReplicaSetConfig,
            str::stream() << "replica set configuration cannot contain '"
                          << ReplSetConfigSettings::kReplicaSetIdFieldName
                          << "' "
                             "field when called from replSetInitiate: "
                          << cfg,
            newReplicaSetId == result.getReplicaSetId());
    return result;
}

BSONObj ReplSetConfig::toBSON() const {
    BSONObjBuilder builder;
    serialize(&builder);

    if (_recipientConfig) {
        builder.append(kRecipientConfigFieldName, _recipientConfig->toBSON());
    }

    return builder.obj();
}

void ReplSetConfig::_setRequiredFields() {
    // The three required fields need to be set to something valid to avoid a potential
    // invariant if the uninitialized object is ever used with toBSON().
    if (getReplSetName().empty())
        setReplSetName("INVALID");
    if (getConfigVersion() == -1)
        setConfigVersion(2147483647);
    if (getMembers().empty())
        setMembers({});
}

ReplSetConfig::ReplSetConfig(MutableReplSetConfig&& base)
    : MutableReplSetConfig(std::move(base)), _isInitialized(true) {
    uassertStatusOK(_initialize(false, boost::none, OID()));
}

ReplSetConfig::ReplSetConfig(const BSONObj& cfg,
                             bool forInitiate,
                             boost::optional<long long> forceTerm,
                             OID defaultReplicaSetId)
    : _isInitialized(true) {
    // The settings field is optional, but we always serialize it.  Because we can't default it in
    // the IDL, we default it here.
    setSettings(ReplSetConfigSettings());
    ReplSetConfigBase::parseProtected(IDLParserContext("ReplSetConfig"), cfg);
    uassertStatusOK(_initialize(forInitiate, forceTerm, defaultReplicaSetId));

    if (cfg.hasField(kRecipientConfigFieldName)) {
        auto splitConfig = cfg[kRecipientConfigFieldName].Obj();
        _recipientConfig.reset(new ReplSetConfig(
            splitConfig, false /* forInitiate */, forceTerm, defaultReplicaSetId));
    }
}

Status ReplSetConfig::_initialize(bool forInitiate,
                                  boost::optional<long long> forceTerm,
                                  OID defaultReplicaSetId) {
    if (getRepaired()) {
        return {ErrorCodes::RepairedReplicaSetNode, "Replicated data has been repaired"};
    }
    Status status(Status::OK());
    if (forceTerm != boost::none) {
        // Set term to the value explicitly passed in.
        setConfigTerm(*forceTerm);
    }

    //
    // Add tag data from members
    //
    for (auto&& member : getMembers()) {
        // The const_cast is necessary because "non_const_getter" in the IDL doesn't work for
        // arrays.
        const_cast<MemberConfig&>(member).addTagInfo(&_tagConfig);

        setSecondaryDelaySecsFieldDefault(member.getId());
    }

    //
    // Initialize configServer
    //
    if (forInitiate && serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        !getConfigServer().has_value()) {
        setConfigServer(true);
    }

    //
    // Put getLastErrorModes into the tag configuration.
    //
    auto modesStatus = getSettings()->getGetLastErrorModes().convertToTagPatternMap(&_tagConfig);
    if (!modesStatus.isOK()) {
        return modesStatus.getStatus();
    }
    _customWriteConcernModes = std::move(modesStatus.getValue());

    if (!getSettings()->getReplicaSetId() && defaultReplicaSetId.isSet()) {
        auto settings = *getSettings();
        settings.setReplicaSetId(defaultReplicaSetId);
        setSettings(settings);
    }

    _calculateMajorities();
    _addInternalWriteConcernModes();
    _initializeConnectionString();
    return Status::OK();
}

Status ReplSetConfig::validate() const {
    return _validate(false);
}

Status ReplSetConfig::validateAllowingSplitHorizonIP() const {
    return _validate(true);
}

Status ReplSetConfig::_validate(bool allowSplitHorizonIP) const {
    if (getMembers().size() > kMaxMembers || getMembers().empty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Replica set configuration contains " << getMembers().size()
                                    << " members, but must have at least 1 and no more than  "
                                    << kMaxMembers);
    }

    size_t localhostCount = 0;
    size_t voterCount = 0;
    size_t arbiterCount = 0;
    size_t electableCount = 0;

    auto extractHorizonMembers = [](const auto& replMember) {
        std::vector<std::string> rv;
        std::transform(replMember.getHorizonMappings().begin(),
                       replMember.getHorizonMappings().end(),
                       back_inserter(rv),
                       [](auto&& mapping) { return mapping.first; });
        std::sort(begin(rv), end(rv));
        return rv;
    };

    const auto expectedHorizonNameMapping = extractHorizonMembers(getMembers()[0]);

    stdx::unordered_set<std::string> nonUniversalHorizons;
    std::map<HostAndPort, int> horizonHostNameCounts;
    for (size_t i = 0; i < getMembers().size(); ++i) {
        const MemberConfig& memberI = getMembers()[i];

        // Check that no horizon mappings contain IP addresses
        if (!disableSplitHorizonIPCheck && !allowSplitHorizonIP) {
            for (auto&& mapping : memberI.getHorizonMappings()) {
                // Ignore the default horizon -- this can be an IP
                if (mapping.first == SplitHorizon::kDefaultHorizon) {
                    continue;
                }

                // Anything which can be parsed as a valid CIDR range will cause failure
                if (isValidCIDRRange(mapping.second.host())) {
                    return Status(ErrorCodes::UnsupportedFormat,
                                  str::stream() << "Found split horizon configuration using IP "
                                                   "address, which is disallowed: "
                                                << kMembersFieldName << "." << i << "."
                                                << MemberConfig::kHorizonsFieldName
                                                << " contains entry {\"" << mapping.first
                                                << "\": \"" << mapping.second.toString() << "\"}");
                }
            }
        }

        // Check the replica set configuration for errors in horizon specification:
        //   * Check that all members have the same set of horizon names
        //   * Check that no hostname:port appears more than once for any member
        //   * Check that all hostname:port endpoints are unique for all members
        const auto seenHorizonNameMapping = extractHorizonMembers(memberI);

        if (expectedHorizonNameMapping != seenHorizonNameMapping) {
            // Collect a list of horizons only seen on one side of the pair of horizon maps
            // considered.  Names that are only on one side are non-universal, and should be
            // reported -- the same set of horizon names must exist across all replica set members.
            // We collect the list while parsing over ALL members, this way we can report all
            // horizons which are not universally listed in the replica set configuration in a
            // single error message.
            std::set_symmetric_difference(
                begin(expectedHorizonNameMapping),
                end(expectedHorizonNameMapping),
                begin(seenHorizonNameMapping),
                end(seenHorizonNameMapping),
                inserter(nonUniversalHorizons, end(nonUniversalHorizons)));
        } else {
            // Because "__default" always lives in the mappings, we don't have to get it separately
            for (const auto& mapping : memberI.getHorizonMappings()) {
                ++horizonHostNameCounts[mapping.second];
            }
        }

        if (memberI.getHostAndPort().isLocalHost()) {
            ++localhostCount;
        }
        if (memberI.isVoter()) {
            ++voterCount;
        }
        // Nodes may be arbiters or electable, or neither, but never both.
        if (memberI.isArbiter()) {
            ++arbiterCount;
        } else if (memberI.getPriority() > 0) {
            ++electableCount;
        }
        for (size_t j = 0; j < getMembers().size(); ++j) {
            if (i == j)
                continue;
            const MemberConfig& memberJ = getMembers()[j];
            if (memberI.getId() == memberJ.getId()) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << "Found two member configurations with same "
                                  << MemberConfig::kIdFieldName << " field, " << kMembersFieldName
                                  << "." << i << "." << MemberConfig::kIdFieldName
                                  << " == " << kMembersFieldName << "." << j << "."
                                  << MemberConfig::kIdFieldName << " == " << memberI.getId());
            }
            if (memberI.getHostAndPort() == memberJ.getHostAndPort()) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << "Found two member configurations with same "
                                  << MemberConfig::kHostFieldName << " field, " << kMembersFieldName
                                  << "." << i << "." << MemberConfig::kHostFieldName
                                  << " == " << kMembersFieldName << "." << j << "."
                                  << MemberConfig::kHostFieldName
                                  << " == " << memberI.getHostAndPort().toString());
            }
        }
    }

    // If we found horizons that weren't universally present, list all non-universally present
    // horizons for this replica set.
    if (!nonUniversalHorizons.empty()) {
        const auto missingHorizonList = [&] {
            std::string rv;
            for (const auto& horizonName : nonUniversalHorizons) {
                rv += " " + horizonName + ",";
            }
            rv.pop_back();
            return rv;
        }();
        return Status(ErrorCodes::BadValue,
                      "Saw a replica set member with a different horizon mapping than the "
                      "others.  The following horizons were not universally present:" +
                          missingHorizonList);
    }

    const auto nonUniqueHostNameList = [&] {
        std::vector<HostAndPort> rv;
        for (const auto& host : horizonHostNameCounts) {
            if (host.second > 1)
                rv.push_back(host.first);
        }
        return rv;
    }();

    if (!nonUniqueHostNameList.empty()) {
        const auto nonUniqueHostNames = [&] {
            std::string rv;
            for (const auto& hostName : nonUniqueHostNameList) {
                rv += " " + hostName.toString() + ",";
            }
            rv.pop_back();
            return rv;
        }();
        return Status(ErrorCodes::BadValue,
                      "The following hostnames are not unique across all horizons and host "
                      "specifications in the replica set:" +
                          nonUniqueHostNames);
    }


    if (localhostCount != 0 && localhostCount != getMembers().size()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Either all host names in a replica set configuration must be localhost "
                   "references, or none must be; found "
                << localhostCount << " out of " << getMembers().size());
    }

    if (voterCount > kMaxVotingMembers || voterCount == 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Replica set configuration contains " << voterCount
                                    << " voting members, but must be at least 1 and no more than "
                                    << kMaxVotingMembers);
    }

    if (electableCount == 0) {
        return Status(ErrorCodes::BadValue,
                      "Replica set configuration must contain at least "
                      "one non-arbiter member with priority > 0");
    }

    if (getConfigServer()) {
        if (arbiterCount > 0) {
            return Status(ErrorCodes::BadValue,
                          "Arbiters are not allowed in replica set configurations being used for "
                          "config servers");
        }
        for (MemberIterator mem = membersBegin(); mem != membersEnd(); mem++) {
            if (!mem->shouldBuildIndexes()) {
                return Status(ErrorCodes::BadValue,
                              "Members in replica set configurations being used for config "
                              "servers must build indexes");
            }
            if (mem->getSecondaryDelay() != Seconds(0)) {
                return Status(ErrorCodes::BadValue,
                              "Members in replica set configurations being used for config "
                              "servers cannot have a non-zero secondaryDelaySecs");
            }
        }
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer &&
            !skipShardingConfigurationChecks) {
            return Status(ErrorCodes::BadValue,
                          "Nodes being used for config servers must be started with the "
                          "--configsvr flag");
        }
        if (!getWriteConcernMajorityShouldJournal()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << kWriteConcernMajorityShouldJournalFieldName
                                        << " must be true in replica set configurations being "
                                           "used for config servers");
        }
    } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return Status(ErrorCodes::BadValue,
                      "Nodes started with the --configsvr flag must have configsvr:true in "
                      "their config");
    }

    if (!allowMultipleArbiters && arbiterCount > 1) {
        return Status(ErrorCodes::BadValue,
                      "Multiple arbiters are not allowed unless all nodes were started with "
                      "with --setParameter 'allowMultipleArbiters=true'");
    }

    if (!_connectionString.isValid()) {
        return Status(ErrorCodes::BadValue,
                      "ReplSetConfig represented an invalid replica set ConnectionString");
    }

    return Status::OK();
}

Status ReplSetConfig::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions& writeConcern) const {
    if (auto wNumNodes = stdx::get_if<int64_t>(&writeConcern.w)) {
        if (*wNumNodes > getNumDataBearingMembers()) {
            return Status(ErrorCodes::UnsatisfiableWriteConcern, "Not enough data-bearing nodes");
        }

        return Status::OK();
    }

    StatusWith<ReplSetTagPattern> tagPatternStatus = [&]() {
        auto wMode = stdx::get_if<std::string>(&writeConcern.w);
        return wMode ? findCustomWriteMode(*wMode)
                     : makeCustomWriteMode(stdx::get<WTags>(writeConcern.w));
    }();

    if (!tagPatternStatus.isOK()) {
        return tagPatternStatus.getStatus();
    }

    ReplSetTagMatch matcher(tagPatternStatus.getValue());
    for (size_t j = 0; j < getMembers().size(); ++j) {
        const MemberConfig& memberConfig = getMembers()[j];
        for (MemberConfig::TagIterator it = memberConfig.tagsBegin(); it != memberConfig.tagsEnd();
             ++it) {
            if (matcher.update(*it)) {
                return Status::OK();
            }
        }
    }

    // Even if all the nodes in the set had a given write it still would not satisfy this
    // write concern mode.
    auto wModeForError = [&]() {
        auto wMode = stdx::get_if<std::string>(&writeConcern.w);
        return wMode ? fmt::format("\"{}\"", *wMode)
                     : fmt::format("{}", stdx::get<WTags>(writeConcern.w));
    }();

    return Status(ErrorCodes::UnsatisfiableWriteConcern,
                  str::stream() << "Not enough nodes match write concern mode \"" << wModeForError
                                << "\"");
}

int ReplSetConfig::getNumDataBearingMembers() const {
    int numArbiters = std::count_if(
        begin(getMembers()), end(getMembers()), [](const auto& x) { return x.isArbiter(); });
    return getMembers().size() - numArbiters;
}

const MemberConfig& ReplSetConfig::getMemberAt(size_t i) const {
    invariant(i < getMembers().size());
    return getMembers()[i];
}

const MemberConfig* ReplSetConfig::findMemberByID(int id) const {
    for (std::vector<MemberConfig>::const_iterator it = getMembers().begin();
         it != getMembers().end();
         ++it) {
        if (it->getId() == MemberId(id)) {
            return &(*it);
        }
    }
    return nullptr;
}

int ReplSetConfig::findMemberIndexByHostAndPort(const HostAndPort& hap) const {
    int x = 0;
    for (std::vector<MemberConfig>::const_iterator it = getMembers().begin();
         it != getMembers().end();
         ++it) {
        if (it->getHostAndPort() == hap) {
            return x;
        }
        ++x;
    }
    return -1;
}

int ReplSetConfig::findMemberIndexByConfigId(int configId) const {
    int x = 0;
    for (const auto& member : getMembers()) {
        if (member.getId() == MemberId(configId)) {
            return x;
        }
        ++x;
    }
    return -1;
}

const MemberConfig* ReplSetConfig::findMemberByHostAndPort(const HostAndPort& hap) const {
    int idx = findMemberIndexByHostAndPort(hap);
    return idx != -1 ? &getMemberAt(idx) : nullptr;
}

Milliseconds ReplSetConfig::getHeartbeatInterval() const {
    auto heartbeatInterval = Milliseconds(getSettings()->getHeartbeatIntervalMillis());
    forceHeartbeatIntervalMS.execute([&](const BSONObj& data) {
        auto intervalMS = data["intervalMS"].numberInt();
        heartbeatInterval = Milliseconds(intervalMS);
    });
    return heartbeatInterval;
}

bool ReplSetConfig::isLocalHostAllowed() const {
    // It is sufficient to check any one member's hostname, since in ReplSetConfig::validate,
    // it's ensured that either all members have hostname localhost or none do.
    return getMembers().begin()->getHostAndPort().isLocalHost();
}

ReplSetTag ReplSetConfig::findTag(StringData key, StringData value) const {
    return _tagConfig.findTag(key, value);
}

StatusWith<ReplSetTagPattern> ReplSetConfig::findCustomWriteMode(StringData patternName) const {
    // The string "majority" corresponds to the internal "$majority" custom write mode
    if (patternName == WriteConcernOptions::kMajority) {
        patternName = kMajorityWriteConcernModeName;
    }

    const StringMap<ReplSetTagPattern>::const_iterator iter =
        _customWriteConcernModes.find(patternName);
    if (iter == _customWriteConcernModes.end()) {
        return StatusWith<ReplSetTagPattern>(
            ErrorCodes::UnknownReplWriteConcern,
            "No write concern mode named '{}' found in replica set configuration"_format(
                str::escape(patternName.toString())));
    }
    return StatusWith<ReplSetTagPattern>(iter->second);
}

StatusWith<ReplSetTagPattern> ReplSetConfig::makeCustomWriteMode(const WTags& wTags) const {
    ReplSetTagPattern pattern = _tagConfig.makePattern();
    for (const auto& [tagName, minNodesWithTag] : wTags) {
        auto status = _tagConfig.addTagCountConstraintToPattern(&pattern, tagName, minNodesWithTag);
        if (!status.isOK()) {
            return status;
        }
    }

    return pattern;
}

void ReplSetConfig::_calculateMajorities() {
    const int voters = std::count_if(
        begin(getMembers()), end(getMembers()), [](const auto& x) { return x.isVoter(); });
    const int arbiters = std::count_if(
        begin(getMembers()), end(getMembers()), [](const auto& x) { return x.isArbiter(); });
    _totalVotingMembers = voters;
    _majorityVoteCount = voters / 2 + 1;
    _writableVotingMembersCount = voters - arbiters;
    _writeMajority = std::min(_majorityVoteCount, _writableVotingMembersCount);
}

void ReplSetConfig::_addInternalWriteConcernModes() {
    // $majority: the majority of voting nodes or all non-arbiter voting nodes if
    // the majority of voting nodes are arbiters.
    ReplSetTagPattern pattern = _tagConfig.makePattern();

    Status status = _tagConfig.addTagCountConstraintToPattern(
        &pattern, MemberConfig::kInternalVoterTagName, _writeMajority);

    if (status.isOK()) {
        _customWriteConcernModes[kMajorityWriteConcernModeName] = pattern;
    } else if (status != ErrorCodes::NoSuchKey) {
        // NoSuchKey means we have no $voter-tagged nodes in this config;
        // other errors are unexpected.
        fassert(28693, status);
    }

    // $votingMembers: all voting data-bearing nodes.
    pattern = _tagConfig.makePattern();
    status = _tagConfig.addTagCountConstraintToPattern(
        &pattern, MemberConfig::kInternalVoterTagName, _writableVotingMembersCount);

    if (status.isOK()) {
        _customWriteConcernModes[kVotingMembersWriteConcernModeName] = pattern;
    } else if (status != ErrorCodes::NoSuchKey) {
        // NoSuchKey means we have no $voter-tagged nodes in this config;
        // other errors are unexpected.
        fassert(4671203, status);
    }

    // $stepDownCheck: one electable node plus ourselves
    pattern = _tagConfig.makePattern();
    status = _tagConfig.addTagCountConstraintToPattern(
        &pattern, MemberConfig::kInternalElectableTagName, 2);
    if (status.isOK()) {
        _customWriteConcernModes[kStepDownCheckWriteConcernModeName] = pattern;
    } else if (status != ErrorCodes::NoSuchKey) {
        // NoSuchKey means we have no $electable-tagged nodes in this config;
        // other errors are unexpected
        fassert(28694, status);
    }

    // $majorityConfig: the majority of all voting members including arbiters.
    pattern = _tagConfig.makePattern();
    status = _tagConfig.addTagCountConstraintToPattern(
        &pattern, MemberConfig::kConfigVoterTagName, _majorityVoteCount / 2 + 1);
    if (status.isOK()) {
        _customWriteConcernModes[kConfigMajorityWriteConcernModeName] = pattern;
    } else if (status != ErrorCodes::NoSuchKey) {
        // NoSuchKey means we have no $configAll-tagged nodes in this config;
        // other errors are unexpected
        fassert(31472, status);
    }

    // $configAll: all members including arbiters.
    pattern = _tagConfig.makePattern();
    status = _tagConfig.addTagCountConstraintToPattern(
        &pattern, MemberConfig::kConfigAllTagName, getMembers().size());
    if (status.isOK()) {
        _customWriteConcernModes[kConfigAllWriteConcernName] = pattern;
    } else if (status != ErrorCodes::NoSuchKey) {
        // NoSuchKey means we have no $all-tagged nodes in this config;
        // other errors are unexpected
        fassert(31473, status);
    }
}

void ReplSetConfig::_initializeConnectionString() {
    std::vector<HostAndPort> visibleMembers;
    for (const auto& member : getMembers()) {
        if (!member.isHidden() && !member.isArbiter()) {
            visibleMembers.push_back(member.getHostAndPort());
        }
    }

    try {
        _connectionString = ConnectionString::forReplicaSet(getReplSetName(), visibleMembers);
    } catch (const DBException& e) {
        invariant(e.code() == ErrorCodes::FailedToParse);
        // Failure to construct the ConnectionString means either an invalid replica set name
        // or members array, which should be caught in validate()
    }
}

std::vector<std::string> ReplSetConfig::getWriteConcernNames() const {
    std::vector<std::string> names;
    for (StringMap<ReplSetTagPattern>::const_iterator mode = _customWriteConcernModes.begin();
         mode != _customWriteConcernModes.end();
         ++mode) {
        names.push_back(mode->first);
    }
    return names;
}

BSONObj ReplSetConfig::toBSONWithoutNewlyAdded() const {
    // This takes the toBSON() output and makes a new copy without the newlyAdded field, by
    // re-serializing the member array.  So it is not too efficient, but this object isn't
    // very big and this method not used too often.
    auto obj = toBSON();
    BSONObjBuilder bob;
    BSONObjIterator it(obj);
    while (it.more()) {
        BSONElement e = it.next();
        if (e.fieldName() == kMembersFieldName) {
            BSONArrayBuilder memberBuilder(bob.subarrayStart(kMembersFieldName));
            for (auto&& member : getMembers())
                memberBuilder.append(member.toBSON(true /* omitNewlyAddedField */));
            memberBuilder.done();
            continue;
        }
        bob.append(e);
    }
    return bob.obj();
}

Milliseconds ReplSetConfig::getPriorityTakeoverDelay(int memberIdx) const {
    auto member = getMemberAt(memberIdx);
    int priorityRank = calculatePriorityRank(member.getPriority());
    return (priorityRank + 1) * getElectionTimeoutPeriod();
}

int ReplSetConfig::calculatePriorityRank(double priority) const {
    int count = 0;
    for (MemberIterator mem = membersBegin(); mem != membersEnd(); mem++) {
        if (mem->getPriority() > priority) {
            count++;
        }
    }
    return count;
}

bool ReplSetConfig::containsArbiter() const {
    for (MemberIterator mem = membersBegin(); mem != membersEnd(); mem++) {
        if (mem->isArbiter()) {
            return true;
        }
    }
    return false;
}

MutableReplSetConfig ReplSetConfig::getMutable() const {
    return *static_cast<const MutableReplSetConfig*>(this);
}

bool ReplSetConfig::isImplicitDefaultWriteConcernMajority() const {
    // Only set defaultWC to majority when writable voting members are strictly more than voting
    // majority. This will prevent arbiters from keeping the primary elected while no majority write
    // can be fulfilled.
    auto arbiters = _totalVotingMembers - _writableVotingMembersCount;
    return arbiters == 0 || _writableVotingMembersCount > _majorityVoteCount;
}

bool ReplSetConfig::containsCustomizedGetLastErrorDefaults() const {
    // Since the ReplSetConfig always has a WriteConcernOptions, the only way to know if it has been
    // customized through getLastErrorDefaults is if it's different from { w: 1, wtimeout: 0 }.
    const auto& getLastErrorDefaults = getDefaultWriteConcern();
    if (auto wNumNodes = stdx::get_if<int64_t>(&getLastErrorDefaults.w);
        !wNumNodes || *wNumNodes != 1)
        return true;
    if (getLastErrorDefaults.wTimeout != Milliseconds::zero())
        return true;
    if (getLastErrorDefaults.syncMode != WriteConcernOptions::SyncMode::UNSET)
        return true;
    return false;
}

Status ReplSetConfig::validateWriteConcern(const WriteConcernOptions& writeConcern) const {
    if (writeConcern.hasCustomWriteMode()) {
        return findCustomWriteMode(stdx::get<std::string>(writeConcern.w)).getStatus();
    }
    return Status::OK();
}

bool ReplSetConfig::isSplitConfig() const {
    return !!_recipientConfig;
}

ReplSetConfigPtr ReplSetConfig::getRecipientConfig() const {
    return _recipientConfig;
}

bool ReplSetConfig::areWriteConcernModesTheSame(ReplSetConfig* otherConfig) const {
    auto modeNames = getWriteConcernNames();
    auto otherModeNames = otherConfig->getWriteConcernNames();

    if (modeNames.size() != otherModeNames.size()) {
        return false;
    }

    for (auto it = modeNames.begin(); it != modeNames.end(); it++) {
        auto swPatternA = findCustomWriteMode(*it);
        auto swPatternB = otherConfig->findCustomWriteMode(*it);
        if (!swPatternA.isOK() || !swPatternB.isOK()) {
            return false;
        }

        if (swPatternA.getValue() != swPatternB.getValue()) {
            return false;
        }
    }

    return true;
}

MemberConfig* MutableReplSetConfig::_findMemberByID(MemberId id) {
    for (auto it = getMembers().begin(); it != getMembers().end(); ++it) {
        if (it->getId() == id) {
            return const_cast<MemberConfig*>(&(*it));
        }
    }
    LOGV2_FATAL(4709100, "Unable to find member", "id"_attr = id);
}

void MutableReplSetConfig::addNewlyAddedFieldForMember(MemberId memberId) {
    _findMemberByID(memberId)->setNewlyAdded(true);
}

void MutableReplSetConfig::removeNewlyAddedFieldForMember(MemberId memberId) {
    _findMemberByID(memberId)->setNewlyAdded(boost::none);
}

void MutableReplSetConfig::setSecondaryDelaySecsFieldDefault(MemberId memberId) {
    auto mem = _findMemberByID(memberId);
    if (mem->hasSecondaryDelaySecs()) {
        return;
    }
    mem->setSecondaryDelaySecs(0LL);
}

}  // namespace repl
}  // namespace mongo
