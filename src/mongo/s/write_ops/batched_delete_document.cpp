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

        fieldState = FieldParser::extractNumber(source, limit, &_limit, errMsg);
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
