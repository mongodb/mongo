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

#include "mongo/platform/basic.h"

#include "document_source.h"

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/server_parameters.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(internalAggregationLookupBatchSize,
                                      int,
                                      LiteParsedQuery::kDefaultBatchSize);

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           std::string localField,
                                           std::string foreignField,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _localField(std::move(localField)),
      _foreignField(foreignField),
      _foreignFieldFieldName(std::move(foreignField)) {}

DocumentSourceLookUp::~DocumentSourceLookUp() {
    DESTRUCTOR_GUARD(
        // A DBClientCursor will issue a killCursors command through its parent DBDirectClient when
        // it goes out of scope. To issue a killCursors command, a DBDirectClient needs a valid
        // OperationContext. So here we set the OperationContext on the DBDirectClient, then
        // aggressively destroy the DBClientCursor.
        // Note that we cannot rely on any sort of callback from above to provide a valid
        // OperationContext, since we might be destroyed from the destructor of a CursorManager,
        // which does not have an OperationContext. Thus, we unfortunately have to make a new one or
        // use the one on our thread's Client.
        if (_mongod && _cursor) {
            auto& client = cc();
            if (auto opCtx = client.getOperationContext()) {
                pExpCtx->opCtx = opCtx;
                _mongod->setOperationContext(opCtx);
                _cursor.reset();
            } else {
                auto newOpCtx = client.makeOperationContext();
                pExpCtx->opCtx = newOpCtx.get();
                _mongod->setOperationContext(newOpCtx.get());
                _cursor.reset();
            }
        });
}

REGISTER_DOCUMENT_SOURCE(lookup, DocumentSourceLookUp::createFromBson);

const char* DocumentSourceLookUp::getSourceName() const {
    return "$lookup";
}

std::unique_ptr<DBClientCursor> DocumentSourceLookUp::doQuery(const Document& docToLookUp) const {
    auto query = DocumentSourceLookUp::queryForInput(docToLookUp);

    // Defaults for everything except batch size.
    const int nToReturn = 0;
    const int nToSkip = 0;
    const BSONObj* fieldsToReturn = nullptr;
    const int queryOptions = 0;

    const int batchSize = internalAggregationLookupBatchSize;
    return _mongod->directClient()->query(_fromNs.ns(),
                                          std::move(query),
                                          nToReturn,
                                          nToSkip,
                                          fieldsToReturn,
                                          queryOptions,
                                          batchSize);
}

boost::optional<Document> DocumentSourceLookUp::getNext() {
    pExpCtx->checkForInterrupt();

    uassert(4567, "from collection cannot be sharded", !_mongod->isSharded(_fromNs));

    if (_handlingUnwind) {
        return unwindResult();
    }

    boost::optional<Document> input = pSource->getNext();
    if (!input)
        return {};
    auto cursor = doQuery(*input);

    std::vector<Value> results;
    int objsize = 0;
    while (cursor->more()) {
        BSONObj result = cursor->nextSafe();
        objsize += result.objsize();
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll() << " matching "
                              << DocumentSourceLookUp::queryForInput(*input)
                              << " exceeds maximum document size",
                objsize <= BSONObjMaxInternalSize);
        results.push_back(Value(result));
    }

    MutableDocument output(std::move(*input));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

bool DocumentSourceLookUp::coalesce(const intrusive_ptr<DocumentSource>& pNextSource) {
    if (_handlingUnwind) {
        return false;
    }

    auto unwindSrc = dynamic_cast<DocumentSourceUnwind*>(pNextSource.get());
    if (!unwindSrc || unwindSrc->getUnwindPath() != _as.getPath(false)) {
        return false;
    }
    _unwindSrc = std::move(unwindSrc);
    _handlingUnwind = true;
    return true;
}

void DocumentSourceLookUp::dispose() {
    _cursor.reset();
    pSource->dispose();
}

