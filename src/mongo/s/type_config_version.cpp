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

namespace mongo {

    using mongoutils::str::stream;

    const string VersionType::ConfigNS("config.version");

    const BSONField<int> VersionType::minVersion("minVersion");
    const BSONField<int> VersionType::maxVersion("maxVersion");
    const BSONField<OID> VersionType::clusterId("clusterId");
    const BSONField<int> VersionType::version_DEPRECATED("version");

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
        if (_maxVersion > 3 && !_clusterId.isSet()) {
            if (errMsg) {
                *errMsg = stream() << "no clusterId found";
            }
            return false;
        }

        return true;
    }

    void VersionType::clear() {
        _minVersion = -1;
        _maxVersion = -1;
        _clusterId = OID();
    }

    bool VersionType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extractNumber(source, minVersion, -1, &_minVersion, errMsg)) return false;

        if (_minVersion == -1) {
            if (!FieldParser::extractNumber(source, version_DEPRECATED, -1, &_minVersion, errMsg)) return false;
        }

        if (!FieldParser::extractNumber(source, maxVersion, -1, &_maxVersion, errMsg)) return false;

        if (!FieldParser::extract(source, clusterId, OID(), &_clusterId, errMsg)) return false;

        if (_maxVersion == -1) {
            _maxVersion = _minVersion;
        }

        return true;
    }

    BSONObj VersionType::toBSON() const {
        return BSON("_id" << 1
                << version_DEPRECATED(_minVersion)
                << minVersion(_minVersion)
                << maxVersion(_maxVersion)
                << clusterId(_clusterId));
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

    int VersionType::getMaxCompatibleVersion() const {
        return _maxVersion;
    }

    void VersionType::setMaxCompatibleVersion(int version) {
        _maxVersion = version;
    }

    bool VersionType::isCompatibleVersion(int version) const {
        return version >= _minVersion && version <= _maxVersion;
    }

    void VersionType::setDefaultVersion() {
        _minVersion = 1;
        _maxVersion = 1;
        _clusterId = OID();
    }

    void VersionType::setEmptyVersion() {
        _minVersion = 0;
        _maxVersion = 0;
        _clusterId = OID();
    }

    bool VersionType::isEmptyVersion() const {
        return _minVersion == 0 && _maxVersion == 0 && _clusterId == OID();
    }

    bool VersionType::isEquivalentTo(const VersionType& other) const {
        return _minVersion == other._minVersion && _maxVersion == other._maxVersion
               && _clusterId == other._clusterId;
    }

} // namespace mongo
