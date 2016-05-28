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

const size_t ReplicaSetConfig::kMaxMembers;
const size_t ReplicaSetConfig::kMaxVotingMembers;

const std::string ReplicaSetConfig::kConfigServerFieldName = "configsvr";
const std::string ReplicaSetConfig::kVersionFieldName = "version";
const std::string ReplicaSetConfig::kMajorityWriteConcernModeName = "$majority";
const Milliseconds ReplicaSetConfig::kDefaultHeartbeatInterval(2000);
const Seconds ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod(10);
const Milliseconds ReplicaSetConfig::kDefaultElectionTimeoutPeriod(10000);
const bool ReplicaSetConfig::kDefaultChainingAllowed(true);

namespace {

const std::string kIdFieldName = "_id";
const std::string kMembersFieldName = "members";
const std::string kSettingsFieldName = "settings";
const std::string kStepDownCheckWriteConcernModeName = "$stepDownCheck";
const std::string kProtocolVersionFieldName = "protocolVersion";
const std::string kWriteConcernMajorityJournalDefaultFieldName =
    "writeConcernMajorityJournalDefault";

const std::string kLegalConfigTopFieldNames[] = {kIdFieldName,
                                                 ReplicaSetConfig::kVersionFieldName,
                                                 kMembersFieldName,
                                                 kSettingsFieldName,
                                                 kProtocolVersionFieldName,
                                                 ReplicaSetConfig::kConfigServerFieldName,
                                                 kWriteConcernMajorityJournalDefaultFieldName};

const std::string kChainingAllowedFieldName = "chainingAllowed";
const std::string kElectionTimeoutFieldName = "electionTimeoutMillis";
const std::string kGetLastErrorDefaultsFieldName = "getLastErrorDefaults";
const std::string kGetLastErrorModesFieldName = "getLastErrorModes";
const std::string kHeartbeatIntervalFieldName = "heartbeatIntervalMillis";
const std::string kHeartbeatTimeoutFieldName = "heartbeatTimeoutSecs";
const std::string kReplicaSetIdFieldName = "replicaSetId";

}  // namespace

Status ReplicaSetConfig::initialize(const BSONObj& cfg,
                                    bool usePV1ByDefault,
                                    OID defaultReplicaSetId) {
    return _initialize(cfg, false, usePV1ByDefault, defaultReplicaSetId);
}

Status ReplicaSetConfig::initializeForInitiate(const BSONObj& cfg, bool usePV1ByDefault) {
    return _initialize(cfg, true, usePV1ByDefault, OID());
}

