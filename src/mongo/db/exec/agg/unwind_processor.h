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

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/field_path.h"

#include <boost/optional.hpp>

namespace mongo::exec::agg {

// This class is used by the aggregation framework and streams enterprise module
// to perform the document processing needed for $unwind.
class UnwindProcessor {
public:
    UnwindProcessor(const FieldPath& unwindPath,
                    bool preserveNullAndEmptyArrays,
                    const boost::optional<FieldPath>& indexPath,
                    bool strict);

    // Reset the processor to unwind a new document.
    void process(const Document& document);

    // Returns the next document unwound from the document provided to process().
    // Returns boost::none if the array is exhausted.
    boost::optional<Document> getNext();

private:
    const FieldPath _unwindPath;
    // Documents that have a nullish value, or an empty array for the field '_unwindPath', will pass
    // through the $unwind stage unmodified if '_preserveNullAndEmptyArrays' is true.
    const bool _preserveNullAndEmptyArrays{false};
    // If set, the $unwind stage will include the array index in the specified path, overwriting any
    // existing value, setting to null when the value was a non-array or empty array.
    const boost::optional<FieldPath> _indexPath;

    // Tracks whether or not we can possibly return any more documents. Note we may return
    // boost::none even if this is true.
    bool _haveNext{false};

    // Specifies if input to $unwind is required to be an array.
    bool _strict{false};

    Value _inputArray;

    MutableDocument _output;

    // Document indexes of the field path components.
    std::vector<Position> _unwindPathFieldIndexes;

    // Index into the _inputArray to return next.
    size_t _index{0};

    // True if we are including the array index and it's path is a parent of the unwind path. If
    // this is true, we will just return the array indices and ignore the array values.
    const bool _conflictingPaths;
};

}  // namespace mongo::exec::agg
