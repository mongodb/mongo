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

    BSONObj getQuery() const {
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

    BSONObj getHint() const {
        return _hint.value_or(BSONObj());
    }

    void setHint(BSONObj hint);

    BSONObj getCollation() const {
        return _collation.value_or(BSONObj());
    }

    void setCollation(BSONObj collation);

    bool isExplain() const {
        return _explain;
    }

    void setExplain(bool explain) {
        _explain = explain;
    }

    const std::string& getComment() const {
        return _comment;
    }

    void setComment(StringData comment) {
        _comment = comment.toString();
    }

    unsigned int getMaxTimeMS() const {
        return _maxTimeMS;
    }

    void setMaxTimeMS(unsigned int maxTimeMS) {
        _maxTimeMS = maxTimeMS;
    }

    BSONObj getReadConcern() const {
        return _readConcern;
    }

    void setReadConcern(BSONObj readConcern) {
        _readConcern = readConcern.getOwned();
    }

    BSONObj getUnwrappedReadPref() const {
        return _unwrappedReadPref;
    }

    void setUnwrappedReadPref(BSONObj unwrappedReadPref) {
        _unwrappedReadPref = unwrappedReadPref.getOwned();
    }

    /**
     * Converts this CountRequest into an aggregation.
     */
    StatusWith<BSONObj> asAggregationCommand() const;

    /**
     * Construct a CountRequest from the command specification and db name. Caller must indicate if
     * this is an explained count via 'isExplain'.
     */
    static StatusWith<CountRequest> parseFromBSON(const std::string& dbname,
                                                  const BSONObj& cmdObj,
                                                  bool isExplain);

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

    BSONObj _readConcern;

    // The unwrapped readPreference object, if one was given to us by the mongos command processor.
    // This object will be empty when no readPreference is specified or if the request does not
    // originate from mongos.
    BSONObj _unwrappedReadPref;

    // When non-empty, represents a user comment.
    std::string _comment;

    // A user-specified maxTimeMS limit, or a value of '0' if not specified.
    unsigned int _maxTimeMS = 0;

    // If true, generate an explain plan instead of the actual count.
    bool _explain = false;
};

}  // namespace mongo
