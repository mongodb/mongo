/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;
using std::string;
using std::vector;

/** Helper class to unwind array from a single document. */
class DocumentSourceUnwind::Unwinder {
public:
    Unwinder(const FieldPath& unwindPath,
             bool preserveNullAndEmptyArrays,
             const boost::optional<FieldPath>& indexPath);
    /** Reset the unwinder to unwind a new document. */
    void resetDocument(const Document& document);

    /**
     * @return the next document unwound from the document provided to resetDocument(), using
     * the current value in the array located at the provided unwindPath.
     *
     * Returns boost::none if the array is exhausted.
     */
    boost::optional<Document> getNext();

private:
    // Tracks whether or not we can possibly return any more documents. Note we may return
    // boost::none even if this is true.
    bool _haveNext = false;

    // Path to the array to unwind.
    const FieldPath _unwindPath;

    // Documents that have a nullish value, or an empty array for the field '_unwindPath', will pass
    // through the $unwind stage unmodified if '_preserveNullAndEmptyArrays' is true.
    const bool _preserveNullAndEmptyArrays;

    // If set, the $unwind stage will include the array index in the specified path, overwriting any
    // existing value, setting to null when the value was a non-array or empty array.
    const boost::optional<FieldPath> _indexPath;

    Value _inputArray;

    MutableDocument _output;

    // Document indexes of the field path components.
    vector<Position> _unwindPathFieldIndexes;

    // Index into the _inputArray to return next.
    size_t _index;
};

DocumentSourceUnwind::Unwinder::Unwinder(const FieldPath& unwindPath,
                                         bool preserveNullAndEmptyArrays,
                                         const boost::optional<FieldPath>& indexPath)
    : _unwindPath(unwindPath),
      _preserveNullAndEmptyArrays(preserveNullAndEmptyArrays),
      _indexPath(indexPath) {}

void DocumentSourceUnwind::Unwinder::resetDocument(const Document& document) {
    // Reset document specific attributes.
    _output.reset(document);
    _unwindPathFieldIndexes.clear();
    _index = 0;
    _inputArray = document.getNestedField(_unwindPath, &_unwindPathFieldIndexes);
    _haveNext = true;
}

boost::optional<Document> DocumentSourceUnwind::Unwinder::getNext() {
    // WARNING: Any functional changes to this method must also be implemented in the unwinding
    // implementation of the $lookup stage.
    if (!_haveNext) {
        return boost::none;
    }

    // Track which index this value came from. If 'includeArrayIndex' was specified, we will use
    // this index in the output document, or null if the value didn't come from an array.
    boost::optional<long long> indexForOutput;

    if (_inputArray.getType() == Array) {
        const size_t length = _inputArray.getArrayLength();
        invariant(_index == 0 || _index < length);

        if (length == 0) {
            // Preserve documents with empty arrays if asked to, otherwise skip them.
            _haveNext = false;
            if (!_preserveNullAndEmptyArrays) {
                return boost::none;
            }
            _output.removeNestedField(_unwindPathFieldIndexes);
        } else {
            // Set field to be the next element in the array. If needed, this will automatically
            // clone all the documents along the field path so that the end values are not shared
            // across documents that have come out of this pipeline operator. This is a partial deep
            // clone. Because the value at the end will be replaced, everything along the path
            // leading to that will be replaced in order not to share that change with any other
            // clones (or the original).
            _output.setNestedField(_unwindPathFieldIndexes, _inputArray[_index]);
            indexForOutput = _index;
            _index++;
            _haveNext = _index < length;
        }
    } else if (_inputArray.nullish()) {
        // Preserve a nullish value if asked to, otherwise skip it.
        _haveNext = false;
        if (!_preserveNullAndEmptyArrays) {
            return boost::none;
        }
    } else {
        // Any non-nullish, non-array type should pass through.
        _haveNext = false;
    }

    if (_indexPath) {
        _output.getNestedField(*_indexPath) =
            indexForOutput ? Value(*indexForOutput) : Value(BSONNULL);
    }

    return _haveNext ? _output.peek() : _output.freeze();
}

DocumentSourceUnwind::DocumentSourceUnwind(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                           const FieldPath& fieldPath,
                                           bool preserveNullAndEmptyArrays,
                                           const boost::optional<FieldPath>& indexPath)
    : DocumentSource(pExpCtx),
      _unwindPath(fieldPath),
      _preserveNullAndEmptyArrays(preserveNullAndEmptyArrays),
      _indexPath(indexPath),
      _unwinder(new Unwinder(fieldPath, preserveNullAndEmptyArrays, indexPath)) {}

REGISTER_DOCUMENT_SOURCE(unwind, DocumentSourceUnwind::createFromBson);

const char* DocumentSourceUnwind::getSourceName() const {
    return "$unwind";
}

intrusive_ptr<DocumentSourceUnwind> DocumentSourceUnwind::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const string& unwindPath,
    bool preserveNullAndEmptyArrays,
    const boost::optional<string>& indexPath) {
    intrusive_ptr<DocumentSourceUnwind> source(
        new DocumentSourceUnwind(expCtx,
                                 FieldPath(unwindPath),
                                 preserveNullAndEmptyArrays,
                                 indexPath ? FieldPath(*indexPath) : boost::optional<FieldPath>()));
    source->injectExpressionContext(expCtx);
    return source;
}

