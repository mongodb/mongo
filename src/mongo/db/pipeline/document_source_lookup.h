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
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lookup_set_cache.h"
#include "mongo/db/pipeline/value_comparator.h"

namespace mongo {

/**
 * Queries separate collection for equality matches with documents in the pipeline collection.
 * Adds matching documents to a new array field in the input document.
 */
class DocumentSourceLookUp final : public DocumentSourceNeedsMongod,
                                   public SplittableDocumentSource {
public:
    static std::unique_ptr<LiteParsedDocumentSourceOneForeignCollection> liteParse(
        const AggregationRequest& request, const BSONElement& spec);

    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void serializeToArray(std::vector<Value>& array, bool explain = false) const final;

    /**
     * Returns the 'as' path, and possibly fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    bool canSwapWithMatch() const final {
        return true;
    }

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwindSrc'
     * field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    GetDepsReturn getDependencies(DepsTracker* deps) const final;
    void dispose() final;

    BSONObjSet getOutputSorts() final {
        return DocumentSource::truncateSortSet(pSource->getOutputSorts(), {_as.fullPath()});
    }

    bool needsPrimaryShard() const final {
        return true;
    }

    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }

    boost::intrusive_ptr<DocumentSource> getMergeSource() final {
        return this;
    }

    void addInvolvedCollections(std::vector<NamespaceString>* collections) const final {
        collections->push_back(_fromNs);
    }

    void doDetachFromOperationContext() final;

    void doReattachToOperationContext(OperationContext* opCtx) final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Builds the BSONObj used to query the foreign collection and wraps it in a $match.
     */
    static BSONObj makeMatchStageFromInput(const Document& input,
                                           const FieldPath& localFieldName,
                                           const std::string& foreignFieldName,
                                           const BSONObj& additionalFilter);

    /**
     * Helper to absorb an $unwind stage. Only used for testing this special behavior.
     */
    void setUnwindStage(const boost::intrusive_ptr<DocumentSourceUnwind>& unwind) {
        invariant(!_handlingUnwind);
        _unwindSrc = unwind;
        _handlingUnwind = true;
    }

private:
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::string localField,
                         std::string foreignField,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(bool explain = false) const final {
        // Should not be called; use serializeToArray instead.
        MONGO_UNREACHABLE;
    }

    GetNextResult unwindResult();

    NamespaceString _fromNs;
    FieldPath _as;
    FieldPath _localField;
    FieldPath _foreignField;
    std::string _foreignFieldFieldName;
    boost::optional<BSONObj> _additionalFilter;

    // The ExpressionContext used when performing aggregation pipelines against the '_fromNs'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline to perform against the '_fromNs' namespace.
    std::vector<BSONObj> _fromPipeline;

    boost::intrusive_ptr<DocumentSourceMatch> _matchSrc;
    boost::intrusive_ptr<DocumentSourceUnwind> _unwindSrc;

    bool _handlingUnwind = false;
    bool _handlingMatch = false;

    // The following members are used to hold onto state across getNext() calls when
    // '_handlingUnwind' is true.
    long long _cursorIndex = 0;
    boost::intrusive_ptr<Pipeline> _pipeline;
    boost::optional<Document> _input;
    boost::optional<Document> _nextValue;
};

}  // namespace mongo
