/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {


template <typename T>
class StatusWith;

/**
 * A description of a request for a count operation. Copyable.
 */
class CountRequest {
public:
    /**
     * Construct an empty request.
     */
    CountRequest(NamespaceString nss, BSONObj query);

    const NamespaceString& getNs() const {
        return _nss;
    }

    const BSONObj getQuery() const {
        return _query;
    }

    long long getLimit() const {
        return _limit.value_or(0);
    }

    void setLimit(long long limit) {
        _limit = limit;
    }

    long long getSkip() const {
        return _skip.value_or(0);
    }

    void setSkip(long long skip) {
        _skip = skip;
    }

    const BSONObj getHint() const {
        return _hint.value_or(BSONObj());
    }

    void setHint(BSONObj hint);

    const BSONObj getCollation() const {
        return _collation.value_or(BSONObj());
    }

    void setCollation(BSONObj collation);

    /**
     * Constructs a BSON representation of this request, which can be used for sending it in
     * commands.
     */
    BSONObj toBSON() const;

    /**
     * Construct a CountRequest from the command specification and db name.
     */
    static StatusWith<CountRequest> parseFromBSON(const std::string& dbname, const BSONObj& cmdObj);

private:
    // Namespace to operate on (e.g. "foo.bar").
    const NamespaceString _nss;

    // A predicate describing the set of documents to count.
    const BSONObj _query;

    // Optional. An integer limiting the number of documents to count.
    boost::optional<long long> _limit;

    // Optional. An integer indicating to not include the first n documents in the count.
    boost::optional<long long> _skip;

    // Optional. Indicates to the query planner that it should generate a count plan using a
    // particular index.
    boost::optional<BSONObj> _hint;

    // Optional. The collation used to compare strings.
    boost::optional<BSONObj> _collation;
};

}  // namespace mongo
