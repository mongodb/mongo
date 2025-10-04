/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class MONGO_MOD_NEEDS_REPLACEMENT SortPattern {
public:
    enum class SortKeySerialization {
        kForExplain,
        kForPipelineSerialization,
        kForSortKeyMerging,
    };

    // Represents one of the components in a compound sort pattern. Each component is either the
    // field path by which we are sorting, or an Expression which can be used to retrieve the sort
    // value in the case of a $meta-sort (but not both).
    struct SortPatternPart {
        bool isAscending = true;
        boost::optional<FieldPath> fieldPath;
        boost::intrusive_ptr<ExpressionMeta> expression;

        bool operator==(const SortPatternPart& other) const {
            return isAscending == other.isAscending && fieldPath == other.fieldPath &&
                expression == other.expression;
        }
        bool operator!=(const SortPatternPart& other) const {
            return !(*this == other);
        }
    };

    SortPattern(const BSONObj&, const boost::intrusive_ptr<ExpressionContext>&);

    SortPattern(std::vector<SortPatternPart> patterns) : _sortPattern(std::move(patterns)) {
        for (auto&& patternPart : _sortPattern) {
            if (patternPart.fieldPath) {
                const auto [_, inserted] = _paths.insert(patternPart.fieldPath->fullPath());
                uassert(7472501,
                        str::stream() << "$sort key must not contain duplicate keys (field: '"
                                      << patternPart.fieldPath->fullPath() << "')",
                        inserted);
            }
        }
    }

    /**
     * Write out a Document whose contents are the sort key pattern.
     */
    Document serialize(SortKeySerialization serializationMode,
                       const SerializationOptions& options = {}) const;

    /**
     * Serializes the document to BSON, only keeping the paths specified in the sort pattern.
     */
    BSONObj documentToBsonWithSortPaths(const Document& doc) const {
        return document_path_support::documentToBsonWithPaths(doc, _paths);
    }

    size_t size() const {
        return _sortPattern.size();
    }

    bool empty() const {
        return _sortPattern.empty();
    }

    void addDependencies(DepsTracker* deps) const;

    /**
     * Singleton sort patterns are a special case. In memory, sort keys for singleton patterns get
     * stored as a single Value, corresponding to the single component of the sort pattern. By
     * contrast, sort patterns for "compound" sort keys get stored as a Value that is an array,
     * with one element for each component of the sort.
     */
    bool isSingleElementKey() const {
        return _sortPattern.size() == 1;
    }

    const SortPatternPart& operator[](int idx) const {
        return _sortPattern[idx];
    }

    /**
     * Returns true if this SortPattern is an extension of the other.
     */
    bool isExtensionOf(const SortPattern& other) const;

    bool operator==(const SortPattern& other) const {
        return _sortPattern == other._sortPattern && _paths == other._paths;
    }

    bool operator!=(const SortPattern& other) const {
        return !(*this == other);
    }

    std::vector<SortPatternPart>::const_iterator begin() const {
        return _sortPattern.cbegin();
    }

    std::vector<SortPatternPart>::const_iterator end() const {
        return _sortPattern.cend();
    }

    /**
     * Return first sort pattern.
     */
    const SortPatternPart& front() const {
        return _sortPattern.front();
    }

    /**
     * Return last sort pattern.
     */
    const SortPatternPart& back() const {
        return _sortPattern.back();
    }

    /**
     * Returns the types of metadata depended on by this sort.
     *
     * The caller can optionally supply a description of the types of metadata that are not
     * available. In this case, throws a UserException if any unavailable metadata type is also a
     * metadata dependency.
     */
    QueryMetadataBitSet metadataDeps(DepsTracker::MetadataDependencyValidation availableMetadata =
                                         DepsTracker::NoMetadataValidation()) const;

private:
    std::vector<SortPatternPart> _sortPattern;

    // The set of paths on which we're sorting.
    OrderedPathSet _paths;
};

/**
 * Returns true if 'sortPattern' represents a sort pattern on a single metadata field like:
 * {score: {$meta: "searchScore"}}.
 *
 * Sort clause must only be on a single field, i.e. {score: {$meta: "searchScore"}, _id: 1} will
 * return false.
 *
 * The 'metadataToConsider' field represents a bitset of all possible metadata fields to consider
 * the sort is on. If the bitset is empty, any metadata will be considered.
 */
bool isSortOnSingleMetaField(const SortPattern& sortPattern,
                             QueryMetadataBitSet metadataToConsider = QueryMetadataBitSet{});
}  // namespace mongo
