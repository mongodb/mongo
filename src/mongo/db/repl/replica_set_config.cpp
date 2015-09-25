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

#include "mongo/db/repl/replica_set_config.h"

#include <algorithm>

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

#ifndef _MSC_VER
const size_t ReplicaSetConfig::kMaxMembers;
const size_t ReplicaSetConfig::kMaxVotingMembers;
#endif

const std::string ReplicaSetConfig::kConfigServerFieldName = "configsvr";
const std::string ReplicaSetConfig::kVersionFieldName = "version";
const std::string ReplicaSetConfig::kMajorityWriteConcernModeName = "$majority";
const Milliseconds ReplicaSetConfig::kDefaultHeartbeatInterval(2000);
const Seconds ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod(10);
const Milliseconds ReplicaSetConfig::kDefaultElectionTimeoutPeriod(10000);
const int ReplicaSetConfig::kDefaultElectionTimeoutOffsetLimit(2000);

namespace {

const std::string kIdFieldName = "_id";
const std::string kMembersFieldName = "members";
const std::string kSettingsFieldName = "settings";
const std::string kStepDownCheckWriteConcernModeName = "$stepDownCheck";
const std::string kProtocolVersionFieldName = "protocolVersion";

const std::string kLegalConfigTopFieldNames[] = {kIdFieldName,
                                                 ReplicaSetConfig::kVersionFieldName,
                                                 kMembersFieldName,
                                                 kSettingsFieldName,
                                                 kProtocolVersionFieldName,
                                                 ReplicaSetConfig::kConfigServerFieldName};

const std::string kElectionTimeoutFieldName = "electionTimeoutMillis";
const std::string kElectionTimeoutOffsetLimitFieldName = "electionTimeoutOffsetLimitMillis";
const std::string kHeartbeatIntervalFieldName = "heartbeatIntervalMillis";
const std::string kHeartbeatTimeoutFieldName = "heartbeatTimeoutSecs";
const std::string kChainingAllowedFieldName = "chainingAllowed";
const std::string kGetLastErrorDefaultsFieldName = "getLastErrorDefaults";
const std::string kGetLastErrorModesFieldName = "getLastErrorModes";

}  // namespace

Status ReplicaSetConfig::initialize(const BSONObj& cfg, bool usePV1ByDefault) {
    _isInitialized = false;
    _members.clear();
    Status status =
        bsonCheckOnlyHasFields("replica set configuration", cfg, kLegalConfigTopFieldNames);
    if (!status.isOK())
        return status;

    //
    // Parse replSetName
    //
    status = bsonExtractStringField(cfg, kIdFieldName, &_replSetName);
    if (!status.isOK())
        return status;

    //
    // Parse version
    //
    status = bsonExtractIntegerField(cfg, kVersionFieldName, &_version);
    if (!status.isOK())
        return status;

    //
    // Parse members
    //
    BSONElement membersElement;
    status = bsonExtractTypedField(cfg, kMembersFieldName, Array, &membersElement);
    if (!status.isOK())
        return status;

    for (BSONObj::iterator membersIterator(membersElement.Obj()); membersIterator.more();) {
        BSONElement memberElement = membersIterator.next();
        if (memberElement.type() != Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "Expected type of " << kMembersFieldName << "."
                                        << memberElement.fieldName() << " to be Object, but found "
                                        << typeName(memberElement.type()));
        }
        _members.resize(_members.size() + 1);
        const auto& memberBSON = memberElement.Obj();
        status = _members.back().initialize(memberBSON, &_tagConfig);
        if (!status.isOK())
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          str::stream() << status.toString() << " for member:" << memberBSON);
    }

    //
    // Parse configServer
    //
    status = bsonExtractBooleanFieldWithDefault(
        cfg, kConfigServerFieldName, serverGlobalParams.configsvr, &_configServer);
    if (!status.isOK()) {
        return status;
    }

    //
    // Parse protocol version
    //
    status = bsonExtractIntegerField(cfg, kProtocolVersionFieldName, &_protocolVersion);
    if (!status.isOK()) {
        if (status != ErrorCodes::NoSuchKey) {
            return status;
        }

        if (usePV1ByDefault) {
            _protocolVersion = 1;
        }
    }

    //
    // Parse settings
    //
    BSONElement settingsElement;
    status = bsonExtractTypedField(cfg, kSettingsFieldName, Object, &settingsElement);
    BSONObj settings;
    if (status.isOK()) {
        settings = settingsElement.Obj();
    } else if (status != ErrorCodes::NoSuchKey) {
        return status;
    }
    status = _parseSettingsSubdocument(settings);
    if (!status.isOK())
        return status;

    _calculateMajorities();
    _addInternalWriteConcernModes();
    _isInitialized = true;
    return Status::OK();
}