boost::optional<Document> DocumentSourceUnwind::getNext() {
    pExpCtx->checkForInterrupt();

    boost::optional<Document> out = _unwinder->getNext();
    while (!out) {
        // No more elements in array currently being unwound. This will loop if the input
        // document is missing the unwind field or has an empty array.
        boost::optional<Document> input = pSource->getNext();
        if (!input)
            return boost::none;  // input exhausted

        // Try to extract an output document from the new input document.
        _unwinder->resetDocument(*input);
        out = _unwinder->getNext();
    }

    return out;
}

BSONObjSet DocumentSourceUnwind::getOutputSorts() {
    BSONObjSet out;
    std::string unwoundPath = getUnwindPath();
    BSONObjSet inputSort = pSource->getOutputSorts();

    for (auto&& sortObj : inputSort) {
        // Truncate each sortObj at the unwindPath.
        BSONObjBuilder outputSort;

        for (BSONElement fieldSort : sortObj) {
            if (fieldSort.fieldNameStringData() == unwoundPath) {
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

Pipeline::SourceContainer::iterator DocumentSourceUnwind::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get())) {
        std::set<std::string> fields = {_unwindPath.fullPath()};

        if (_indexPath) {
            fields.insert((*_indexPath).fullPath());
        }

        auto splitMatch = nextMatch->splitSourceBy(fields);

        invariant(splitMatch.first || splitMatch.second);

        if (!splitMatch.first && splitMatch.second) {
            // No optimization was possible.
            return std::next(itr);
        }

        container->erase(std::next(itr));

        // If splitMatch.second is not null, then there is a new $match stage to insert after
        // ourselves.
        if (splitMatch.second) {
            container->insert(std::next(itr), std::move(splitMatch.second));
        }

        if (splitMatch.first) {
            container->insert(itr, std::move(splitMatch.first));
            if (std::prev(itr) == container->begin()) {
                return std::prev(itr);
            }
            return std::prev(std::prev(itr));
        }
    }
    return std::next(itr);
}

Value DocumentSourceUnwind::serialize(bool explain) const {
    return Value(DOC(getSourceName() << DOC(
                         "path" << _unwindPath.fullPathWithPrefix() << "preserveNullAndEmptyArrays"
                                << (_preserveNullAndEmptyArrays ? Value(true) : Value())
                                << "includeArrayIndex"
                                << (_indexPath ? Value((*_indexPath).fullPath()) : Value()))));
}

DocumentSource::GetDepsReturn DocumentSourceUnwind::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(_unwindPath.fullPath());
    return SEE_NEXT;
}

intrusive_ptr<DocumentSource> DocumentSourceUnwind::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    // $unwind accepts either the legacy "{$unwind: '$path'}" syntax, or a nested document with
    // extra options.
    string prefixedPathString;
    bool preserveNullAndEmptyArrays = false;
    boost::optional<string> indexPath;
    if (elem.type() == Object) {
        for (auto&& subElem : elem.Obj()) {
            if (subElem.fieldNameStringData() == "path") {
                uassert(28808,
                        str::stream() << "expected a string as the path for $unwind stage, got "
                                      << typeName(subElem.type()),
                        subElem.type() == String);
                prefixedPathString = subElem.str();
            } else if (subElem.fieldNameStringData() == "preserveNullAndEmptyArrays") {
                uassert(28809,
                        str::stream() << "expected a boolean for the preserveNullAndEmptyArrays "
                                         "option to $unwind stage, got "
                                      << typeName(subElem.type()),
                        subElem.type() == Bool);
                preserveNullAndEmptyArrays = subElem.Bool();
            } else if (subElem.fieldNameStringData() == "includeArrayIndex") {
                uassert(28810,
                        str::stream() << "expected a non-empty string for the includeArrayIndex "
                                         " option to $unwind stage, got "
                                      << typeName(subElem.type()),
                        subElem.type() == String && !subElem.String().empty());
                indexPath = subElem.String();
                uassert(28822,
                        str::stream() << "includeArrayIndex option to $unwind stage should not be "
                                         "prefixed with a '$': "
                                      << (*indexPath),
                        (*indexPath)[0] != '$');
            } else {
                uasserted(28811,
                          str::stream() << "unrecognized option to $unwind stage: "
                                        << subElem.fieldNameStringData());
            }
        }
    } else if (elem.type() == String) {
        prefixedPathString = elem.str();
    } else {
        uasserted(
            15981,
            str::stream()
                << "expected either a string or an object as specification for $unwind stage, got "
                << typeName(elem.type()));
    }
    uassert(28812, "no path specified to $unwind stage", !prefixedPathString.empty());

    uassert(28818,
            str::stream() << "path option to $unwind stage should be prefixed with a '$': "
                          << prefixedPathString,
            prefixedPathString[0] == '$');
    string pathString(Expression::removeFieldPrefix(prefixedPathString));
    return DocumentSourceUnwind::create(pExpCtx, pathString, preserveNullAndEmptyArrays, indexPath);
}
}
