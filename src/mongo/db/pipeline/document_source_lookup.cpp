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
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;

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

REGISTER_DOCUMENT_SOURCE(lookup, DocumentSourceLookUp::createFromBson);

const char* DocumentSourceLookUp::getSourceName() const {
    return "$lookup";
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
    BSONObj query = queryForInput(*input);
    std::unique_ptr<DBClientCursor> cursor = _mongod->directClient()->query(_fromNs.ns(), query);

    std::vector<Value> results;
    int objsize = 0;
    while (cursor->more()) {
        BSONObj result = cursor->nextSafe();
        objsize += result.objsize();
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll() << " matching "
                              << query << " exceeds maximum document size",
                objsize <= BSONObjMaxInternalSize);
        results.push_back(Value(result));
    }

    MutableDocument output(std::move(*input));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

Pipeline::SourceContainer::iterator DocumentSourceLookUp::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    if (nextUnwind && !_handlingUnwind && nextUnwind->getUnwindPath() == _as.getPath(false)) {
        _unwindSrc = std::move(nextUnwind);
        _handlingUnwind = true;
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
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

BSONObjSet DocumentSourceLookUp::getOutputSorts() {
    BSONObjSet out;

    BSONObjSet inputSort = pSource->getOutputSorts();
    std::string asPath = _as.getPath(false);

    for (auto&& sortObj : inputSort) {
        // Truncate each sortObj at the '_as' path.
        BSONObjBuilder outputSort;

        for (BSONElement fieldSort : sortObj) {
            if (fieldSort.fieldNameStringData() == asPath) {
                break;
            }
            outputSort.append(fieldSort);
        }

        BSONObj outSortObj = outputSort.obj();
        if (!outSortObj.isEmpty()) {
            out.insert(outSortObj);
        }
    }

    return out;
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

        _cursor = _mongod->directClient()->query(_fromNs.ns(), queryForInput(*_input));
        _cursorIndex = 0;

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