Status ReplicaSetConfig::_parseSettingsSubdocument(const BSONObj& settings) {
    //
    // Parse heartbeatIntervalMillis
    //
    long long heartbeatIntervalMillis;
    Status hbIntervalStatus =
        bsonExtractIntegerFieldWithDefault(settings,
                                           kHeartbeatIntervalFieldName,
                                           durationCount<Milliseconds>(kDefaultHeartbeatInterval),
                                           &heartbeatIntervalMillis);
    if (!hbIntervalStatus.isOK()) {
        return hbIntervalStatus;
    }
    _heartbeatInterval = Milliseconds(heartbeatIntervalMillis);

    //
    // Parse electionTimeoutMillis
    //
    BSONElement electionTimeoutMillisElement = settings[kElectionTimeoutFieldName];
    if (electionTimeoutMillisElement.eoo()) {
        _electionTimeoutPeriod = Milliseconds(kDefaultElectionTimeoutPeriod);
    } else if (electionTimeoutMillisElement.isNumber()) {
        _electionTimeoutPeriod = Milliseconds(electionTimeoutMillisElement.numberInt());
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected type of " << kSettingsFieldName << "."
                                    << kElectionTimeoutFieldName
                                    << " to be a number, but found a value of type "
                                    << typeName(electionTimeoutMillisElement.type()));
    }

    //
    // Parse electionTimeoutOffsetLimit
    //
    BSONElement electionTimeoutOffsetLimitElement = settings[kElectionTimeoutOffsetLimitFieldName];
    if (electionTimeoutOffsetLimitElement.eoo()) {
        _electionTimeoutOffsetLimit = kDefaultElectionTimeoutOffsetLimit;
    } else if (electionTimeoutOffsetLimitElement.isNumber()) {
        _electionTimeoutOffsetLimit = electionTimeoutOffsetLimitElement.numberInt();
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected type of " << kSettingsFieldName << "."
                                    << kElectionTimeoutOffsetLimitFieldName
                                    << " to be a number, but found a value of type "
                                    << typeName(electionTimeoutOffsetLimitElement.type()));
    }

    //
    // Parse heartbeatTimeoutSecs
    //
    BSONElement hbTimeoutSecsElement = settings[kHeartbeatTimeoutFieldName];
    if (hbTimeoutSecsElement.eoo()) {
        _heartbeatTimeoutPeriod = Seconds(kDefaultHeartbeatTimeoutPeriod);
    } else if (hbTimeoutSecsElement.isNumber()) {
        _heartbeatTimeoutPeriod = Seconds(hbTimeoutSecsElement.numberInt());
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "Expected type of " << kSettingsFieldName << "."
                                    << kHeartbeatTimeoutFieldName
                                    << " to be a number, but found a value of type "
                                    << typeName(hbTimeoutSecsElement.type()));
    }

    //
    // Parse chainingAllowed
    //
    Status status = bsonExtractBooleanFieldWithDefault(
        settings, kChainingAllowedFieldName, true, &_chainingAllowed);
    if (!status.isOK())
        return status;

    //
    // Parse getLastErrorDefaults
    //
    BSONElement gleDefaultsElement;
    status = bsonExtractTypedField(
        settings, kGetLastErrorDefaultsFieldName, Object, &gleDefaultsElement);
    if (status.isOK()) {
        status = _defaultWriteConcern.parse(gleDefaultsElement.Obj());
        if (!status.isOK())
            return status;
    } else if (status == ErrorCodes::NoSuchKey) {
        // Default write concern is w: 1.
        _defaultWriteConcern.reset();
        _defaultWriteConcern.wNumNodes = 1;
    } else {
        return status;
    }

    //
    // Parse getLastErrorModes
    //
    BSONElement gleModesElement;
    status = bsonExtractTypedField(settings, kGetLastErrorModesFieldName, Object, &gleModesElement);
    BSONObj gleModes;
    if (status.isOK()) {
        gleModes = gleModesElement.Obj();
    } else if (status != ErrorCodes::NoSuchKey) {
        return status;
    }

    for (BSONObj::iterator gleModeIter(gleModes); gleModeIter.more();) {
        const BSONElement modeElement = gleModeIter.next();
        if (_customWriteConcernModes.find(modeElement.fieldNameStringData()) !=
            _customWriteConcernModes.end()) {
            return Status(ErrorCodes::DuplicateKey,
                          str::stream() << kSettingsFieldName << '.' << kGetLastErrorModesFieldName
                                        << " contains multiple fields named "
                                        << modeElement.fieldName());
        }
        if (modeElement.type() != Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "Expected " << kSettingsFieldName << '.'
                                        << kGetLastErrorModesFieldName << '.'
                                        << modeElement.fieldName() << " to be an Object, not "
                                        << typeName(modeElement.type()));
        }
        ReplicaSetTagPattern pattern = _tagConfig.makePattern();
        for (BSONObj::iterator constraintIter(modeElement.Obj()); constraintIter.more();) {
            const BSONElement constraintElement = constraintIter.next();
            if (!constraintElement.isNumber()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "Expected " << kSettingsFieldName << '.'
                                  << kGetLastErrorModesFieldName << '.' << modeElement.fieldName()
                                  << '.' << constraintElement.fieldName() << " to be a number, not "
                                  << typeName(constraintElement.type()));
            }
            const int minCount = constraintElement.numberInt();
            if (minCount <= 0) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Value of " << kSettingsFieldName << '.'
                                            << kGetLastErrorModesFieldName << '.'
                                            << modeElement.fieldName() << '.'
                                            << constraintElement.fieldName()
                                            << " must be positive, but found " << minCount);
            }
            status = _tagConfig.addTagCountConstraintToPattern(
                &pattern, constraintElement.fieldNameStringData(), minCount);
            if (!status.isOK()) {
                return status;
            }
        }
        _customWriteConcernModes[modeElement.fieldNameStringData()] = pattern;
    }

    return Status::OK();
}

