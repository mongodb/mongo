/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/query.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
/**
 * Represents a full query description, including all options required for the query to be passed on
 * to other hosts
 */
class QuerySpec {
    std::string _ns;
    int _ntoskip;
    int _ntoreturn;
    int _options;
    BSONObj _query;
    BSONObj _fields;
    Query _queryObj;

public:
    QuerySpec(const std::string& ns,
              const BSONObj& query,
              const BSONObj& fields,
              int ntoskip,
              int ntoreturn,
              int options)
        : _ns(ns),
          _ntoskip(ntoskip),
          _ntoreturn(ntoreturn),
          _options(options),
          _query(query.getOwned()),
          _fields(fields.getOwned()),
          _queryObj(_query) {}

    QuerySpec() {}

    bool isEmpty() const {
        return _ns.size() == 0;
    }

    bool isExplain() const {
        return _queryObj.isExplain();
    }
    BSONObj filter() const {
        return _queryObj.getFilter();
    }

    BSONObj hint() const {
        return _queryObj.getHint();
    }
    BSONObj sort() const {
        return _queryObj.getSort();
    }
    BSONObj query() const {
        return _query;
    }
    BSONObj fields() const {
        return _fields;
    }
    BSONObj* fieldsData() {
        return &_fields;
    }

    // don't love this, but needed downstrem
    const BSONObj* fieldsPtr() const {
        return &_fields;
    }

    std::string ns() const {
        return _ns;
    }
    int ntoskip() const {
        return _ntoskip;
    }
    int ntoreturn() const {
        return _ntoreturn;
    }
    int options() const {
        return _options;
    }

    void setFields(BSONObj& o) {
        _fields = o.getOwned();
    }

    std::string toString() const {
        return str::stream() << "QSpec "
                             << BSON("ns" << _ns << "n2skip" << _ntoskip << "n2return" << _ntoreturn
                                          << "options"
                                          << _options
                                          << "query"
                                          << _query
                                          << "fields"
                                          << _fields);
    }
};

}  // namespace mongo