BSONObj DocumentSourceLookUp::queryForInput(const Document& input) const {
    Value localFieldVal = input.getNestedField(_localField);
    if (localFieldVal.missing()) {
        localFieldVal = Value(BSONNULL);
    }

    // { _foreignFieldFiedlName : { "$eq" : localFieldValue } }
    BSONObjBuilder query;
    BSONObjBuilder subObj(query.subobjStart(_foreignFieldFieldName));
    subObj << "$eq" << localFieldVal;
    subObj.doneFast();
    return query.obj();
}

boost::optional<Document> DocumentSourceLookUp::unwindResult() {
    const boost::optional<FieldPath> indexPath(_unwindSrc->indexPath());

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_cursor || !_cursor->more()) {
        _input = pSource->getNext();
        if (!_input)
            return {};

        _cursorIndex = 0;
        _cursor = doQuery(*_input);

        if (_unwindSrc->preserveNullAndEmptyArrays() && !_cursor->more()) {
            // There were no results for this cursor, but the $unwind was asked to preserve empty
            // arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly objects in the prefix of '_as', to act as if we had created
            // an empty array and then removed it.
            output.setNestedField(_as, Value());
            if (indexPath) {
                output.setNestedField(*indexPath, Value(BSONNULL));
            }
            return output.freeze();
        }
    }
    invariant(_cursor->more() && bool(_input));
    auto nextVal = Value(_cursor->nextSafe());

    // Move input document into output if this is the last or only result, otherwise perform a copy.
    MutableDocument output(_cursor->more() ? *_input : std::move(*_input));
    output.setNestedField(_as, nextVal);

    if (indexPath) {
        output.setNestedField(*indexPath, Value(_cursorIndex));
    }

    _cursorIndex++;
    return output.freeze();
}

void DocumentSourceLookUp::serializeToArray(std::vector<Value>& array, bool explain) const {
    MutableDocument output(
        DOC(getSourceName() << DOC("from" << _fromNs.coll() << "as" << _as.getPath(false)
                                          << "localField" << _localField.getPath(false)
                                          << "foreignField" << _foreignField.getPath(false))));
    if (_handlingUnwind && explain) {
        const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
        output[getSourceName()]["unwinding"] =
            Value(DOC("preserveNullAndEmptyArrays"
                      << _unwindSrc->preserveNullAndEmptyArrays() << "includeArrayIndex"
                      << (indexPath ? Value((*indexPath).getPath(false)) : Value())));
    }
    array.push_back(Value(output.freeze()));
    if (_handlingUnwind && !explain) {
        _unwindSrc->serializeToArray(array);
    }
}

DocumentSource::GetDepsReturn DocumentSourceLookUp::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(_localField.getPath(false));
    return SEE_NEXT;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(4569, "the $lookup specification must be an Object", elem.type() == Object);

    NamespaceString fromNs;
    std::string as;
    std::string localField;
    std::string foreignField;

    for (auto&& argument : elem.Obj()) {
        uassert(4570,
                str::stream() << "arguments to $lookup must be strings, " << argument << " is type "
                              << argument.type(),
                argument.type() == String);
        const auto argName = argument.fieldNameStringData();

        if (argName == "from") {
            fromNs = NamespaceString(pExpCtx->ns.db().toString() + '.' + argument.String());
        } else if (argName == "as") {
            as = argument.String();
        } else if (argName == "localField") {
            localField = argument.String();
        } else if (argName == "foreignField") {
            foreignField = argument.String();
        } else {
            uasserted(4571,
                      str::stream() << "unknown argument to $lookup: " << argument.fieldName());
        }
    }

    uassert(4572,
            "need to specify fields from, as, localField, and foreignField for a $lookup",
            !fromNs.ns().empty() && !as.empty() && !localField.empty() && !foreignField.empty());

    return new DocumentSourceLookUp(
        std::move(fromNs), std::move(as), std::move(localField), std::move(foreignField), pExpCtx);
}
}