Status ReplicaSetConfig::validate() const {
    if (_version <= 0 || _version > std::numeric_limits<int>::max()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kVersionFieldName << " field value of " << _version
                                    << " is out of range");
    }
    if (_replSetName.empty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Replica set configuration must have non-empty "
                                    << kIdFieldName << " field");
    }
    if (_heartbeatInterval < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kSettingsFieldName << '.' << kHeartbeatIntervalFieldName
                                    << " field value must be non-negative, "
                                       "but found "
                                    << durationCount<Milliseconds>(_heartbeatInterval));
    }
    if (_heartbeatTimeoutPeriod < Seconds(0)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kSettingsFieldName << '.' << kHeartbeatTimeoutFieldName
                                    << " field value must be non-negative, "
                                       "but found "
                                    << durationCount<Seconds>(_heartbeatTimeoutPeriod));
    }
    if (_electionTimeoutOffsetLimit < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kSettingsFieldName << '.'
                                    << kElectionTimeoutOffsetLimitFieldName
                                    << " field value must be non-negative, "
                                       "but found " << _electionTimeoutOffsetLimit);
    }
    if (_electionTimeoutPeriod < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kSettingsFieldName << '.' << kElectionTimeoutFieldName
                                    << " field value must be non-negative, "
                                       "but found "
                                    << durationCount<Milliseconds>(_electionTimeoutPeriod));
    }
    if (_members.size() > kMaxMembers || _members.empty()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Replica set configuration contains " << _members.size()
                                    << " members, but must have at least 1 and no more than  "
                                    << kMaxMembers);
    }

    size_t localhostCount = 0;
    size_t voterCount = 0;
    size_t arbiterCount = 0;
    size_t electableCount = 0;
    for (size_t i = 0; i < _members.size(); ++i) {
        const MemberConfig& memberI = _members[i];
        Status status = memberI.validate();
        if (!status.isOK())
            return status;
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
        for (size_t j = 0; j < _members.size(); ++j) {
            if (i == j)
                continue;
            const MemberConfig& memberJ = _members[j];
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
                              str::stream() << "Found two member configurations with same "
                                            << MemberConfig::kHostFieldName << " field, "
                                            << kMembersFieldName << "." << i << "."
                                            << MemberConfig::kHostFieldName
                                            << " == " << kMembersFieldName << "." << j << "."
                                            << MemberConfig::kHostFieldName
                                            << " == " << memberI.getHostAndPort().toString());
            }
        }
    }

    if (localhostCount != 0 && localhostCount != _members.size()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Either all host names in a replica set configuration must be localhost "
                   "references, or none must be; found " << localhostCount << " out of "
                << _members.size());
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

    // TODO(schwerin): Validate satisfiability of write modes? Omitting for backwards
    // compatibility.
    if (_defaultWriteConcern.wMode.empty()) {
        if (_defaultWriteConcern.wNumNodes == 0) {
            return Status(ErrorCodes::BadValue,
                          "Default write concern mode must wait for at least 1 member");
        }
    } else {
        if (WriteConcernOptions::kMajority != _defaultWriteConcern.wMode &&
            !findCustomWriteMode(_defaultWriteConcern.wMode).isOK()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Default write concern requires undefined write mode "
                                        << _defaultWriteConcern.wMode);
        }
    }

    if (_protocolVersion < 0 || _protocolVersion > std::numeric_limits<int>::max()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kProtocolVersionFieldName << " field value of "
                                    << _protocolVersion << " is out of range");
    }

    if (_configServer) {
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
            if (mem->getSlaveDelay() != Seconds(0)) {
                return Status(ErrorCodes::BadValue,
                              "Members in replica set configurations being used for config "
                              "servers cannot have a non-zero slaveDelay");
            }
        }
        if (!serverGlobalParams.configsvr) {
            return Status(ErrorCodes::BadValue,
                          "Nodes being used for config servers must be started with the "
                          "--configsvr flag");
        }
    } else if (serverGlobalParams.configsvr) {
        return Status(ErrorCodes::BadValue,
                      "Nodes started with the --configsvr flag must have configsvr:true in "
                      "their config");
    }

    return Status::OK();
}

