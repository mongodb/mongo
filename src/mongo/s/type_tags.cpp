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
#include "mongo/s/type_tags.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string TagsType::ConfigNS = "config.tags";

    const BSONField<std::string> TagsType::ns("ns");
    const BSONField<std::string> TagsType::tag("tag");
    const BSONField<BSONObj> TagsType::min("min");
    const BSONField<BSONObj> TagsType::max("max");

    TagsType::TagsType() {
        clear();
    }

    TagsType::~TagsType() {
    }

    bool TagsType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isNsSet) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (!_isTagSet) {
            *errMsg = stream() << "missing " << tag.name() << " field";
            return false;
        }
        if (!_isMinSet) {
            *errMsg = stream() << "missing " << min.name() << " field";
            return false;
        }
        if (!_isMaxSet) {
            *errMsg = stream() << "missing " << max.name() << " field";
            return false;
        }

        // NOTE: all the following semantic checks should eventually become the caller's
        // responsibility, and should be moved out of this class completely

        // 'min' and 'max' must share the same fields.
        if (_min.nFields() != _max.nFields()) {
            *errMsg = stream() << "min and max have a different number of keys";
            return false;
        }
        BSONObjIterator minIt(_min);
        BSONObjIterator maxIt(_max);
        while (minIt.more() && maxIt.more()) {
            BSONElement minElem = minIt.next();
            BSONElement maxElem = maxIt.next();
            if (strcmp(minElem.fieldName(), maxElem.fieldName())) {
                *errMsg = stream() << "min and max must have the same set of keys";
                return false;
            }
        }

        // 'max' should be greater than 'min'.
        if (_min.woCompare(_max) >= 0) {
            *errMsg = stream() << "max key must be greater than min key";
            return false;
        }

        return true;
    }

    BSONObj TagsType::toBSON() const {
        BSONObjBuilder builder;

        if (_isNsSet) builder.append(ns(), _ns);
        if (_isTagSet) builder.append(tag(), _tag);
        if (_isMinSet) builder.append(min(), _min);
        if (_isMaxSet) builder.append(max(), _max);

        return builder.obj();
    }

    bool TagsType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, ns, &_ns, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, tag, &_tag, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isTagSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, min, &_min, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMinSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, max, &_max, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMaxSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void TagsType::clear() {

        _ns.clear();
        _isNsSet = false;

        _tag.clear();
        _isTagSet = false;

        _min = BSONObj();
        _isMinSet = false;

        _max = BSONObj();
        _isMaxSet = false;

    }

    void TagsType::cloneTo(TagsType* other) const {
        other->clear();

        other->_ns = _ns;
        other->_isNsSet = _isNsSet;

        other->_tag = _tag;
        other->_isTagSet = _isTagSet;

        other->_min = _min;
        other->_isMinSet = _isMinSet;

        other->_max = _max;
        other->_isMaxSet = _isMaxSet;

    }

    std::string TagsType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
