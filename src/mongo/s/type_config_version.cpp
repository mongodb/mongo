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
 */
#include "mongo/s/type_config_version.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/version.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string VersionType::ConfigNS = "config.version";

    const BSONField<int> VersionType::minCompatibleVersion("minCompatibleVersion");
    const BSONField<int> VersionType::currentVersion("currentVersion");
    const BSONField<BSONArray> VersionType::excludingMongoVersions("excluding");
    const BSONField<OID> VersionType::clusterId("clusterId");
    const BSONField<int> VersionType::version_DEPRECATED("version");
    const BSONField<OID> VersionType::upgradeId("upgradeId");
    const BSONField<BSONObj> VersionType::upgradeState("upgradeState");

    VersionType::VersionType() {
        clear();
    }

    VersionType::~VersionType() {
    }

    bool VersionType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isMinCompatibleVersionSet) {
            *errMsg = stream() << "missing " << minCompatibleVersion.name() << " field";
            return false;
        }
        if (!_isCurrentVersionSet) {
            *errMsg = stream() << "missing " << currentVersion.name() << " field";
            return false;
        }

        // Hardcoded 3 here because it's the last version without a cluster id
        if (_currentVersion > 3 && !_isClusterIdSet) {
            *errMsg = stream() << "missing " << clusterId.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj VersionType::toBSON() const {
        BSONObjBuilder builder;

        builder.append("_id", 1);
        if (_isMinCompatibleVersionSet) {
            builder.append(version_DEPRECATED(), _minCompatibleVersion);
            builder.append(minCompatibleVersion(), _minCompatibleVersion);
        }
        if (_isCurrentVersionSet) builder.append(currentVersion(), _currentVersion);
        if (_isExcludingMongoVersionsSet) {
            builder.append(excludingMongoVersions(), _excludingMongoVersions);
        }
        if (_isClusterIdSet) builder.append(clusterId(), _clusterId);
        if (_isUpgradeIdSet) {
            builder.append(upgradeId(), _upgradeId);
            builder.append(upgradeState(), _upgradeState);
        }

        return builder.obj();
    }

    bool VersionType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extractNumber(source, minCompatibleVersion,
                                                -1, &_minCompatibleVersion, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMinCompatibleVersionSet = fieldState == FieldParser::FIELD_SET;

        if (!_isMinCompatibleVersionSet) {
            fieldState = FieldParser::extractNumber(source, version_DEPRECATED,
                                                    -1, &_minCompatibleVersion, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID) return false;
            _isMinCompatibleVersionSet = fieldState == FieldParser::FIELD_SET;
        }

        fieldState = FieldParser::extractNumber(source, currentVersion,
                                                -1, &_currentVersion, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isCurrentVersionSet = fieldState == FieldParser::FIELD_SET;

        if (!_isCurrentVersionSet && _isMinCompatibleVersionSet) {
            _currentVersion = _minCompatibleVersion;
            _isCurrentVersionSet = true;
        }

        fieldState = FieldParser::extract(source, excludingMongoVersions,
                                          BSONArray(), &_excludingMongoVersions, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isExcludingMongoVersionsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, clusterId, OID(), &_clusterId, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isClusterIdSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, upgradeId, OID(), &_upgradeId, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpgradeIdSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, upgradeState, BSONObj(), &_upgradeState, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpgradeStateSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void VersionType::clear() {

        _minCompatibleVersion = -1;
        _isMinCompatibleVersionSet = false;

        _currentVersion = -1;
        _isCurrentVersionSet = false;

        _excludingMongoVersions = BSONArray();
        _isExcludingMongoVersionsSet = false;

        _clusterId = OID();
        _isClusterIdSet = false;

        _upgradeId = OID();
        _isUpgradeIdSet = false;

        _upgradeState = BSONObj();
        _isUpgradeStateSet = false;

    }

    void VersionType::cloneTo(VersionType* other) const {
        other->clear();

        other->_minCompatibleVersion = _minCompatibleVersion;
        other->_isMinCompatibleVersionSet = _isMinCompatibleVersionSet;

        other->_currentVersion = _currentVersion;
        other->_isCurrentVersionSet = _isCurrentVersionSet;

        other->_excludingMongoVersions = _excludingMongoVersions;
        other->_isExcludingMongoVersionsSet = _isExcludingMongoVersionsSet;

        other->_clusterId = _clusterId;
        other->_isClusterIdSet = _isClusterIdSet;

        other->_upgradeId = _upgradeId;
        other->_isUpgradeIdSet = _isUpgradeIdSet;

        other->_upgradeState = _upgradeState;
        other->_isUpgradeStateSet = _isUpgradeStateSet;

    }

    std::string VersionType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
