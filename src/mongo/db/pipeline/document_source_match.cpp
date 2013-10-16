/**
*    Copyright (C) 2011 10gen Inc.
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

#include "mongo/pch.h"

#include <cctype>

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    const char DocumentSourceMatch::matchName[] = "$match";

    const char *DocumentSourceMatch::getSourceName() const {
        return matchName;
    }

    Value DocumentSourceMatch::serialize(bool explain) const {
        return Value(DOC(getSourceName() << Document(getQuery())));
    }

    boost::optional<Document> DocumentSourceMatch::getNext() {
        pExpCtx->checkForInterrupt();

        while (boost::optional<Document> next = pSource->getNext()) {
            // The matcher only takes BSON documents, so we have to make one.
            if (matcher.matches(next->toBson()))
                return next;
        }

        // Nothing matched
        return boost::none;
    }

namespace {
    // This block contains the functions that make up the implemantation of
    // DocumentSourceMatch::redactSafePortion(). They will only be called after
    // the Match expression has been successfully parsed so they can assume that
    // input is well formed.

    bool isAllDigits(const StringData& str) {
        if (str.empty())
            return false;

        for (size_t i=0; i < str.size(); i++) {
            if (!isdigit(str[i]))
                return false;
        }
        return true;
    }

    bool isFieldnameRedactSafe(const StringData& fieldName) {
        // Can't have numeric elements in the dotted path since redacting elements from an array
        // would change the indexes.

        const size_t dotPos = fieldName.find('.');
        if (dotPos == string::npos)
            return !isAllDigits(fieldName);

        const StringData part = fieldName.substr(0, dotPos);
        const StringData rest = fieldName.substr(dotPos + 1);
        return !isAllDigits(part) && isFieldnameRedactSafe(rest);
    }

    bool isTypeRedactSafeInComparison(BSONType type) {
        if (type == Array) return false;
        if (type == Object) return false;
        if (type == jstNULL) return false;
        if (type == Undefined) return false; // Currently a Matcher parse error.

        return true;
    }

    Document redactSafePortionTopLevel(BSONObj query); // mutually recursive with next function

    // Returns the redact-safe portion of an "inner" match expression. This is the layer like
    // {$gt: 5} which does not include the field name. Returns an empty document if none of the
    // expression can safely be promoted in front of a $redact.
    Document redactSafePortionDollarOps(BSONObj expr) {
        MutableDocument output;
        BSONForEach(field, expr) {
            if (field.fieldName()[0] != '$')
                continue;

            switch(BSONObj::MatchType(field.getGtLtOp(BSONObj::Equality))) {
            // These are always ok
            case BSONObj::opTYPE:
            case BSONObj::opREGEX:
            case BSONObj::opOPTIONS:
            case BSONObj::opMOD:
                output[field.fieldNameStringData()] = Value(field);
                break;

            // These are ok if the type of the rhs is allowed in comparisons
            case BSONObj::LTE:
            case BSONObj::GTE:
            case BSONObj::LT:
            case BSONObj::GT:
                if (isTypeRedactSafeInComparison(field.type()))
                    output[field.fieldNameStringData()] = Value(field);
                break;

            // $in must be all-or-nothing (like $or). Can't include subset of elements.
            case BSONObj::opIN: {
                bool allOk = true;
                BSONForEach(elem, field.Obj()) {
                    if (!isTypeRedactSafeInComparison(elem.type())) {
                        allOk = false;
                        break;
                    }
                }
                if (allOk) {
                    output[field.fieldNameStringData()] = Value(field);
                }

                break;
            }

            case BSONObj::opALL: {
                // $all can include subset of elements (like $and).
                vector<Value> matches;
                BSONForEach(elem, field.Obj()) {
                    // NOTE this currently doesn't allow {$all: [{$elemMatch: {...}}]}
                    if (isTypeRedactSafeInComparison(elem.type())) {
                        matches.push_back(Value(elem));
                    }
                }
                if (!matches.empty())
                    output[field.fieldNameStringData()] = Value::consume(matches);

                break;
            }

            case BSONObj::opELEM_MATCH: {
                BSONObj subIn = field.Obj();
                Document subOut;
                if (subIn.firstElementFieldName()[0] == '$') {
                    subOut = redactSafePortionDollarOps(subIn);
                } else {
                    subOut = redactSafePortionTopLevel(subIn);
                }

                if (!subOut.empty())
                    output[field.fieldNameStringData()] = Value(subOut);

                break;
            }

            // These are never allowed
            case BSONObj::Equality: // This actually means unknown
            case BSONObj::opMAX_DISTANCE:
            case BSONObj::opNEAR:
            case BSONObj::NE:
            case BSONObj::opSIZE:
            case BSONObj::NIN:
            case BSONObj::opEXISTS:
            case BSONObj::opWITHIN:
            case BSONObj::opGEO_INTERSECTS:
                continue;
            }
        }
        return output.freeze();
    }

    // Returns the redact-safe portion of an "outer" match expression. This is the layer like
    // {fieldName: {...}} which does include the field name. Returns an empty document if none of
    // the expression can safely be promoted in front of a $redact.
    Document redactSafePortionTopLevel(BSONObj query) {
        MutableDocument output;
        BSONForEach(field, query) {
            if (field.fieldName()[0] == '$') {
                if (str::equals(field.fieldName(), "$or")) {
                    // $or must be all-or-nothing (line $in). Can't include subset of elements.
                    vector<Value> okClauses;
                    BSONForEach(elem, field.Obj()) {
                        Document clause = redactSafePortionTopLevel(elem.Obj());
                        if (clause.empty()) {
                            okClauses.clear();
                            break;
                        }
                        okClauses.push_back(Value(clause));
                    }

                    if (!okClauses.empty())
                        output["$or"] = Value::consume(okClauses);
                }
                else if (str::equals(field.fieldName(), "$and")) {
                    // $and can include subset of elements (like $all).
                    vector<Value> okClauses;
                    BSONForEach(elem, field.Obj()) {
                        Document clause = redactSafePortionTopLevel(elem.Obj());
                        if (!clause.empty())
                            okClauses.push_back(Value(clause));
                    }
                    if (!okClauses.empty())
                        output["$and"] = Value::consume(okClauses);
                }

                continue;
            }

            if (!isFieldnameRedactSafe(field.fieldNameStringData()))
                continue;

            switch (field.type()) {
            case Array: continue; // exact matches on arrays are never allowed
            case jstNULL: continue; // can't look for missing fields
            case Undefined: continue; // Currently a Matcher parse error.

            case Object: {
                Document sub = redactSafePortionDollarOps(field.Obj());
                if (!sub.empty())
                    output[field.fieldNameStringData()] = Value(sub);

                break;
            }

            // All other types are ok to pass through
            default:
                output[field.fieldNameStringData()] = Value(field);
                break;
            }
        }
        return output.freeze();
    }
}

    BSONObj DocumentSourceMatch::redactSafePortion() const {
        return redactSafePortionTopLevel(getQuery()).toBson();
    }

    static void uassertNoDisallowedClauses(BSONObj query) {
        BSONForEach(e, query) {
            // can't use the Matcher API because this would segfault the constructor
            uassert(16395, "$where is not allowed inside of a $match aggregation expression",
                    ! str::equals(e.fieldName(), "$where"));
            // geo breaks if it is not the first portion of the pipeline
            uassert(16424, "$near is not allowed inside of a $match aggregation expression",
                    ! str::equals(e.fieldName(), "$near"));
            uassert(16426, "$nearSphere is not allowed inside of a $match aggregation expression",
                    ! str::equals(e.fieldName(), "$nearSphere"));
            if (e.isABSONObj())
                uassertNoDisallowedClauses(e.Obj());
        }
    }

    intrusive_ptr<DocumentSource> DocumentSourceMatch::createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15959, "the match filter must be an expression in an object",
                elem.type() == Object);

        uassertNoDisallowedClauses(elem.Obj());

        return new DocumentSourceMatch(elem.Obj(), pExpCtx);
    }

    BSONObj DocumentSourceMatch::getQuery() const {
        return *(matcher.getQuery());
    }

    DocumentSourceMatch::DocumentSourceMatch(const BSONObj &query,
                                             const intrusive_ptr<ExpressionContext> &pExpCtx)
        : DocumentSource(pExpCtx)
        , matcher(query.getOwned())
    {}
}
