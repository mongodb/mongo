/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/is_master_response.h"

#include "mongo/base/status.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

const std::string kIsMasterFieldName = "ismaster";
const std::string kSecondaryFieldName = "secondary";
const std::string kSetNameFieldName = "setName";
const std::string kSetVersionFieldName = "setVersion";
const std::string kHostsFieldName = "hosts";
const std::string kPassivesFieldName = "passives";
const std::string kArbitersFieldName = "arbiters";
const std::string kPrimaryFieldName = "primary";
const std::string kArbiterOnlyFieldName = "arbiterOnly";
const std::string kPassiveFieldName = "passive";
const std::string kHiddenFieldName = "hidden";
const std::string kBuildIndexesFieldName = "buildIndexes";
const std::string kSlaveDelayFieldName = "slaveDelay";
const std::string kTagsFieldName = "tags";
const std::string kMeFieldName = "me";
const std::string kElectionIdFieldName = "electionId";
const std::string kLastWriteOpTimeFieldName = "opTime";
const std::string kLastWriteDateFieldName = "lastWriteDate";
const std::string kLastMajorityWriteOpTimeFieldName = "majorityOpTime";
const std::string kLastMajorityWriteDateFieldName = "majorityWriteDate";
const std::string kLastWriteFieldName = "lastWrite";

// field name constants that don't directly correspond to member variables
const std::string kInfoFieldName = "info";
const std::string kIsReplicaSetFieldName = "isreplicaset";
const std::string kErrmsgFieldName = "errmsg";
const std::string kCodeFieldName = "code";

}  // namespace

IsMasterResponse::IsMasterResponse()
    : _isMaster(false),
      _isMasterSet(false),
      _secondary(false),
      _isSecondarySet(false),
      _setNameSet(false),
      _setVersion(0),
      _setVersionSet(false),
      _hostsSet(false),
      _passivesSet(false),
      _arbitersSet(false),
      _primarySet(false),
      _arbiterOnly(false),
      _arbiterOnlySet(false),
      _passive(false),
      _passiveSet(false),
      _hidden(false),
      _hiddenSet(false),
      _buildIndexes(true),
      _buildIndexesSet(false),
      _slaveDelay(0),
      _slaveDelaySet(false),
      _tagsSet(false),
      _meSet(false),
      _electionId(OID()),
      _configSet(true),
      _shutdownInProgress(false) {}

