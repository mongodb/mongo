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
 */

#include "mongo/s/batched_update_document.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const BSONField<BSONObj> BatchedUpdateDocument::query("q");
    const BSONField<BSONObj> BatchedUpdateDocument::updateExpr("u");
    const BSONField<bool> BatchedUpdateDocument::multi("multi");
    const BSONField<bool> BatchedUpdateDocument::upsert("upsert");

    BatchedUpdateDocument::BatchedUpdateDocument() {
        clear();
    }

    BatchedUpdateDocument::~BatchedUpdateDocument() {
    }

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

        if (_isQuerySet) builder.append(query(), _query);

        if (_isUpdateExprSet) builder.append(updateExpr(), _updateExpr);

        if (_isMultiSet) builder.append(multi(), _multi);

        if (_isUpsertSet) builder.append(upsert(), _upsert);

        return builder.obj();
    }

    bool BatchedUpdateDocument::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, query, &_query, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isQuerySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, updateExpr, &_updateExpr, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpdateExprSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, multi, &_multi, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMultiSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, upsert, &_upsert, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpsertSet = fieldState == FieldParser::FIELD_SET;

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
        dassert(_isMultiSet);
        return _multi;
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
        dassert(_isUpsertSet);
        return _upsert;
    }

} // namespace mongo