Status ReplicaSetConfig::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions& writeConcern) const {
    if (!writeConcern.wMode.empty() && writeConcern.wMode != WriteConcernOptions::kMajority) {
        StatusWith<ReplicaSetTagPattern> tagPatternStatus = findCustomWriteMode(writeConcern.wMode);
        if (!tagPatternStatus.isOK()) {
            return tagPatternStatus.getStatus();
        }

        ReplicaSetTagMatch matcher(tagPatternStatus.getValue());
        for (size_t j = 0; j < _members.size(); ++j) {
            const MemberConfig& memberConfig = _members[j];
            for (MemberConfig::TagIterator it = memberConfig.tagsBegin();
                 it != memberConfig.tagsEnd();
                 ++it) {
                if (matcher.update(*it)) {
                    return Status::OK();
                }
            }
        }
        // Even if all the nodes in the set had a given write it still would not satisfy this
        // write concern mode.
        return Status(ErrorCodes::CannotSatisfyWriteConcern,
                      str::stream() << "Not enough nodes match write concern mode \""
                                    << writeConcern.wMode << "\"");
    } else {
        int nodesRemaining = writeConcern.wNumNodes;
        for (size_t j = 0; j < _members.size(); ++j) {
            if (!_members[j].isArbiter()) {  // Only count data-bearing nodes
                --nodesRemaining;
                if (nodesRemaining <= 0) {
                    return Status::OK();
                }
            }
        }
        return Status(ErrorCodes::CannotSatisfyWriteConcern, "Not enough data-bearing nodes");
    }
}