void IsMasterResponse::addToBSON(BSONObjBuilder* builder) const {
    if (_hostsSet) {
        std::vector<std::string> hosts;
        for (size_t i = 0; i < _hosts.size(); ++i) {
            hosts.push_back(_hosts[i].toString());
        }
        builder->append(kHostsFieldName, hosts);
    }
    if (_passivesSet) {
        std::vector<std::string> passives;
        for (size_t i = 0; i < _passives.size(); ++i) {
            passives.push_back(_passives[i].toString());
        }
        builder->append(kPassivesFieldName, passives);
    }
    if (_arbitersSet) {
        std::vector<std::string> arbiters;
        for (size_t i = 0; i < _arbiters.size(); ++i) {
            arbiters.push_back(_arbiters[i].toString());
        }
        builder->append(kArbitersFieldName, arbiters);
    }

    if (_setNameSet) {
        builder->append(kSetNameFieldName, _setName);
    }

    if (_shutdownInProgress) {
        builder->append(kCodeFieldName, ErrorCodes::ShutdownInProgress);
        builder->append(kErrmsgFieldName, "replication shutdown in progress");
        return;
    }

    if (!_configSet) {
        builder->append(kIsMasterFieldName, false);
        builder->append(kSecondaryFieldName, false);
        builder->append(kInfoFieldName, "Does not have a valid replica set config");
        builder->append(kIsReplicaSetFieldName, true);
        return;
    }

    invariant(_setVersionSet);
    builder->append(kSetVersionFieldName, static_cast<int>(_setVersion));
    invariant(_isMasterSet);
    builder->append(kIsMasterFieldName, _isMaster);
    invariant(_isSecondarySet);
    builder->append(kSecondaryFieldName, _secondary);

    if (_primarySet)
        builder->append(kPrimaryFieldName, _primary.toString());
    if (_arbiterOnlySet)
        builder->append(kArbiterOnlyFieldName, _arbiterOnly);
    if (_passiveSet)
        builder->append(kPassiveFieldName, _passive);
    if (_hiddenSet)
        builder->append(kHiddenFieldName, _hidden);
    if (_buildIndexesSet)
        builder->append(kBuildIndexesFieldName, _buildIndexes);
    if (_slaveDelaySet)
        builder->appendIntOrLL(kSlaveDelayFieldName, durationCount<Seconds>(_slaveDelay));
    if (_tagsSet) {
        BSONObjBuilder tags(builder->subobjStart(kTagsFieldName));
        for (unordered_map<std::string, std::string>::const_iterator it = _tags.begin();
             it != _tags.end();
             ++it) {
            tags.append(it->first, it->second);
        }
    }
    invariant(_meSet);
    builder->append(kMeFieldName, _me.toString());
    if (_electionId.isSet())
        builder->append(kElectionIdFieldName, _electionId);
    if (_lastWrite || _lastMajorityWrite) {
        BSONObjBuilder lastWrite(builder->subobjStart(kLastWriteFieldName));
        if (_lastWrite) {
            lastWrite.append(kLastWriteOpTimeFieldName, _lastWrite->opTime.toBSON());
            lastWrite.appendTimeT(kLastWriteDateFieldName, _lastWrite->value);
        }
        if (_lastMajorityWrite) {
            lastWrite.append(kLastMajorityWriteOpTimeFieldName,
                             _lastMajorityWrite->opTime.toBSON());
            lastWrite.appendTimeT(kLastMajorityWriteDateFieldName, _lastMajorityWrite->value);
        }
    }
}

BSONObj IsMasterResponse::toBSON() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.obj();
}

