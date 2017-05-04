/**
 * Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * This class is for DocumentSources that take in and return one document at a time, in a 1:1
 * transformation. It should only be used via an alias that passes the transformation logic through
 * a ParsedSingleDocumentTransformation. It is not a registered DocumentSource, and it cannot be
 * created from BSON.
 */
class DocumentSourceSingleDocumentTransformation final : public DocumentSource {
public:
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
        virtual Document serialize(boost::optional<ExplainOptions::Verbosity> explain) const = 0;
        virtual DocumentSource::GetDepsReturn addDependencies(DepsTracker* deps) const = 0;
        virtual GetModPathsReturn getModifiedPaths() const = 0;

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

    DocumentSourceSingleDocumentTransformation(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        std::unique_ptr<TransformerInterface> parsedTransform,
        std::string name);

    // virtuals from DocumentSource
    const char* getSourceName() const final;
    GetNextResult getNext() final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    DocumentSource::GetDepsReturn getDependencies(DepsTracker* deps) const final;
    GetModPathsReturn getModifiedPaths() const final;

    bool canSwapWithMatch() const final {
        return true;
    }

    TransformerInterface::TransformerType getType() const {
        return _parsedTransform->getType();
    }

    bool isSubsetOfProjection(const BSONObj& proj) const {
        return _parsedTransform->isSubsetOfProjection(proj);
    }

protected:
    void doDispose() final;

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    // Stores transformation logic.
    std::unique_ptr<TransformerInterface> _parsedTransform;

    // Specific name of the transformation.
    std::string _name;
};

}  // namespace mongo