const MemberConfig& ReplicaSetConfig::getMemberAt(size_t i) const {
    invariant(i < _members.size());
    return _members[i];
}

const MemberConfig* ReplicaSetConfig::findMemberByID(int id) const {
    for (std::vector<MemberConfig>::const_iterator it = _members.begin(); it != _members.end();
         ++it) {
        if (it->getId() == id) {
            return &(*it);
        }
    }
    return NULL;
}

const int ReplicaSetConfig::findMemberIndexByHostAndPort(const HostAndPort& hap) const {
    int x = 0;
    for (std::vector<MemberConfig>::const_iterator it = _members.begin(); it != _members.end();
         ++it) {
        if (it->getHostAndPort() == hap) {
            return x;
        }
        ++x;
    }
    return -1;
}

const int ReplicaSetConfig::findMemberIndexByConfigId(long long configId) const {
    int x = 0;
    for (const auto& member : _members) {
        if (member.getId() == configId) {
            return x;
        }
        ++x;
    }
    return -1;
}

const MemberConfig* ReplicaSetConfig::findMemberByHostAndPort(const HostAndPort& hap) const {
    int idx = findMemberIndexByHostAndPort(hap);
    return idx != -1 ? &getMemberAt(idx) : NULL;
}

Milliseconds ReplicaSetConfig::getHeartbeatInterval() const {
    return _heartbeatInterval;
}

ReplicaSetTag ReplicaSetConfig::findTag(StringData key, StringData value) const {
    return _tagConfig.findTag(key, value);
}

StatusWith<ReplicaSetTagPattern> ReplicaSetConfig::findCustomWriteMode(
    StringData patternName) const {
    const StringMap<ReplicaSetTagPattern>::const_iterator iter =
        _customWriteConcernModes.find(patternName);
    if (iter == _customWriteConcernModes.end()) {
        return StatusWith<ReplicaSetTagPattern>(
            ErrorCodes::UnknownReplWriteConcern,
            str::stream() << "No write concern mode named '" << escape(patternName.toString())
                          << "' found in replica set configuration");
    }
    return StatusWith<ReplicaSetTagPattern>(iter->second);
}

void ReplicaSetConfig::_calculateMajorities() {
    const int voters = std::count_if(_members.begin(),
                                     _members.end(),
                                     stdx::bind(&MemberConfig::isVoter, stdx::placeholders::_1));
    const int arbiters =
        std::count_if(_members.begin(),
                      _members.end(),
                      stdx::bind(&MemberConfig::isArbiter, stdx::placeholders::_1));
    _totalVotingMembers = voters;
    _majorityVoteCount = voters / 2 + 1;
    _writeMajority = std::min(_majorityVoteCount, voters - arbiters);
}

