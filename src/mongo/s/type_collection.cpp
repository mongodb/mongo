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
    BSONField<Date_t> CollectionType::createdAt("createdAt");
    BSONField<bool> CollectionType::noBalance("noBalance");
    BSONField<OID> CollectionType::epoch("epoch");

    BSONField<OID> CollectionType::DEPRECATED_lastmodEpoch("lastmodEpoch");
    BSONField<Date_t> CollectionType::DEPRECATED_lastmod("lastmod");
    BSONField<bool> CollectionType::DEPRECATED_dropped("dropped");

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
        if (_ns.empty()) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (_createdAt.millis == 0) {
            *errMsg = stream() << "missing " << createdAt.name() << " field";
            return false;
        }
        if (! _epoch.isSet()) {
            *errMsg = stream() << "missing " << epoch.name() << " field";
            return false;
        }

        // Either sharding or primary information should be filled.
        if (_primary.empty() == (_keyPattern.nFields() == 0)) {
            *errMsg = stream() << "either " << primary.name()
                               << " or " << keyPattern.name()
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
        if (!_ns.empty()) builder.append(ns(), _ns);
        if (!_primary.empty()) builder.append(primary(), _primary);
        if (_keyPattern.nFields()) builder.append(keyPattern(), _keyPattern);
        if (_unique) builder.append(unique(), _unique);
        if (_createdAt.millis > 0ULL) builder.append(createdAt(), _createdAt);
        if (_noBalance) builder.append(noBalance(), _noBalance);
        if (_epoch.isSet()) builder.append(epoch(), _epoch);
        return builder.obj();
    }

    void CollectionType::parseBSON(BSONObj source) {
        clear();

        bool ok = true;
        ok &= FieldParser::extract(source, ns, "", &_ns);
        ok &= FieldParser::extract(source, primary, "", &_primary);
        ok &= FieldParser::extract(source, keyPattern, BSONObj(), &_keyPattern);
        ok &= FieldParser::extract(source, unique, false, &_unique);
        ok &= FieldParser::extract(source, createdAt, 0ULL, &_createdAt);
        ok &= FieldParser::extract(source, noBalance, false, &_noBalance);
        ok &= FieldParser::extract(source, epoch, OID(), &_epoch);
        if (! ok) {
            clear();
            return;
        }

        //
        // backward compatibility
        //

        // 'createAt' used to be called 'lastmod' up to 2.2.
        Date_t lastmod;
        if (! FieldParser::extract(source, DEPRECATED_lastmod, 0ULL, &lastmod)) {
            clear();
            return;
        }
        else if (lastmod != 0ULL) {
            _createdAt = lastmod;
        }

        // There was a flag to mark a collection as loggically dropped, up to 2.2.
        bool dropped;
        if (! FieldParser::extract(source, DEPRECATED_dropped, false, &dropped) || dropped) {
            clear();
            return;
        }

        // 'lastmodEpoch' was a transition format to 'epoch', up to 2.2
        OID lastmodEpoch;
        if (! FieldParser::extract(source, DEPRECATED_lastmodEpoch, OID(), &lastmodEpoch)) {
            clear();
            return;
        }
        else if (lastmodEpoch.isSet()) {
            _epoch = lastmodEpoch;
        }
    }

    void CollectionType::clear() {
        _ns.clear();
        _primary.clear();
        _keyPattern = BSONObj();
        _unique = false;
        _createdAt = 0ULL;
        _noBalance = false;
        _epoch = OID();
    }

    void CollectionType::cloneTo(CollectionType* other) {
        other->clear();
        other->_ns = _ns;
        other->_primary = _primary;
        other->_keyPattern = _keyPattern;
        other->_unique = _unique;
        other->_createdAt = _createdAt;
        other->_noBalance = _noBalance;
        other->_epoch = _epoch;
    }

    std::string CollectionType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
