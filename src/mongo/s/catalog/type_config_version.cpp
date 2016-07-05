/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/s/catalog/type_config_version.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const std::string VersionType::ConfigNS = "config.version";

const BSONField<int> VersionType::minCompatibleVersion("minCompatibleVersion");
const BSONField<int> VersionType::currentVersion("currentVersion");
const BSONField<BSONArray> VersionType::excludingMongoVersions("excluding");
const BSONField<OID> VersionType::clusterId("clusterId");
const BSONField<OID> VersionType::upgradeId("upgradeId");
const BSONField<BSONObj> VersionType::upgradeState("upgradeState");

void VersionType::clear() {
    _minCompatibleVersion.reset();
    _currentVersion.reset();
    _excludingMongoVersions.reset();
    _clusterId.reset();
    _upgradeId.reset();
    _upgradeState.reset();
}

void VersionType::cloneTo(VersionType* other) const {
    other->clear();

    other->_minCompatibleVersion = _minCompatibleVersion;
    other->_currentVersion = _currentVersion;
    other->_excludingMongoVersions = _excludingMongoVersions;
    other->_clusterId = _clusterId;
    other->_upgradeId = _upgradeId;
    other->_upgradeState = _upgradeState;
}

Status VersionType::validate() const {
    if (!_minCompatibleVersion.is_initialized()) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "missing " << minCompatibleVersion.name() << " field"};
    }

    if (!_currentVersion.is_initialized()) {
        return {ErrorCodes::NoSuchKey,
                str::stream() << "missing " << currentVersion.name() << " field"};
    }

    // UpgradeHistory::UpgradeHistory_NoEpochVersion is the last version without a cluster id
    if (getCurrentVersion() > UpgradeHistory::UpgradeHistory_NoEpochVersion &&
        !_clusterId.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << clusterId.name() << " field"};
    }

    if (!_clusterId->isSet()) {
        return {ErrorCodes::NotYetInitialized, "Cluster ID cannot be empty"};
    }

    return Status::OK();
}

BSONObj VersionType::toBSON() const {
    BSONObjBuilder builder;

    builder.append("_id", 1);
    if (_minCompatibleVersion)
        builder.append(minCompatibleVersion.name(), getMinCompatibleVersion());
    if (_currentVersion)
        builder.append(currentVersion.name(), getCurrentVersion());
    if (_excludingMongoVersions) {
        builder.append(excludingMongoVersions.name(),
                       MongoVersionRange::toBSONArray(getExcludingMongoVersions()));
    }
    if (_clusterId)
        builder.append(clusterId.name(), getClusterId());
    if (_upgradeId) {
        builder.append(upgradeId.name(), getUpgradeId());
        builder.append(upgradeState.name(), getUpgradeState());
    }

    return builder.obj();
}

StatusWith<VersionType> VersionType::fromBSON(const BSONObj& source) {
    VersionType version;

    {
        long long vMinCompatibleVersion;
        Status status =
            bsonExtractIntegerField(source, minCompatibleVersion.name(), &vMinCompatibleVersion);
        if (!status.isOK())
            return status;
        version._minCompatibleVersion = vMinCompatibleVersion;
    }

    {
        long long vCurrentVersion;
        Status status = bsonExtractIntegerField(source, currentVersion.name(), &vCurrentVersion);
        if (status.isOK()) {
            version._currentVersion = vCurrentVersion;
        } else if (status == ErrorCodes::NoSuchKey) {
            version._currentVersion = version._minCompatibleVersion;
        } else {
            return status;
        }
    }

    {
        BSONElement vClusterIdElem;
        Status status =
            bsonExtractTypedField(source, clusterId.name(), BSONType::jstOID, &vClusterIdElem);
        if (status.isOK()) {
            version._clusterId = vClusterIdElem.OID();
        } else if (status == ErrorCodes::NoSuchKey &&
                   version.getCurrentVersion() <= UpgradeHistory::UpgradeHistory_NoEpochVersion) {
            // UpgradeHistory::UpgradeHistory_NoEpochVersion is the last version
            // without a cluster id
        } else {
            return status;
        }
    }

    {
        BSONElement vExclMongoVersionsElem;
        Status status = bsonExtractTypedField(
            source, excludingMongoVersions.name(), BSONType::Array, &vExclMongoVersionsElem);
        if (status.isOK()) {
            version._excludingMongoVersions = std::vector<MongoVersionRange>();
            BSONObjIterator it(vExclMongoVersionsElem.Obj());
            while (it.more()) {
                MongoVersionRange range;

                std::string errMsg;
                if (!range.parseBSONElement(it.next(), &errMsg)) {
                    return {ErrorCodes::FailedToParse, errMsg};
                }

                version._excludingMongoVersions->push_back(range);
            }
        } else if (status == ErrorCodes::NoSuchKey) {
            // 'excludingMongoVersions' field is optional
        } else {
            return status;
        }
    }

    {
        BSONElement vUpgradeIdElem;
        Status status =
            bsonExtractTypedField(source, upgradeId.name(), BSONType::jstOID, &vUpgradeIdElem);
        if (status.isOK()) {
            version._upgradeId = vUpgradeIdElem.OID();
        } else if (status == ErrorCodes::NoSuchKey) {
            // 'upgradeId' field is optional
        } else {
            return status;
        }
    }

    if (source.hasField(upgradeState.name())) {
        BSONElement vUpgradeStateElem;
        Status status = bsonExtractTypedField(
            source, upgradeState.name(), BSONType::Object, &vUpgradeStateElem);
        if (status.isOK()) {
            version._upgradeState = vUpgradeStateElem.Obj().getOwned();
        } else if (status == ErrorCodes::NoSuchKey) {
            // 'upgradeState' field is optional
        } else {
            return status;
        }
    }

    return version;
}

void VersionType::setMinCompatibleVersion(const int minCompatibleVersion) {
    _minCompatibleVersion = minCompatibleVersion;
}

void VersionType::setCurrentVersion(const int currentVersion) {
    _currentVersion = currentVersion;
}

void VersionType::setClusterId(const OID& clusterId) {
    _clusterId = clusterId;
}

void VersionType::setExcludingMongoVersions(
    const std::vector<MongoVersionRange>& excludingMongoVersions) {
    _excludingMongoVersions = excludingMongoVersions;
}

std::string VersionType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
