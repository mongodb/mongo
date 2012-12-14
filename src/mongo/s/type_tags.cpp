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

#include "mongo/s/type_tags.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string TagsType::ConfigNS = "config.tags";

    BSONField<std::string> TagsType::ns("ns");
    BSONField<std::string> TagsType::tag("tag");
    BSONField<BSONObj> TagsType::min("min");
    BSONField<BSONObj> TagsType::max("max");

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
        if (_ns.empty()) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (_tag.empty()) {
            *errMsg = stream() << "missing " << tag.name() << " field";
            return false;
        }
        if (! _min.nFields()) {
            *errMsg = stream() << "missing " << min.name() << " field";
            return false;
        }
        if (! _max.nFields()) {
            *errMsg = stream() << "missing " << max.name() << " field";
            return false;
        }

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

        if (!_ns.empty()) builder.append(ns(), _ns);
        if (!_tag.empty()) builder.append(tag(), _tag);
        if (_min.nFields()) builder.append(min(), _min);
        if (_max.nFields()) builder.append(max(), _max);

        return builder.obj();
    }

    bool TagsType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, ns, "", &_ns, errMsg)) return false;
        if (!FieldParser::extract(source, tag, "", &_tag, errMsg)) return false;
        if (!FieldParser::extract(source, min, BSONObj(), &_min, errMsg)) return false;
        if (!FieldParser::extract(source, max, BSONObj(), &_max, errMsg)) return false;

        return true;
    }

    void TagsType::clear() {
        _ns.clear();
        _tag.clear();
        _min = BSONObj();
        _max = BSONObj();
    }

    void TagsType::cloneTo(TagsType* other) {
        other->clear();
        other->_ns = _ns;
        other->_tag = _tag;
        other->_min = _min;
        other->_max = _max;
    }

} // namespace mongo