Status ReplicaSetConfig::_initialize(const BSONObj& cfg,
                                     bool forInitiate,
                                     bool usePV1ByDefault,
                                     OID defaultReplicaSetId) {
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
                                        << memberElement.fieldName()
                                        << " to be Object, but found "
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
        cfg,
        kConfigServerFieldName,
        forInitiate ? serverGlobalParams.clusterRole == ClusterRole::ConfigServer : false,
        &_configServer);
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
    // Parse writeConcernMajorityJournalDefault
    //
    status = bsonExtractBooleanFieldWithDefault(cfg,
                                                kWriteConcernMajorityJournalDefaultFieldName,
                                                _protocolVersion == 1,
                                                &_writeConcernMajorityJournalDefault);
    if (!status.isOK())
        return status;

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

    //
    // Generate replica set ID if called from replSetInitiate.
    // Otherwise, uses 'defaultReplicatSetId' as default if 'cfg' doesn't have an ID.
    //
    if (forInitiate) {
        if (_replicaSetId.isSet()) {
            return Status(ErrorCodes::InvalidReplicaSetConfig,
                          str::stream() << "replica set configuration cannot contain '"
                                        << kReplicaSetIdFieldName
                                        << "' "
                                           "field when called from replSetInitiate: "
                                        << cfg);
        }
        _replicaSetId = OID::gen();
    } else if (!_replicaSetId.isSet()) {
        _replicaSetId = defaultReplicaSetId;
    }

    _calculateMajorities();
    _addInternalWriteConcernModes();
    _initializeConnectionString();
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
    auto greaterThanZero = stdx::bind(std::greater<long long>(), stdx::placeholders::_1, 0);
    long long electionTimeoutMillis;
    auto electionTimeoutStatus = bsonExtractIntegerFieldWithDefaultIf(
        settings,
        kElectionTimeoutFieldName,
        durationCount<Milliseconds>(kDefaultElectionTimeoutPeriod),
        greaterThanZero,
        "election timeout must be greater than 0",
        &electionTimeoutMillis);
    if (!electionTimeoutStatus.isOK()) {
        return electionTimeoutStatus;
    }
    _electionTimeoutPeriod = Milliseconds(electionTimeoutMillis);

    //
    // Parse heartbeatTimeoutSecs
    //
    long long heartbeatTimeoutSecs;
    Status heartbeatTimeoutStatus =
        bsonExtractIntegerFieldWithDefaultIf(settings,
                                             kHeartbeatTimeoutFieldName,
                                             durationCount<Seconds>(kDefaultHeartbeatTimeoutPeriod),
                                             greaterThanZero,
                                             "heartbeat timeout must be greater than 0",
                                             &heartbeatTimeoutSecs);
    if (!heartbeatTimeoutStatus.isOK()) {
        return heartbeatTimeoutStatus;
    }
    _heartbeatTimeoutPeriod = Seconds(heartbeatTimeoutSecs);

    //
    // Parse chainingAllowed
    //
    Status status = bsonExtractBooleanFieldWithDefault(
        settings, kChainingAllowedFieldName, kDefaultChainingAllowed, &_chainingAllowed);
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
                                        << kGetLastErrorModesFieldName
                                        << '.'
                                        << modeElement.fieldName()
                                        << " to be an Object, not "
                                        << typeName(modeElement.type()));
        }
        ReplicaSetTagPattern pattern = _tagConfig.makePattern();
        for (BSONObj::iterator constraintIter(modeElement.Obj()); constraintIter.more();) {
            const BSONElement constraintElement = constraintIter.next();
            if (!constraintElement.isNumber()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Expected " << kSettingsFieldName << '.'
                                            << kGetLastErrorModesFieldName
                                            << '.'
                                            << modeElement.fieldName()
                                            << '.'
                                            << constraintElement.fieldName()
                                            << " to be a number, not "
                                            << typeName(constraintElement.type()));
            }
            const int minCount = constraintElement.numberInt();
            if (minCount <= 0) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Value of " << kSettingsFieldName << '.'
                                            << kGetLastErrorModesFieldName
                                            << '.'
                                            << modeElement.fieldName()
                                            << '.'
                                            << constraintElement.fieldName()
                                            << " must be positive, but found "
                                            << minCount);
            }
            status = _tagConfig.addTagCountConstraintToPattern(
                &pattern, constraintElement.fieldNameStringData(), minCount);
            if (!status.isOK()) {
                return status;
            }
        }
        _customWriteConcernModes[modeElement.fieldNameStringData()] = pattern;
    }

    // Parse replica set ID.
    OID replicaSetId;
    status = mongo::bsonExtractOIDField(settings, kReplicaSetIdFieldName, &replicaSetId);
    if (status.isOK()) {
        if (!replicaSetId.isSet()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << kReplicaSetIdFieldName << " field value cannot be null");
        }
    } else if (status != ErrorCodes::NoSuchKey) {
        return status;
    }
    _replicaSetId = replicaSetId;

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
                                    << kIdFieldName
                                    << " field");
    }
    if (_heartbeatInterval < Milliseconds(0)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kSettingsFieldName << '.' << kHeartbeatIntervalFieldName
                                    << " field value must be non-negative, "
                                       "but found "
                                    << durationCount<Milliseconds>(_heartbeatInterval));
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
                              str::stream() << "Found two member configurations with same "
                                            << MemberConfig::kIdFieldName
                                            << " field, "
                                            << kMembersFieldName
                                            << "."
                                            << i
                                            << "."
                                            << MemberConfig::kIdFieldName
                                            << " == "
                                            << kMembersFieldName
                                            << "."
                                            << j
                                            << "."
                                            << MemberConfig::kIdFieldName
                                            << " == "
                                            << memberI.getId());
            }
            if (memberI.getHostAndPort() == memberJ.getHostAndPort()) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Found two member configurations with same "
                                            << MemberConfig::kHostFieldName
                                            << " field, "
                                            << kMembersFieldName
                                            << "."
                                            << i
                                            << "."
                                            << MemberConfig::kHostFieldName
                                            << " == "
                                            << kMembersFieldName
                                            << "."
                                            << j
                                            << "."
                                            << MemberConfig::kHostFieldName
                                            << " == "
                                            << memberI.getHostAndPort().toString());
            }
        }
    }

    if (localhostCount != 0 && localhostCount != _members.size()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "Either all host names in a replica set configuration must be localhost "
                   "references, or none must be; found "
                << localhostCount
                << " out of "
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

    if (_protocolVersion != 0 && _protocolVersion != 1) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << kProtocolVersionFieldName << " field value of "
                                    << _protocolVersion
                                    << " is not 1 or 0");
    }

    if (_configServer) {
        if (_protocolVersion == 0) {
            return Status(ErrorCodes::BadValue, "Config servers cannot run in protocolVersion 0");
        }
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
        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            return Status(ErrorCodes::BadValue,
                          "Nodes being used for config servers must be started with the "
                          "--configsvr flag");
        }
        if (!_writeConcernMajorityJournalDefault) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << kWriteConcernMajorityJournalDefaultFieldName
                                        << " must be true in replica set configurations being "
                                           "used for config servers");
        }
    } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return Status(ErrorCodes::BadValue,
                      "Nodes started with the --configsvr flag must have configsvr:true in "
                      "their config");
    }

    if (!_connectionString.isValid()) {
        return Status(ErrorCodes::BadValue,
                      "ReplicaSetConfig represented an invalid replica set ConnectionString");
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
                                    << writeConcern.wMode
                                    << "\"");
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

