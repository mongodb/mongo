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

#include "mongo/s/batched_delete_document.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;
        const BSONField<BSONObj> BatchedDeleteDocument::query("q");
        const BSONField<int> BatchedDeleteDocument::limit("limit", 1);

    BatchedDeleteDocument::BatchedDeleteDocument() {
        clear();
    }

    BatchedDeleteDocument::~BatchedDeleteDocument() {
    }

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

        return true;
    }

    BSONObj BatchedDeleteDocument::toBSON() const {
        BSONObjBuilder builder;

        if (_isQuerySet) builder.append(query(), _query);

        if (_isLimitSet) builder.append(limit(), _limit);

        return builder.obj();
    }

    bool BatchedDeleteDocument::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, query, &_query, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isQuerySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, limit, &_limit, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isLimitSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void BatchedDeleteDocument::clear() {
        _query = BSONObj();
        _isQuerySet = false;

        _limit = 0;
        _isLimitSet = false;

    }

    void BatchedDeleteDocument::cloneTo(BatchedDeleteDocument* other) const {
        other->clear();

        other->_query = _query;
        other->_isQuerySet = _isQuerySet;

        other->_limit = _limit;
        other->_isLimitSet = _isLimitSet;
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

} // namespace mongo
