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

#include "mongo/client/client_deprecated.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/query_request_helper.h"

namespace mongo {
namespace client_deprecated {

namespace {
bool isComplexQueryObj(const BSONObj& obj, bool* hasDollar) {
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

BSONObj filterFromOpQueryObj(const BSONObj& obj) {
    bool hasDollar;
    if (!isComplexQueryObj(obj, &hasDollar)) {
        return obj;
    }

    return obj.getObjectField(hasDollar ? "$query" : "query");
}

void initFindFromOptions(int options, FindCommandRequest* findCommand) {
    bool tailable = (options & QueryOption_CursorTailable) != 0;
    bool awaitData = (options & QueryOption_AwaitData) != 0;
    if (awaitData) {
        findCommand->setAwaitData(true);
    }
    if (tailable) {
        findCommand->setTailable(true);
    }

    if ((options & QueryOption_NoCursorTimeout) != 0) {
        findCommand->setNoCursorTimeout(true);
    }
    if ((options & QueryOption_PartialResults) != 0) {
        findCommand->setAllowPartialResults(true);
    }
}

/**
 * Fills out the 'findCommand' output parameter based on the contents of 'querySettings'. Here,
 * 'querySettings' has the same format as the "query" field of the no-longer-supported OP_QUERY wire
 * protocol message. It can look something like this for example:
 *
 *    {$query: ..., $hint: ..., $min: ..., $max: ...}
 *
 * Note that this does not set the filter itself on the 'FindCommandRequest' -- this function only
 * deals with options that can be packed into the filter object.
 *
 * Although the OP_QUERY wire protocol message is no longer ever sent over the wire by the internal
 * client, this supports old callers of that still specify the operation they want to perform using
 * an OP_QUERY-inspired format.
 */
Status initFindFromOpQueryObj(const BSONObj& querySettings, FindCommandRequest* findCommand) {
    for (auto&& e : querySettings) {
        StringData name = e.fieldNameStringData();

        if (name == "$orderby" || name == "orderby") {
            if (Object == e.type()) {
                findCommand->setSort(e.embeddedObject().getOwned());
            } else if (Array == e.type()) {
                findCommand->setSort(e.embeddedObject());

                // TODO: Is this ever used?  I don't think so.
                // Quote:
                // This is for languages whose "objects" are not well ordered (JSON is well
                // ordered).
                // [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
                // note: this is slow, but that is ok as order will have very few pieces
                BSONObjBuilder b;
                char p[2] = "0";

                while (1) {
                    BSONObj j = findCommand->getSort().getObjectField(p);
                    if (j.isEmpty()) {
                        break;
                    }
                    BSONElement e = j.firstElement();
                    if (e.eoo()) {
                        return Status(ErrorCodes::BadValue, "bad order array");
                    }
                    if (!e.isNumber()) {
                        return Status(ErrorCodes::BadValue, "bad order array [2]");
                    }
                    b.append(e);
                    (*p)++;
                    if (!(*p <= '9')) {
                        return Status(ErrorCodes::BadValue, "too many ordering elements");
                    }
                }

                findCommand->setSort(b.obj());
            } else {
                return Status(ErrorCodes::BadValue, "sort must be object or array");
            }
        } else if (name == "term") {
            findCommand->setTerm(e.safeNumberLong());
        } else if (name == "readConcern") {
            if (e.type() != BSONType::Object) {
                return Status(ErrorCodes::BadValue, "readConcern must be an object");
            }
            findCommand->setReadConcern(e.embeddedObject().getOwned());
        } else if (name.startsWith("$")) {
            name = name.substr(1);  // chop first char
            if (name == "min") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                }
                findCommand->setMin(e.embeddedObject().getOwned());
            } else if (name == "max") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                }
                findCommand->setMax(e.embeddedObject().getOwned());
            } else if (name == "hint") {
                if (e.isABSONObj()) {
                    findCommand->setHint(e.embeddedObject().getOwned());
                } else if (String == e.type()) {
                    findCommand->setHint(e.wrap());
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "$hint must be either a string or nested object");
                }
            } else if (name == "returnKey") {
                // Won't throw.
                if (e.trueValue()) {
                    findCommand->setReturnKey(true);
                }
            } else if (name == "showDiskLoc") {
                // Won't throw.
                if (e.trueValue()) {
                    findCommand->setShowRecordId(true);
                    query_request_helper::addShowRecordIdMetaProj(findCommand);
                }
            } else if (name == "maxTimeMS") {
                StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                if (!maxTimeMS.isOK()) {
                    return maxTimeMS.getStatus();
                }
                findCommand->setMaxTimeMS(maxTimeMS.getValue());
            } else if (name == "readOnce") {
                if (e.booleanSafe()) {
                    findCommand->setReadOnce(true);
                }
            } else if (name == "_requestResumeToken") {
                if (e.booleanSafe()) {
                    findCommand->setRequestResumeToken(true);
                }
            } else if (name == "_resumeAfter") {
                findCommand->setResumeAfter(e.embeddedObjectUserCheck().getOwned());
            }
        }
    }

    return Status::OK();
}

}  // namespace

void initFindFromLegacyOptions(BSONObj bsonOptions, int options, FindCommandRequest* findCommand) {
    invariant(findCommand);
    BSONObj filter = filterFromOpQueryObj(bsonOptions);
    if (!filter.isEmpty()) {
        findCommand->setFilter(filter.getOwned());
    }

    uassertStatusOK(initFindFromOpQueryObj(bsonOptions, findCommand));
    initFindFromOptions(options, findCommand);
}

}  // namespace client_deprecated
}  // namespace mongo
