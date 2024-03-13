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
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/explain_options.h"

namespace mongo {
/**
 * This class defines the minimal interface that every parser wishing to take advantage of
 * DocumentSourceSingleDocumentTransformation must implement.
 *
 * This interface ensures that DocumentSourceSingleDocumentTransformations are passed parsed
 * objects that can execute the transformation and provide additional features like
 * serialization and reporting and returning dependencies. The parser must also provide
 * implementations for optimizing and adding the expression context, even if those functions do
 * nothing.
 */
class TransformerInterface {
public:
    enum class TransformerType {
        kExclusionProjection,
        kInclusionProjection,
        kComputedProjection,
        kReplaceRoot,
        kGroupFromFirstDocument,
    };
    virtual ~TransformerInterface() = default;
    virtual Document applyTransformation(const Document& input) const = 0;
    virtual TransformerType getType() const = 0;
    virtual void optimize() = 0;
    virtual DepsTracker::State addDependencies(DepsTracker* deps) const = 0;
    virtual void addVariableRefs(std::set<Variables::Id>* refs) const = 0;
    virtual DocumentSource::GetModPathsReturn getModifiedPaths() const = 0;

    /**
     * Returns a document describing this transformation. For example, this function will return
     * {_id: 0, x: 1} for the stage parsed from {$project: {_id: 0, x: 1}}.
     */
    virtual Document serializeTransformation(boost::optional<ExplainOptions::Verbosity> explain,
                                             const SerializationOptions& options = {}) const = 0;

    /**
     * Method used by inclusion and add fields projecton executors to extract computed projections
     * that depend only on the 'oldName' field. Returns a pair of <BSONObj, bool>. The BSONObj
     * contains the extracted projections. The boolean flag is true if the original projection has
     * become empty after the extraction and can be deleted by the caller.
     */
    virtual std::pair<BSONObj, bool> extractComputedProjections(
        StringData oldName, StringData newName, const std::set<StringData>& reservedNames) {
        return {BSONObj{}, false};
    }

    virtual std::pair<BSONObj, bool> extractProjectOnFieldAndRename(StringData oldName,
                                                                    StringData newName) {
        return {BSONObj{}, false};
    }
};
}  // namespace mongo
