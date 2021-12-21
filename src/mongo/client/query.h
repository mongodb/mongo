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

#include "mongo/bson/json.h"
#include "mongo/client/read_preference.h"
#include "mongo/rpc/message.h"

namespace mongo {

/**
 * Represents a subset of query settings, such as sort, hint, etc. It is only used in the context of
 * the deprecated query API in 'DBClientBase', which has been superseded by `DBClientBase::find()`
 * and friends. Additional uses of this class should not be added to the code base!
 */
class Query {
public:
    static const BSONField<BSONObj> ReadPrefField;
    static const BSONField<std::string> ReadPrefModeField;
    static const BSONField<BSONArray> ReadPrefTagsField;

    /**
     * Creating a Query object from raw BSON is on its way out. Please don't add new callers under
     * any circumstances.
     */
    static Query fromBSONDeprecated(const BSONObj& b) {
        Query q;
        q.obj = b;
        return q;
    }

    Query() : obj(BSONObj()) {}

    /** Add a sort (ORDER BY) criteria to the query expression.
        @param sortPattern the sort order template.  For example to order by name ascending, time
            descending:
          { name : 1, ts : -1 }
        i.e.
          BSON( "name" << 1 << "ts" << -1 )
        or
          fromjson(" name : 1, ts : -1 ")
    */
    Query& sort(const BSONObj& sortPattern);

    /** Provide a hint to the query.
        @param keyPattern Key pattern for the index to use.
        Example:
          hint("{ts:1}")
    */
    Query& hint(BSONObj keyPattern);

    /**
     * Sets the read preference for this query.
     *
     * @param pref the read preference mode for this query.
     * @param tags the set of tags to use for this query.
     */
    Query& readPref(ReadPreference pref, const BSONArray& tags);

    BSONObj getFilter() const;

    /**
     * A temporary accessor that returns a reference to the internal BSON object. No new callers
     * should be introduced!
     * NB: must be implemented in the header because db/query/query_request cannot link against
     * client/client_query.
     */
    const BSONObj& getFullSettingsDeprecated() const {
        return obj;
    }

    /**
     * The setters below were added to make the contents of the Query's settings internal BSON
     * explicit. They will be reviewed and deprecated/removed as appropriate.
     */
    Query& appendElements(BSONObj elements);
    Query& requestResumeToken(bool enable);
    Query& resumeAfter(BSONObj point);
    Query& maxTimeMS(long long timeout);
    Query& term(long long value);
    Query& readConcern(BSONObj rc);
    Query& readOnce(bool enable);

private:
    BSONObj obj;

    /**
     * @return true if this query has an orderby, hint, or some other field
     */
    bool isComplex(bool* hasDollar = nullptr) const;
    static bool isComplex(const BSONObj& obj, bool* hasDollar = nullptr);

    void makeComplex();
    template <class T>
    void appendComplex(const char* fieldName, const T& val) {
        makeComplex();
        BSONObjBuilder b(std::move(obj));
        b.append(fieldName, val);
        obj = b.obj();
    }
};

inline std::ostream& operator<<(std::ostream& s, const Query& q) {
    return s << q.getFullSettingsDeprecated().toString();
}

}  // namespace mongo
