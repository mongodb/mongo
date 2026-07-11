// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo::exec::agg {

/**
 * This class is used by the aggregation framework and streams enterprise module to perform the
 * document processing needed for $unwind.
 */
class [[MONGO_MOD_PUBLIC]] UnwindProcessor {
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

    // Returns true if the processor may have more results from the current array.
    bool haveNext() const {
        return _haveNext;
    }

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
