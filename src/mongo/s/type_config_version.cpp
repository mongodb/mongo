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

    const string VersionType::ConfigNS("config.version");

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

    bool VersionType::isValid(string* errMsg) const {

        std::string dummy;

        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        if (_minVersion == -1) {
            if (errMsg) {
                *errMsg = stream() << "no version found";
            }
            return false;
        }

        // Hardcoded 3 here because it's the last version without a cluster id
        if (_currentVersion > 3 && !_clusterId.isSet()) {
            if (errMsg) {
                *errMsg = stream() << "no clusterId found";
            }
            return false;
        }

        return true;
    }

    void VersionType::clear() {
        _minVersion = -1;
        _currentVersion = -1;
        _clusterId = OID();
        _excludes = BSONArray();
        _upgradeId = OID();
        _upgradeState = BSONObj();
    }

    bool VersionType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extractNumber(source, minCompatibleVersion, -1, &_minVersion, errMsg)) return false;

        if (_minVersion == -1) {
            if (!FieldParser::extractNumber(source, version_DEPRECATED, -1, &_minVersion, errMsg)) return false;
        }

        if (!FieldParser::extractNumber(source, currentVersion, -1, &_currentVersion, errMsg)) return false;

        if (_currentVersion == -1) {
            _currentVersion = _minVersion;
        }

        if (!FieldParser::extract(source, clusterId, OID(), &_clusterId, errMsg)) return false;

        if (!FieldParser::extract(source, excludingMongoVersions, _excludes, &_excludes, errMsg)) return false;

        if (!FieldParser::extract(source, upgradeId, OID(), &_upgradeId, errMsg)) return false;
        if (!FieldParser::extract(source, upgradeState, BSONObj(), &_upgradeState, errMsg)) return false;

        return true;
    }

    BSONObj VersionType::toBSON() const {

        BSONObjBuilder bob;
        bob << "_id" << 1;
        bob << version_DEPRECATED(_minVersion);
        bob << minCompatibleVersion(_minVersion);
        bob << currentVersion(_currentVersion);
        bob << clusterId(_clusterId);
        bob << excludingMongoVersions(_excludes);

        if (_upgradeId.isSet()) {
            bob << upgradeId(_upgradeId);
            bob << upgradeState(_upgradeState);
        }

        return bob.obj();
    }

    string VersionType::toString() const {
        return toBSON().toString();
    }

    OID VersionType::getClusterId() const {
        return _clusterId;
    }

    void VersionType::setClusterId(const OID& clusterId) {
        _clusterId = clusterId;
    }

    int VersionType::getMinCompatibleVersion() const {
        return _minVersion;
    }

    void VersionType::setMinCompatibleVersion(int version) {
        _minVersion = version;
    }

    int VersionType::getCurrentVersion() const {
        return _currentVersion;
    }

    void VersionType::setCurrentVersion(int version) {
        _currentVersion = version;
    }

    OID VersionType::getUpgradeId() const {
        return _upgradeId;
    }
    void VersionType::setUpgradeId(const OID& upgradeId) {
        _upgradeId = upgradeId;
    }

    BSONObj VersionType::getUpgradeState() const {
        return _upgradeState;
    }
    void VersionType::setUpgradeState(const BSONObj& upgradeState) {
        _upgradeState = upgradeState;
    }

    void VersionType::cloneTo(VersionType* other) const {

        other->clear();

        other->_minVersion = _minVersion;
        other->_currentVersion = _currentVersion;
        other->_clusterId = _clusterId;
        other->_excludes = _excludes;
    }

} // namespace mongo
