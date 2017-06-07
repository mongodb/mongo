/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/s/write_ops/batched_delete_document.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

using mongoutils::str::stream;
const BSONField<BSONObj> BatchedDeleteDocument::query("q");
const BSONField<int> BatchedDeleteDocument::limit("limit");
const BSONField<BSONObj> BatchedDeleteDocument::collation("collation");

BatchedDeleteDocument::BatchedDeleteDocument() {
    clear();
}

BatchedDeleteDocument::~BatchedDeleteDocument() {}

bool BatchedDeleteDocument::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isQuerySet) {
        *errMsg = stream() << "missing " << query.name() << " field";
        return false;
    }

    if (!_isLimitSet) {
        *errMsg = stream() << "missing " << limit.name() << " field";
        return false;
    }

    if (_limit != 0 && _limit != 1) {
        *errMsg = stream() << "specify either a 0 to delete all"
                           << "matching documents or 1 to delete a single document";
        return false;
    }

    return true;
}

BSONObj BatchedDeleteDocument::toBSON() const {
    BSONObjBuilder builder;

    if (_isQuerySet)
        builder.append(query(), _query);

    if (_isLimitSet)
        builder.append(limit(), _limit);

    if (_isCollationSet)
        builder.append(collation(), _collation);

    return builder.obj();
}

bool BatchedDeleteDocument::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;
    for (BSONElement field : source) {
        const StringData fieldName = field.fieldNameStringData();
        if (fieldName == query.name()) {
            fieldState = FieldParser::extract(field, query, &_query, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isQuerySet = fieldState == FieldParser::FIELD_SET;
        } else if (fieldName == limit.name()) {
            fieldState = FieldParser::extractNumber(field, limit, &_limit, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            // isValid() checks that it is 0 or 1, but by the time it gets there, it doesn't know if
            // it was originally 0.5.
            if (_limit != field.numberDouble()) {
                *errMsg = "The limit field in delete documents must be representable as an int";
                return false;
            }
            _isLimitSet = fieldState == FieldParser::FIELD_SET;
        } else if (fieldName == collation.name()) {
            fieldState = FieldParser::extract(field, collation, &_collation, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isCollationSet = fieldState == FieldParser::FIELD_SET;
        } else {
            *errMsg = str::stream() << "Unknown option in delete document: " << fieldName;
            return false;
        }
    }
    return true;
}

void BatchedDeleteDocument::clear() {
    _query = BSONObj();
    _isQuerySet = false;

    _limit = 0;
    _isLimitSet = false;

    _collation = BSONObj();
    _isCollationSet = false;
}

void BatchedDeleteDocument::cloneTo(BatchedDeleteDocument* other) const {
    other->clear();

    other->_query = _query;
    other->_isQuerySet = _isQuerySet;

    other->_limit = _limit;
    other->_isLimitSet = _isLimitSet;

    other->_collation = _collation;
    other->_isCollationSet = _isCollationSet;
}

std::string BatchedDeleteDocument::toString() const {
    return toBSON().toString();
}

void BatchedDeleteDocument::setQuery(const BSONObj& query) {
    _query = query.getOwned();
    _isQuerySet = true;
}

void BatchedDeleteDocument::unsetQuery() {
    _isQuerySet = false;
}

bool BatchedDeleteDocument::isQuerySet() const {
    return _isQuerySet;
}

const BSONObj& BatchedDeleteDocument::getQuery() const {
    dassert(_isQuerySet);
    return _query;
}

void BatchedDeleteDocument::setLimit(int limit) {
    _limit = limit;
    _isLimitSet = true;
}

void BatchedDeleteDocument::unsetLimit() {
    _isLimitSet = false;
}

bool BatchedDeleteDocument::isLimitSet() const {
    return _isLimitSet;
}

int BatchedDeleteDocument::getLimit() const {
    dassert(_isLimitSet);
    return _limit;
}

void BatchedDeleteDocument::setCollation(const BSONObj& collation) {
    _collation = collation.getOwned();
    _isCollationSet = true;
}

void BatchedDeleteDocument::unsetCollation() {
    _isCollationSet = false;
}

bool BatchedDeleteDocument::isCollationSet() const {
    return _isCollationSet;
}

const BSONObj& BatchedDeleteDocument::getCollation() const {
    dassert(_isCollationSet);
    return _collation;
}

}  // namespace mongo
