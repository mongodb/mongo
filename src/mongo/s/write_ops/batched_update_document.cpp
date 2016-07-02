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

#include "mongo/s/write_ops/batched_update_document.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

using mongoutils::str::stream;

const BSONField<BSONObj> BatchedUpdateDocument::query("q");
const BSONField<BSONObj> BatchedUpdateDocument::updateExpr("u");
const BSONField<bool> BatchedUpdateDocument::multi("multi", false);
const BSONField<bool> BatchedUpdateDocument::upsert("upsert", false);
const BSONField<BSONObj> BatchedUpdateDocument::collation("collation");

BatchedUpdateDocument::BatchedUpdateDocument() {
    clear();
}

BatchedUpdateDocument::~BatchedUpdateDocument() {}

bool BatchedUpdateDocument::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isQuerySet) {
        *errMsg = stream() << "missing " << query.name() << " field";
        return false;
    }

    if (!_isUpdateExprSet) {
        *errMsg = stream() << "missing " << updateExpr.name() << " field";
        return false;
    }

    return true;
}

BSONObj BatchedUpdateDocument::toBSON() const {
    BSONObjBuilder builder;

    if (_isQuerySet)
        builder.append(query(), _query);

    if (_isUpdateExprSet)
        builder.append(updateExpr(), _updateExpr);

    if (_isMultiSet)
        builder.append(multi(), _multi);

    if (_isUpsertSet)
        builder.append(upsert(), _upsert);

    if (_isCollationSet)
        builder.append(collation(), _collation);

    return builder.obj();
}

bool BatchedUpdateDocument::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;

    BSONObjIterator it(source);
    while (it.more()) {
        BSONElement elem = it.next();
        StringData fieldName = elem.fieldNameStringData();

        if (fieldName == query.name()) {
            fieldState = FieldParser::extract(elem, query, &_query, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isQuerySet = fieldState == FieldParser::FIELD_SET;
        } else if (fieldName == updateExpr.name()) {
            fieldState = FieldParser::extract(elem, updateExpr, &_updateExpr, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isUpdateExprSet = fieldState == FieldParser::FIELD_SET;
        } else if (fieldName == multi.name()) {
            fieldState = FieldParser::extract(elem, multi, &_multi, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isMultiSet = fieldState == FieldParser::FIELD_SET;
        } else if (fieldName == upsert.name()) {
            fieldState = FieldParser::extract(elem, upsert, &_upsert, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isUpsertSet = fieldState == FieldParser::FIELD_SET;
        } else if (fieldName == collation.name()) {
            fieldState = FieldParser::extract(elem, collation, &_collation, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID)
                return false;
            _isCollationSet = fieldState == FieldParser::FIELD_SET;
        } else {
            *errMsg = str::stream() << "Unknown option in update document: " << fieldName;
            return false;
        }
    }

    return true;
}

void BatchedUpdateDocument::clear() {
    _query = BSONObj();
    _isQuerySet = false;

    _updateExpr = BSONObj();
    _isUpdateExprSet = false;

    _multi = false;
    _isMultiSet = false;

    _upsert = false;
    _isUpsertSet = false;

    _collation = BSONObj();
    _isCollationSet = false;
}

void BatchedUpdateDocument::cloneTo(BatchedUpdateDocument* other) const {
    other->clear();

    other->_query = _query;
    other->_isQuerySet = _isQuerySet;

    other->_updateExpr = _updateExpr;
    other->_isUpdateExprSet = _isUpdateExprSet;

    other->_multi = _multi;
    other->_isMultiSet = _isMultiSet;

    other->_upsert = _upsert;
    other->_isUpsertSet = _isUpsertSet;

    other->_collation = _collation;
    other->_isCollationSet = _isCollationSet;
}

std::string BatchedUpdateDocument::toString() const {
    return toBSON().toString();
}

void BatchedUpdateDocument::setQuery(const BSONObj& query) {
    _query = query.getOwned();
    _isQuerySet = true;
}

void BatchedUpdateDocument::unsetQuery() {
    _isQuerySet = false;
}

bool BatchedUpdateDocument::isQuerySet() const {
    return _isQuerySet;
}

const BSONObj& BatchedUpdateDocument::getQuery() const {
    dassert(_isQuerySet);
    return _query;
}

void BatchedUpdateDocument::setUpdateExpr(const BSONObj& updateExpr) {
    _updateExpr = updateExpr.getOwned();
    _isUpdateExprSet = true;
}

void BatchedUpdateDocument::unsetUpdateExpr() {
    _isUpdateExprSet = false;
}

bool BatchedUpdateDocument::isUpdateExprSet() const {
    return _isUpdateExprSet;
}

const BSONObj& BatchedUpdateDocument::getUpdateExpr() const {
    dassert(_isUpdateExprSet);
    return _updateExpr;
}

void BatchedUpdateDocument::setMulti(bool multi) {
    _multi = multi;
    _isMultiSet = true;
}

void BatchedUpdateDocument::unsetMulti() {
    _isMultiSet = false;
}

bool BatchedUpdateDocument::isMultiSet() const {
    return _isMultiSet;
}

bool BatchedUpdateDocument::getMulti() const {
    if (_isMultiSet) {
        return _multi;
    } else {
        return multi.getDefault();
    }
}

void BatchedUpdateDocument::setUpsert(bool upsert) {
    _upsert = upsert;
    _isUpsertSet = true;
}

void BatchedUpdateDocument::unsetUpsert() {
    _isUpsertSet = false;
}

bool BatchedUpdateDocument::isUpsertSet() const {
    return _isUpsertSet;
}

bool BatchedUpdateDocument::getUpsert() const {
    if (_isUpsertSet) {
        return _upsert;
    } else {
        return upsert.getDefault();
    }
}

void BatchedUpdateDocument::setCollation(const BSONObj& collation) {
    _collation = collation.getOwned();
    _isCollationSet = true;
}

void BatchedUpdateDocument::unsetCollation() {
    _isCollationSet = false;
}

bool BatchedUpdateDocument::isCollationSet() const {
    return _isCollationSet;
}

const BSONObj& BatchedUpdateDocument::getCollation() const {
    dassert(_isCollationSet);
    return _collation;
}

}  // namespace mongo