Status IsMasterResponse::initialize(const BSONObj& doc) {
    Status status = bsonExtractBooleanField(doc, kIsMasterFieldName, &_isMaster);
    if (!status.isOK()) {
        return status;
    }
    _isMasterSet = true;
    status = bsonExtractBooleanField(doc, kSecondaryFieldName, &_secondary);
    if (!status.isOK()) {
        return status;
    }
    _isSecondarySet = true;
    if (doc.hasField(kInfoFieldName)) {
        if (_isMaster || _secondary || !doc.hasField(kIsReplicaSetFieldName) ||
            !doc[kIsReplicaSetFieldName].booleanSafe()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Expected presence of \"" << kInfoFieldName
                                        << "\" field to indicate no valid config loaded, but other "
                                           "fields weren't as we expected");
        }
        _configSet = false;
        return Status::OK();
    } else {
        if (doc.hasField(kIsReplicaSetFieldName)) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Found \"" << kIsReplicaSetFieldName
                                        << "\" field which should indicate that no valid config "
                                           "is loaded, but we didn't also have an \""
                                        << kInfoFieldName
                                        << "\" field as we expected");
        }
    }

    status = bsonExtractStringField(doc, kSetNameFieldName, &_setName);
    if (!status.isOK()) {
        return status;
    }
    _setNameSet = true;
    status = bsonExtractIntegerField(doc, kSetVersionFieldName, &_setVersion);
    if (!status.isOK()) {
        return status;
    }
    _setVersionSet = true;

    if (doc.hasField(kHostsFieldName)) {
        BSONElement hostsElement;
        status = bsonExtractTypedField(doc, kHostsFieldName, Array, &hostsElement);
        if (!status.isOK()) {
            return status;
        }
        for (BSONObjIterator it(hostsElement.Obj()); it.more();) {
            BSONElement hostElement = it.next();
            if (hostElement.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kHostsFieldName
                                            << "\" array of isMaster response must be of type "
                                            << typeName(String)
                                            << " but found type "
                                            << typeName(hostElement.type()));
            }
            _hosts.push_back(HostAndPort(hostElement.String()));
        }
        _hostsSet = true;
    }

    if (doc.hasField(kPassivesFieldName)) {
        BSONElement passivesElement;
        status = bsonExtractTypedField(doc, kPassivesFieldName, Array, &passivesElement);
        if (!status.isOK()) {
            return status;
        }
        for (BSONObjIterator it(passivesElement.Obj()); it.more();) {
            BSONElement passiveElement = it.next();
            if (passiveElement.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kPassivesFieldName
                                            << "\" array of isMaster response must be of type "
                                            << typeName(String)
                                            << " but found type "
                                            << typeName(passiveElement.type()));
            }
            _passives.push_back(HostAndPort(passiveElement.String()));
        }
        _passivesSet = true;
    }

    if (doc.hasField(kArbitersFieldName)) {
        BSONElement arbitersElement;
        status = bsonExtractTypedField(doc, kArbitersFieldName, Array, &arbitersElement);
        if (!status.isOK()) {
            return status;
        }
        for (BSONObjIterator it(arbitersElement.Obj()); it.more();) {
            BSONElement arbiterElement = it.next();
            if (arbiterElement.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kArbitersFieldName
                                            << "\" array of isMaster response must be of type "
                                            << typeName(String)
                                            << " but found type "
                                            << typeName(arbiterElement.type()));
            }
            _arbiters.push_back(HostAndPort(arbiterElement.String()));
        }
        _arbitersSet = true;
    }

    if (doc.hasField(kPrimaryFieldName)) {
        std::string primaryString;
        status = bsonExtractStringField(doc, kPrimaryFieldName, &primaryString);
        if (!status.isOK()) {
            return status;
        }
        _primary = HostAndPort(primaryString);
        _primarySet = true;
    }

    if (doc.hasField(kArbiterOnlyFieldName)) {
        status = bsonExtractBooleanField(doc, kArbiterOnlyFieldName, &_arbiterOnly);
        if (!status.isOK()) {
            return status;
        }
        _arbiterOnlySet = true;
    }

    if (doc.hasField(kPassiveFieldName)) {
        status = bsonExtractBooleanField(doc, kPassiveFieldName, &_passive);
        if (!status.isOK()) {
            return status;
        }
        _passiveSet = true;
    }

    if (doc.hasField(kHiddenFieldName)) {
        status = bsonExtractBooleanField(doc, kHiddenFieldName, &_hidden);
        if (!status.isOK()) {
            return status;
        }
        _hiddenSet = true;
    }

    if (doc.hasField(kBuildIndexesFieldName)) {
        status = bsonExtractBooleanField(doc, kBuildIndexesFieldName, &_buildIndexes);
        if (!status.isOK()) {
            return status;
        }
        _buildIndexesSet = true;
    }

    if (doc.hasField(kSlaveDelayFieldName)) {
        long long slaveDelaySecs;
        status = bsonExtractIntegerField(doc, kSlaveDelayFieldName, &slaveDelaySecs);
        if (!status.isOK()) {
            return status;
        }
        _slaveDelaySet = true;
        _slaveDelay = Seconds(slaveDelaySecs);
    }

    if (doc.hasField(kTagsFieldName)) {
        BSONElement tagsElement;
        status = bsonExtractTypedField(doc, kTagsFieldName, Object, &tagsElement);
        if (!status.isOK()) {
            return status;
        }
        for (BSONObjIterator it(tagsElement.Obj()); it.more();) {
            BSONElement tagElement = it.next();
            if (tagElement.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kTagsFieldName
                                            << "\" obj "
                                               "of isMaster response must be of type "
                                            << typeName(String)
                                            << " but found type "
                                            << typeName(tagsElement.type()));
            }
            _tags[tagElement.fieldNameStringData().toString()] = tagElement.String();
        }
        _tagsSet = true;
    }

    if (doc.hasField(kElectionIdFieldName)) {
        BSONElement electionIdElem;
        status = bsonExtractTypedField(doc, kElectionIdFieldName, jstOID, &electionIdElem);
        if (!status.isOK()) {
            return status;
        }
        _electionId = electionIdElem.OID();
    }

    if (doc.hasField(kLastWriteFieldName)) {
        BSONElement lastWriteElement;
        status = bsonExtractTypedField(doc, kLastWriteFieldName, Object, &lastWriteElement);
        if (!status.isOK()) {
            return status;
        }
        BSONObj lastWriteObj = lastWriteElement.Obj();
        bool lastWriteOpTimeSet = false;
        bool lastWriteDateSet = false;
        if (auto lastWriteOpTimeElement = lastWriteObj[kLastWriteOpTimeFieldName]) {
            if (lastWriteOpTimeElement.type() != Object) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kLastWriteOpTimeFieldName
                                            << "\" obj "
                                               "of isMaster response must be of type "
                                            << typeName(Object)
                                            << " but found type "
                                            << typeName(lastWriteOpTimeElement.type()));
            }
            auto lastWriteOpTime = OpTime::parseFromOplogEntry(lastWriteOpTimeElement.Obj());
            if (!lastWriteOpTime.isOK()) {
                return lastWriteOpTime.getStatus();
            }
            if (_lastWrite) {
                _lastWrite->opTime = lastWriteOpTime.getValue();
            } else {
                _lastWrite = OpTimeWith<time_t>(0, lastWriteOpTime.getValue());
            }
            lastWriteOpTimeSet = true;
        }
        if (auto lastWriteDateElement = lastWriteObj[kLastWriteDateFieldName]) {
            if (lastWriteDateElement.type() != Date) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kLastWriteDateFieldName
                                            << "\" obj "
                                               "of isMaster response must be of type "
                                            << typeName(Date)
                                            << " but found type "
                                            << typeName(lastWriteDateElement.type()));
            }
            if (_lastWrite) {
                _lastWrite->value = lastWriteDateElement.Date().toTimeT();
            } else {
                _lastWrite = OpTimeWith<time_t>(lastWriteDateElement.Date().toTimeT(), OpTime());
            }
            lastWriteDateSet = true;
        }
        invariant(lastWriteOpTimeSet == lastWriteDateSet);

        bool lastMajorityWriteOpTimeSet = false;
        bool lastMajorityWriteDateSet = false;
        if (auto lastMajorityWriteOpTimeElement = lastWriteObj[kLastMajorityWriteOpTimeFieldName]) {
            if (lastMajorityWriteOpTimeElement.type() != Object) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kLastMajorityWriteOpTimeFieldName
                                            << "\" obj "
                                               "of isMaster response must be of type "
                                            << typeName(Object)
                                            << " but found type "
                                            << typeName(lastMajorityWriteOpTimeElement.type()));
            }
            auto lastMajorityWriteOpTime =
                OpTime::parseFromOplogEntry(lastMajorityWriteOpTimeElement.Obj());
            if (!lastMajorityWriteOpTime.isOK()) {
                return lastMajorityWriteOpTime.getStatus();
            }
            if (_lastMajorityWrite) {
                _lastMajorityWrite->opTime = lastMajorityWriteOpTime.getValue();
            } else {
                _lastMajorityWrite = OpTimeWith<time_t>(0, lastMajorityWriteOpTime.getValue());
            }
            lastMajorityWriteOpTimeSet = true;
        }
        if (auto lastMajorityWriteDateElement = lastWriteObj[kLastMajorityWriteDateFieldName]) {
            if (lastMajorityWriteDateElement.type() != Date) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << kLastMajorityWriteDateFieldName
                                            << "\" obj "
                                               "of isMaster response must be of type "
                                            << typeName(Date)
                                            << " but found type "
                                            << typeName(lastMajorityWriteDateElement.type()));
            }
            if (_lastMajorityWrite) {
                _lastMajorityWrite->value = lastMajorityWriteDateElement.Date().toTimeT();
            } else {
                _lastMajorityWrite =
                    OpTimeWith<time_t>(lastMajorityWriteDateElement.Date().toTimeT(), OpTime());
            }
            lastMajorityWriteDateSet = true;
        }
        invariant(lastMajorityWriteOpTimeSet == lastMajorityWriteDateSet);
    }

    std::string meString;
    status = bsonExtractStringField(doc, kMeFieldName, &meString);
    if (!status.isOK()) {
        return status;
    }
    _me = HostAndPort(meString);
    _meSet = true;

    return Status::OK();
}

