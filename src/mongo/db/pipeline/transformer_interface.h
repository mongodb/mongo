/**
 * Copyright (C) 2018 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
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
    };
    virtual ~TransformerInterface() = default;
    virtual Document applyTransformation(const Document& input) = 0;
    virtual TransformerType getType() const = 0;
    virtual void optimize() = 0;
    virtual DepsTracker::State addDependencies(DepsTracker* deps) const = 0;
    virtual DocumentSource::GetModPathsReturn getModifiedPaths() const = 0;

    /**
     * Returns a document describing this transformation. For example, this function will return
     * {_id: 0, x: 1} for the stage parsed from {$project: {_id: 0, x: 1}}.
     */
    virtual Document serializeTransformation(
        boost::optional<ExplainOptions::Verbosity> explain) const = 0;

    /**
     * Returns true if this transformer is an inclusion projection and is a subset of
     * 'proj', which must be a valid projection specification. For example, if this
     * TransformerInterface represents the inclusion projection
     *
     *      {a: 1, b: 1, c: 1}
     *
     * then it is a subset of the projection {a: 1, c: 1}, and this function returns
     * true.
     */
    virtual bool isSubsetOfProjection(const BSONObj& proj) const {
        return false;
    }
};
}  // namespace mongo
