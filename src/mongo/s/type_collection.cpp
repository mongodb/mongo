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
#include "mongo/s/type_collection.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string CollectionType::ConfigNS = "config.collections";

    const BSONField<std::string> CollectionType::ns("_id");
    const BSONField<std::string> CollectionType::primary("primary");
    const BSONField<BSONObj> CollectionType::keyPattern("key");
    const BSONField<bool> CollectionType::unique("unique");
    const BSONField<Date_t> CollectionType::updatedAt("updatedAt");
    const BSONField<bool> CollectionType::noBalance("noBalance");
    const BSONField<OID> CollectionType::epoch("epoch");
    const BSONField<bool> CollectionType::dropped("dropped");
    const BSONField<OID> CollectionType::DEPRECATED_lastmodEpoch("lastmodEpoch");
    const BSONField<Date_t> CollectionType::DEPRECATED_lastmod("lastmod");

    CollectionType::CollectionType() {
        clear();
    }

    CollectionType::~CollectionType() {
    }

    bool CollectionType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isNsSet) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (!_isUpdatedAtSet) {
            *errMsg = stream() << "missing " << updatedAt.name() << " field";
            return false;
        }
        if (!_isEpochSet) {
            *errMsg = stream() << "missing " << epoch.name() << " field";
            return false;
        }

        // Either sharding or primary information or dropped should be filled.
        int numSet = 0;
        if (_isPrimarySet && !_primary.empty()) numSet++;
        if (_isKeyPatternSet && !(_keyPattern.nFields() == 0)) numSet++;
        if (_isDroppedSet && _dropped) numSet++;
        if (numSet != 1) {
            *errMsg = stream() << "one of " << primary.name() << " or " << keyPattern.name()
                               << " or " << dropped.name() << " should be filled";
            return false;
        }

        // Sharding related fields may only be set if the sharding key pattern is present, unless
        // we're dropped.
        if ( ( _unique || _noBalance ) && ( !_isDroppedSet || !_dropped )
            && ( _keyPattern.nFields() == 0 ) )
        {
            *errMsg = stream() << "missing " << keyPattern.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj CollectionType::toBSON() const {
        BSONObjBuilder builder;

        if (_isNsSet) builder.append(ns(), _ns);
        if (_isPrimarySet) builder.append(primary(), _primary);
        if (_isKeyPatternSet) builder.append(keyPattern(), _keyPattern);
        if (_isUniqueSet) builder.append(unique(), _unique);
        if (_isUpdatedAtSet) builder.append(updatedAt(), _updatedAt);
        if (_isNoBalanceSet) builder.append(noBalance(), _noBalance);

        if (_isUpdatedAtSet) builder.append(DEPRECATED_lastmod(), _updatedAt);
        if (_isEpochSet) {
            builder.append(epoch(), _epoch);
            builder.append(DEPRECATED_lastmodEpoch(), _epoch);
        }

        // Always need to write dropped for compatibility w/ 2.0/2.2
        builder.append(dropped(), _dropped);

        return builder.obj();
    }

    bool CollectionType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, ns, &_ns, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, primary, &_primary, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isPrimarySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, keyPattern, &_keyPattern, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isKeyPatternSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, unique, &_unique, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUniqueSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, updatedAt, &_updatedAt, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpdatedAtSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, noBalance, &_noBalance, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNoBalanceSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, epoch, &_epoch, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isEpochSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, dropped, &_dropped, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDroppedSet = fieldState == FieldParser::FIELD_SET;

        //
        // backward compatibility
        //

        // 'updatedAt' used to be called 'lastmod' up to 2.2.

        Date_t lastmod;
        fieldState = FieldParser::extract(source, DEPRECATED_lastmod, &lastmod, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;

        if (fieldState == FieldParser::FIELD_SET && _isUpdatedAtSet == false) {
            _updatedAt = lastmod;
            _isUpdatedAtSet = true;
        }

        // 'lastmodEpoch' was a transition format to 'epoch', up to 2.2
        OID lastmodEpoch;
        fieldState = FieldParser::extract(source, DEPRECATED_lastmodEpoch, &lastmodEpoch, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;

        if (fieldState == FieldParser::FIELD_SET && _isEpochSet == false) {
            _epoch = lastmodEpoch;
            _isEpochSet = true;
        }

        return true;
    }

    void CollectionType::clear() {

        _ns.clear();
        _isNsSet = false;

        _primary.clear();
        _isPrimarySet = false;

        _keyPattern = BSONObj();
        _isKeyPatternSet = false;

        _unique = false;
        _isUniqueSet = false;

        _updatedAt = 0ULL;
        _isUpdatedAtSet = false;

        _noBalance = false;
        _isNoBalanceSet = false;

        _epoch = OID();
        _isEpochSet = false;

        _dropped = false;
        _isDroppedSet = false;

    }

    void CollectionType::cloneTo(CollectionType* other) const {
        other->clear();

        other->_ns = _ns;
        other->_isNsSet = _isNsSet;

        other->_primary = _primary;
        other->_isPrimarySet = _isPrimarySet;

        other->_keyPattern = _keyPattern;
        other->_isKeyPatternSet = _isKeyPatternSet;

        other->_unique = _unique;
        other->_isUniqueSet = _isUniqueSet;

        other->_updatedAt = _updatedAt;
        other->_isUpdatedAtSet = _isUpdatedAtSet;

        other->_noBalance = _noBalance;
        other->_isNoBalanceSet = _isNoBalanceSet;

        other->_epoch = _epoch;
        other->_isEpochSet = _isEpochSet;

        other->_dropped = _dropped;
        other->_isDroppedSet = _isDroppedSet;

    }

    std::string CollectionType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