void IsMasterResponse::setIsMaster(bool isMaster) {
    _isMasterSet = true;
    _isMaster = isMaster;
}

void IsMasterResponse::setIsSecondary(bool secondary) {
    _isSecondarySet = true;
    _secondary = secondary;
}

void IsMasterResponse::setReplSetName(const std::string& setName) {
    _setNameSet = true;
    _setName = setName;
}

void IsMasterResponse::setReplSetVersion(long long version) {
    _setVersionSet = true;
    _setVersion = version;
}

void IsMasterResponse::addHost(const HostAndPort& host) {
    _hostsSet = true;
    _hosts.push_back(host);
}

void IsMasterResponse::addPassive(const HostAndPort& passive) {
    _passivesSet = true;
    _passives.push_back(passive);
}

void IsMasterResponse::addArbiter(const HostAndPort& arbiter) {
    _arbitersSet = true;
    _arbiters.push_back(arbiter);
}

void IsMasterResponse::setPrimary(const HostAndPort& primary) {
    _primarySet = true;
    _primary = primary;
}

void IsMasterResponse::setIsArbiterOnly(bool arbiterOnly) {
    _arbiterOnlySet = true;
    _arbiterOnly = arbiterOnly;
}

void IsMasterResponse::setIsPassive(bool passive) {
    _passiveSet = true;
    _passive = passive;
}