bool ReplicaSetConfig::isLocalHostAllowed() const {
    // It is sufficient to check any one member's hostname, since in ReplicaSetConfig::validate,
    // it's ensured that either all members have hostname localhost or none do.
    return _members.begin()->getHostAndPort().isLocalHost();
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

void ReplicaSetConfig::_initializeConnectionString() {
    std::vector<HostAndPort> visibleMembers;
    for (const auto& member : _members) {
        if (!member.isHidden() && !member.isArbiter()) {
            visibleMembers.push_back(member.getHostAndPort());
        }
    }

    try {
        _connectionString = ConnectionString::forReplicaSet(_replSetName, visibleMembers);
    } catch (const DBException& e) {
        invariant(e.getCode() == ErrorCodes::FailedToParse);
        // Failure to construct the ConnectionString means either an invalid replica set name
        // or members array, which should be caught in validate()
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

    // Only include writeConcernMajorityJournalDefault if it is not the default version for this
    // ProtocolVersion to prevent breaking cross version-3.2.1 compatibilty of ReplicaSetConfigs.
    if (_protocolVersion > 0) {
        configBuilder.append(kProtocolVersionFieldName, _protocolVersion);
        // Only include writeConcernMajorityJournalDefault if it is not the default version for this
        // ProtocolVersion to prevent breaking cross version-3.2.1 compatibilty of
        // ReplicaSetConfigs.
        if (!_writeConcernMajorityJournalDefault) {
            configBuilder.append(kWriteConcernMajorityJournalDefaultFieldName,
                                 _writeConcernMajorityJournalDefault);
        }
    } else if (_writeConcernMajorityJournalDefault) {
        configBuilder.append(kWriteConcernMajorityJournalDefaultFieldName,
                             _writeConcernMajorityJournalDefault);
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

    if (_replicaSetId.isSet()) {
        settingsBuilder.append(kReplicaSetIdFieldName, _replicaSetId);
    }

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

int ReplicaSetConfig::_calculatePriorityRank(double priority) const {
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
