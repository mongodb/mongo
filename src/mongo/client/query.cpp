/*    Copyright 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/query.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

using std::string;

const BSONField<BSONObj> Query::ReadPrefField("$readPreference");
const BSONField<string> Query::ReadPrefModeField("mode");
const BSONField<BSONArray> Query::ReadPrefTagsField("tags");


Query::Query(const string& json) : obj(fromjson(json)) {}

Query::Query(const char* json) : obj(fromjson(json)) {}

Query& Query::hint(const string& jsonKeyPatt) {
    return hint(fromjson(jsonKeyPatt));
}

Query& Query::where(const string& jscode, BSONObj scope) {
    /* use where() before sort() and hint() and explain(), else this will assert. */
    verify(!isComplex());
    BSONObjBuilder b;
    b.appendElements(obj);
    b.appendWhere(jscode, scope);
    obj = b.obj();
    return *this;
}

void Query::makeComplex() {
    if (isComplex())
        return;
    BSONObjBuilder b;
    b.append("query", obj);
    obj = b.obj();
}

Query& Query::sort(const BSONObj& s) {
    appendComplex("orderby", s);
    return *this;
}

Query& Query::hint(BSONObj keyPattern) {
    appendComplex("$hint", keyPattern);
    return *this;
}

Query& Query::explain() {
    appendComplex("$explain", true);
    return *this;
}

Query& Query::snapshot() {
    appendComplex("$snapshot", true);
    return *this;
}

Query& Query::minKey(const BSONObj& val) {
    appendComplex("$min", val);
    return *this;
}

Query& Query::maxKey(const BSONObj& val) {
    appendComplex("$max", val);
    return *this;
}

bool Query::isComplex(const BSONObj& obj, bool* hasDollar) {
    if (obj.hasElement("query")) {
        if (hasDollar)
            *hasDollar = false;
        return true;
    }

    if (obj.hasElement("$query")) {
        if (hasDollar)
            *hasDollar = true;
        return true;
    }

    return false;
}

Query& Query::readPref(ReadPreference pref, const BSONArray& tags) {
    appendComplex(ReadPrefField.name().c_str(), ReadPreferenceSetting(pref, TagSet(tags)).toBSON());
    return *this;
}

bool Query::isComplex(bool* hasDollar) const {
    return isComplex(obj, hasDollar);
}

bool Query::hasReadPreference(const BSONObj& queryObj) {
    const bool hasReadPrefOption = queryObj["$queryOptions"].isABSONObj() &&
        queryObj["$queryOptions"].Obj().hasField(ReadPrefField.name());

    bool canHaveReadPrefField = Query::isComplex(queryObj) ||
        // The find command has a '$readPreference' option.
        queryObj.firstElementFieldName() == StringData("find");

    return (canHaveReadPrefField && queryObj.hasField(ReadPrefField.name())) || hasReadPrefOption;
}

BSONObj Query::getFilter() const {
    bool hasDollar;
    if (!isComplex(&hasDollar))
        return obj;

    return obj.getObjectField(hasDollar ? "$query" : "query");
}
BSONObj Query::getSort() const {
    if (!isComplex())
        return BSONObj();
    BSONObj ret = obj.getObjectField("orderby");
    if (ret.isEmpty())
        ret = obj.getObjectField("$orderby");
    return ret;
}
BSONObj Query::getHint() const {
    if (!isComplex())
        return BSONObj();
    return obj.getObjectField("$hint");
}
bool Query::isExplain() const {
    return isComplex() && obj.getBoolField("$explain");
}

string Query::toString() const {
    return obj.toString();
}

void assembleQueryRequest(const string& ns,
                          BSONObj query,
                          int nToReturn,
                          int nToSkip,
                          const BSONObj* fieldsToReturn,
                          int queryOptions,
                          Message& toSend) {
    if (kDebugBuild) {
        massert(10337, (string) "object not valid assembleRequest query", query.isValid());
    }

    // see query.h for the protocol we are using here.
    BufBuilder b;
    int opts = queryOptions;
    b.appendNum(opts);
    b.appendStr(ns);
    b.appendNum(nToSkip);
    b.appendNum(nToReturn);
    query.appendSelfToBufBuilder(b);
    if (fieldsToReturn)
        fieldsToReturn->appendSelfToBufBuilder(b);
    toSend.setData(dbQuery, b.buf(), b.len());
}

}  // namespace mongo