void IsMasterResponse::setIsHidden(bool hidden) {
    _hiddenSet = true;
    _hidden = hidden;
}

void IsMasterResponse::setShouldBuildIndexes(bool buildIndexes) {
    _buildIndexesSet = true;
    _buildIndexes = buildIndexes;
}

void IsMasterResponse::setSlaveDelay(Seconds slaveDelay) {
    _slaveDelaySet = true;
    _slaveDelay = slaveDelay;
}

void IsMasterResponse::addTag(const std::string& tagKey, const std::string& tagValue) {
    _tagsSet = true;
    _tags[tagKey] = tagValue;
}

void IsMasterResponse::setMe(const HostAndPort& me) {
    _meSet = true;
    _me = me;
}

void IsMasterResponse::setElectionId(const OID& electionId) {
    _electionId = electionId;
}

void IsMasterResponse::setLastWrite(const OpTime& lastWriteOpTime, const time_t lastWriteDate) {
    _lastWrite = OpTimeWith<time_t>(lastWriteDate, lastWriteOpTime);
}

void IsMasterResponse::setLastMajorityWrite(const OpTime& lastMajorityWriteOpTime,
                                            const time_t lastMajorityWriteDate) {
    _lastMajorityWrite = OpTimeWith<time_t>(lastMajorityWriteDate, lastMajorityWriteOpTime);
}

void IsMasterResponse::markAsNoConfig() {
    _configSet = false;
}

void IsMasterResponse::markAsShutdownInProgress() {
    _shutdownInProgress = true;
}

}  // namespace repl
}  // namespace mongo
