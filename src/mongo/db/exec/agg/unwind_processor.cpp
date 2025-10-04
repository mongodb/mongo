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