void ReplicaSetConfig::_addInternalWriteConcernModes() {
    // $majority: the majority of voting nodes or all non-arbiter voting nodes if
    // the majority of voting nodes are arbiters.
    ReplicaSetTagPattern pattern = _tagConfig.makePattern();

    Status status = _tagConfig.addTagCountConstraintToPattern(
        &pattern, MemberConfig::kInternalVoterTagName, _writeMajority);

    if (status.isOK()) {
        _customWriteConcernModes[kMajorityWriteConcernModeName] = pattern;
    } else if (status != ErrorCodes::NoSuchKey) {
        // NoSuchKey means we have no $voter-tagged nodes in this config;
        // other errors are unexpected.
        fassert(28693, status);
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
}

BSONObj ReplicaSetConfig::toBSON() const {
    BSONObjBuilder configBuilder;
    configBuilder.append(kIdFieldName, _replSetName);
    configBuilder.appendIntOrLL(kVersionFieldName, _version);
    if (_configServer) {
        // Only include "configsvr" field if true
        configBuilder.append(kConfigServerFieldName, _configServer);
    }

    if (_protocolVersion > 0) {
        configBuilder.append(kProtocolVersionFieldName, _protocolVersion);
    }

    BSONArrayBuilder members(configBuilder.subarrayStart(kMembersFieldName));
    for (MemberIterator mem = membersBegin(); mem != membersEnd(); mem++) {
        members.append(mem->toBSON(getTagConfig()));
    }
    members.done();

    BSONObjBuilder settingsBuilder(configBuilder.subobjStart(kSettingsFieldName));
    settingsBuilder.append(kChainingAllowedFieldName, _chainingAllowed);
    settingsBuilder.appendIntOrLL(kHeartbeatIntervalFieldName,
                                  durationCount<Milliseconds>(_heartbeatInterval));
    settingsBuilder.appendIntOrLL(kHeartbeatTimeoutFieldName,
                                  durationCount<Seconds>(_heartbeatTimeoutPeriod));
    settingsBuilder.appendIntOrLL(kElectionTimeoutFieldName,
                                  durationCount<Milliseconds>(_electionTimeoutPeriod));
    settingsBuilder.appendIntOrLL(kElectionTimeoutOffsetLimitFieldName,
                                  _electionTimeoutOffsetLimit);


    BSONObjBuilder gleModes(settingsBuilder.subobjStart(kGetLastErrorModesFieldName));
    for (StringMap<ReplicaSetTagPattern>::const_iterator mode = _customWriteConcernModes.begin();
         mode != _customWriteConcernModes.end();
         ++mode) {
        if (mode->first[0] == '$') {
            // Filter out internal modes
            continue;
        }
        BSONObjBuilder modeBuilder(gleModes.subobjStart(mode->first));
        for (ReplicaSetTagPattern::ConstraintIterator itr = mode->second.constraintsBegin();
             itr != mode->second.constraintsEnd();
             itr++) {
            modeBuilder.append(_tagConfig.getTagKey(ReplicaSetTag(itr->getKeyIndex(), 0)),
                               itr->getMinCount());
        }
        modeBuilder.done();
    }
    gleModes.done();

    settingsBuilder.append(kGetLastErrorDefaultsFieldName, _defaultWriteConcern.toBSON());
    settingsBuilder.done();
    return configBuilder.obj();
}

std::vector<std::string> ReplicaSetConfig::getWriteConcernNames() const {
    std::vector<std::string> names;
    for (StringMap<ReplicaSetTagPattern>::const_iterator mode = _customWriteConcernModes.begin();
         mode != _customWriteConcernModes.end();
         ++mode) {
        names.push_back(mode->first);
    }
    return names;
}

Milliseconds ReplicaSetConfig::getPriorityTakeoverDelay(int memberIdx) const {
    auto member = getMemberAt(memberIdx);
    int priorityRank = _calculatePriorityRank(member.getPriority());
    return (priorityRank + 1) * getElectionTimeoutPeriod();
}

int ReplicaSetConfig::_calculatePriorityRank(int priority) const {
    int count = 0;
    for (MemberIterator mem = membersBegin(); mem != membersEnd(); mem++) {
        if (mem->getPriority() > priority) {
            count++;
        }
    }
    return count;
}

}  // namespace repl
}  // namespace mongo
