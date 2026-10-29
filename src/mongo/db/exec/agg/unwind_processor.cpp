// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/unwind_processor.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_unwind.h"

#include <memory>

namespace mongo {
namespace exec::agg {

using std::string;

UnwindProcessor::UnwindProcessor(const FieldPath& unwindPath,
                                 bool preserveNullAndEmptyArrays,
                                 const boost::optional<FieldPath>& indexPath,
                                 bool strict)
    : _unwindPath(unwindPath),
      _preserveNullAndEmptyArrays(preserveNullAndEmptyArrays),
      _indexPath(indexPath),
      _strict(strict),
      _conflictingPaths(indexPath ? indexPath->isPrefixOf(unwindPath) : false) {}

void UnwindProcessor::process(const Document& document) {
    // Reset document specific attributes.
    _output.reset(document);
    _unwindPathFieldIndexes.clear();
    _index = 0;
    _inputArray = document.getNestedField(_unwindPath, &_unwindPathFieldIndexes);
    _haveNext = true;
}

boost::optional<Document> UnwindProcessor::getNext() {
    // WARNING: Any functional changes to this method must also be implemented in the unwinding
    // implementation of the $lookup stage.
    if (!_haveNext) {
        return boost::none;
    }

    // Track which index this value came from. If 'includeArrayIndex' was specified, we will use
    // this index in the output document, or null if the value didn't come from an array.
    boost::optional<long long> indexForOutput;

    uassert(5858203,
            "an array is expected",
            (_strict && _inputArray.getType() == BSONType::array) || !_strict);
    if (_inputArray.getType() == BSONType::array) {
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
            // clones (or the original). If the array index path is a parent of the unwind path, we
            // ignore the array value since it would be overwritten by the index.
            if (!_conflictingPaths) {
                _output.setNestedField(_unwindPathFieldIndexes, _inputArray[_index]);
            }
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

}  // namespace exec::agg

std::unique_ptr<exec::agg::UnwindProcessor> createUnwindProcessorFromDocumentSource(
    const boost::intrusive_ptr<DocumentSourceUnwind>& ds) {
    return std::make_unique<exec::agg::UnwindProcessor>(
        ds->_unwindPath, ds->_preserveNullAndEmptyArrays, ds->_indexPath, ds->_strict);
}

}  // namespace mongo
