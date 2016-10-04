/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include "mongo/bson/json.h"
#include "mongo/client/read_preference.h"
#include "mongo/util/net/message.h"


namespace mongo {

/** Represents a Mongo query expression.  Typically one uses the QUERY(...) macro to construct a
 * Query object.
    Examples:
       QUERY( "age" << 33 << "school" << "UCLA" ).sort("name")
       QUERY( "age" << GT << 30 << LT << 50 )
*/

class Query {
public:
    static const BSONField<BSONObj> ReadPrefField;
    static const BSONField<std::string> ReadPrefModeField;
    static const BSONField<BSONArray> ReadPrefTagsField;

    BSONObj obj;
    Query() : obj(BSONObj()) {}
    Query(const BSONObj& b) : obj(b) {}
    Query(const std::string& json);
    Query(const char* json);

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

    /** Add a sort (ORDER BY) criteria to the query expression.
        This version of sort() assumes you want to sort on a single field.
        @param asc = 1 for ascending order
        asc = -1 for descending order
    */
    Query& sort(const std::string& field, int asc = 1) {
        sort(BSON(field << asc));
        return *this;
    }

    /** Provide a hint to the query.
        @param keyPattern Key pattern for the index to use.
        Example:
          hint("{ts:1}")
    */
    Query& hint(BSONObj keyPattern);
    Query& hint(const std::string& jsonKeyPatt);

    /** Provide min and/or max index limits for the query.
        min <= x < max
     */
    Query& minKey(const BSONObj& val);
    /**
       max is exclusive
     */
    Query& maxKey(const BSONObj& val);

    /** Return explain information about execution of this query instead of the actual query
     * results.
     *  Normally it is easier to use the mongo shell to run db.find(...).explain().
     */
    Query& explain();

    /** Use snapshot mode for the query.  Snapshot mode assures no duplicates are returned, or
     * objects missed, which were present at both the start and end of the query's execution (if an
     * object is new during the query, or deleted during the query, it may or may not be returned,
     * even with snapshot mode).

        Note that short query responses (less than 1MB) are always effectively snapshotted.

        Currently, snapshot mode may not be used with sorting or explicit hints.
    */
    Query& snapshot();

    /** Queries to the Mongo database support a $where parameter option which contains
        a javascript function that is evaluated to see whether objects being queried match
        its criteria.  Use this helper to append such a function to a query object.
        Your query may also contain other traditional Mongo query terms.

        @param jscode The javascript function to evaluate against each potential object
               match.  The function must return true for matched objects.  Use the this
               variable to inspect the current object.
        @param scope SavedContext for the javascript object.  List in a BSON object any
               variables you would like defined when the jscode executes.  One can think
               of these as "bind variables".

        Examples:
          conn.findOne("test.coll", Query("{a:3}").where("this.b == 2 || this.c == 3"));
          Query badBalance = Query().where("this.debits - this.credits < 0");
    */
    Query& where(const std::string& jscode, BSONObj scope);
    Query& where(const std::string& jscode) {
        return where(jscode, BSONObj());
    }

    /**
     * Sets the read preference for this query.
     *
     * @param pref the read preference mode for this query.
     * @param tags the set of tags to use for this query.
     */
    Query& readPref(ReadPreference pref, const BSONArray& tags);

    /**
     * @return true if this query has an orderby, hint, or some other field
     */
    bool isComplex(bool* hasDollar = 0) const;
    static bool isComplex(const BSONObj& obj, bool* hasDollar = 0);

    BSONObj getFilter() const;
    BSONObj getSort() const;
    BSONObj getHint() const;
    bool isExplain() const;

    /**
     * @return true if the query object contains a read preference specification object.
     */
    static bool hasReadPreference(const BSONObj& queryObj);

    std::string toString() const;
    operator std::string() const {
        return toString();
    }

private:
    void makeComplex();
    template <class T>
    void appendComplex(const char* fieldName, const T& val) {
        makeComplex();
        BSONObjBuilder b;
        b.appendElements(obj);
        b.append(fieldName, val);
        obj = b.obj();
    }
};

void assembleQueryRequest(const std::string& ns,
                          BSONObj query,
                          int nToReturn,
                          int nToSkip,
                          const BSONObj* fieldsToReturn,
                          int queryOptions,
                          Message& toSend);

}  // namespace mongo
