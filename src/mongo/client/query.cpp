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

BSONObj Query::getFilter() const {
    bool hasDollar;
    if (!isComplex(&hasDollar))
        return obj;

    return obj.getObjectField(hasDollar ? "$query" : "query");
}

Query& Query::readPref(ReadPreference pref, const BSONArray& tags) {
    appendComplex(ReadPrefField.name().c_str(),
                  ReadPreferenceSetting(pref, TagSet(tags)).toInnerBSON());
    return *this;
}

bool Query::isComplex(bool* hasDollar) const {
    return isComplex(obj, hasDollar);
}

Query& Query::appendElements(BSONObj elements) {
    makeComplex();
    BSONObjBuilder b(std::move(obj));
    b.appendElements(elements);
    obj = b.obj();
    return *this;
}

Query& Query::requestResumeToken(bool enable) {
    appendComplex("$_requestResumeToken", enable);
    return *this;
}

Query& Query::resumeAfter(BSONObj point) {
    appendComplex("$_resumeAfter", point);
    return *this;
}

Query& Query::maxTimeMS(long long timeout) {
    appendComplex("$maxTimeMS", timeout);
    return *this;
}

Query& Query::term(long long value) {
    appendComplex("term", value);
    return *this;
}

Query& Query::readConcern(BSONObj rc) {
    appendComplex("readConcern", rc);
    return *this;
}

Query& Query::readOnce(bool enable) {
    appendComplex("$readOnce", enable);
    return *this;
}
}  // namespace mongo
