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

#include "mongo/s/type_collection.h"
#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string CollectionType::ConfigNS = "config.collections";
    BSONField<std::string> CollectionType::ns("_id");
    BSONField<std::string> CollectionType::primary("primary");
    BSONField<BSONObj> CollectionType::keyPattern("key");
    BSONField<bool> CollectionType::unique("unique");
    BSONField<Date_t> CollectionType::updatedAt("updatedAt");
    BSONField<bool> CollectionType::noBalance("noBalance");
    BSONField<OID> CollectionType::epoch("epoch");
    // To-be-deprecated, not yet
    BSONField<bool> CollectionType::dropped("dropped");

    BSONField<OID> CollectionType::DEPRECATED_lastmodEpoch("lastmodEpoch");
    BSONField<Date_t> CollectionType::DEPRECATED_lastmod("lastmod");

    CollectionType::CollectionType() {
        clear();
    }

    CollectionType::~CollectionType() {
    }

    bool CollectionType::isValid(std::string* errMsg) const {
        std::string dummy;

        if (errMsg == NULL) errMsg = &dummy;

        // All the mandatory fields must be present.
        if (_ns.empty()) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (_updatedAt.millis == 0) {
            *errMsg = stream() << "missing " << updatedAt.name() << " field";
            return false;
        }
        if (!_epoch.isSet()) {
            *errMsg = stream() << "missing " << epoch.name() << " field";
            return false;
        }

        // Either sharding or primary information should be filled.
        if (_primary.empty() == (_keyPattern.nFields() == 0)) {
            *errMsg = stream() << "either " << primary.name() << " or " << keyPattern.name()
                               << " should be filled";
            return false;
        }

        // Sharding related fields may only be set if the sharding key pattern is present.
        if ((_unique || _noBalance) && (_keyPattern.nFields() == 0)) {
            *errMsg = stream() << "missing " << keyPattern.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj CollectionType::toBSON() const {
        BSONObjBuilder builder;
        builder.append(ns(), _ns);
        if (!_primary.empty()) builder.append(primary(), _primary);
        if (_keyPattern.nFields()) builder.append(keyPattern(), _keyPattern);
        if (_unique) builder.append(unique(), _unique);
        builder.append(updatedAt(), _updatedAt);
        builder.append(DEPRECATED_lastmod(), _updatedAt);
        if (_noBalance) builder.append(noBalance(), _noBalance);
        if (_epoch.isSet()) {
            builder.append(epoch(), _epoch);
            builder.append(DEPRECATED_lastmodEpoch(), _epoch);
        }
        // Always need to write dropped for compatibility w/ 2.0/2.2
        builder.append(dropped(), _dropped);
        return builder.obj();
    }

    bool CollectionType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, ns, "", &_ns, errMsg)) return false;
        if (!FieldParser::extract(source, primary, "", &_primary, errMsg)) return false;
        if (!FieldParser::extract(source, keyPattern, BSONObj(), &_keyPattern, errMsg)) return false;
        if (!FieldParser::extract(source, unique, false, &_unique, errMsg)) return false;
        if (!FieldParser::extract(source, updatedAt, 0ULL, &_updatedAt, errMsg)) return false;
        if (!FieldParser::extract(source, noBalance, false, &_noBalance, errMsg)) return false;
        if (!FieldParser::extract(source, epoch, OID(), &_epoch, errMsg)) return false;
        if (!FieldParser::extract(source, dropped, false, &_dropped, errMsg)) return false;

        //
        // backward compatibility
        //

        // 'createAt' used to be called 'lastmod' up to 2.2.

        Date_t lastmod;
        if (!FieldParser::extract(source, DEPRECATED_lastmod, 0ULL, &lastmod, errMsg)) return false;

        if (lastmod != 0ULL) {
            _updatedAt = lastmod;
        }

        // 'lastmodEpoch' was a transition format to 'epoch', up to 2.2
        OID lastmodEpoch;
        if (!FieldParser::extract(source, DEPRECATED_lastmodEpoch, OID(), &lastmodEpoch, errMsg)) return false;

        if (lastmodEpoch.isSet()) {
            _epoch = lastmodEpoch;
        }

        return true;
    }

    void CollectionType::clear() {
        _ns.clear();
        _primary.clear();
        _keyPattern = BSONObj();
        _unique = false;
        _updatedAt = 0ULL;
        _noBalance = false;
        _epoch = OID();
        _dropped = false;
    }

    void CollectionType::cloneTo(CollectionType* other) const {
        other->clear();
        other->_ns = _ns;
        other->_primary = _primary;
        other->_keyPattern = _keyPattern;
        other->_unique = _unique;
        other->_updatedAt = _updatedAt;
        other->_noBalance = _noBalance;
        other->_epoch = _epoch;
        other->_dropped = _dropped;
    }

    std::string CollectionType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
