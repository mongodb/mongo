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

#include <boost/optional.hpp>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
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
    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        LiteParsed(NamespaceString fromNss,
                   stdx::unordered_set<NamespaceString> foreignNssSet,
                   boost::optional<LiteParsedPipeline> liteParsedPipeline)
            : _fromNss{std::move(fromNss)},
              _foreignNssSet(std::move(foreignNssSet)),
              _liteParsedPipeline(std::move(liteParsedPipeline)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return {_foreignNssSet};
        }

        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            PrivilegeVector requiredPrivileges;
            Privilege::addPrivilegeToPrivilegeVector(
                &requiredPrivileges,
                Privilege(ResourcePattern::forExactNamespace(_fromNss), ActionType::find));

            if (_liteParsedPipeline) {
                Privilege::addPrivilegesToPrivilegeVector(
                    &requiredPrivileges, _liteParsedPipeline->requiredPrivileges(isMongos));
            }

            return requiredPrivileges;
        }

    private:
        const NamespaceString _fromNss;
        const stdx::unordered_set<NamespaceString> _foreignNssSet;
        const boost::optional<LiteParsedPipeline> _liteParsedPipeline;
    };

    GetNextResult getNext() final;
    const char* getSourceName() const final;
    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    /**
     * Returns the 'as' path, and possibly fields modified by an absorbed $unwind.
     */
    GetModPathsReturn getModifiedPaths() const final;

    bool canSwapWithMatch() const final {
        return true;
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const final;

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
        invariant(!_unwindSrc);
        _unwindSrc = unwind;
    }

    /**
     * Returns true if DocumentSourceLookup was constructed with pipeline syntax (as opposed to
     * localField/foreignField syntax).
     */
    bool wasConstructedWithPipelineSyntax() const {
        return !static_cast<bool>(_localField);
    }

protected:
    void doDispose() final;

    /**
     * Attempts to combine with a subsequent $unwind stage, setting the internal '_unwindSrc'
     * field.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    struct LetVariable {
        LetVariable(std::string name, boost::intrusive_ptr<Expression> expression, Variables::Id id)
            : name(std::move(name)), expression(std::move(expression)), id(id) {}

        std::string name;
        boost::intrusive_ptr<Expression> expression;
        Variables::Id id;
    };

    /**
     * Target constructor. Handles common-field initialization for the syntax-specific delegating
     * constructors.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Constructor used for a $lookup stage specified using the {from: ..., localField: ...,
     * foreignField: ..., as: ...} syntax.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::string localField,
                         std::string foreignField,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Constructor used for a $lookup stage specified using the {from: ..., pipeline: [...], as:
     * ...} syntax.
     */
    DocumentSourceLookUp(NamespaceString fromNs,
                         std::string as,
                         std::vector<BSONObj> pipeline,
                         BSONObj letVariables,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Should not be called; use serializeToArray instead.
     */
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;
    }

    GetNextResult unwindResult();

    /**
     * Copies 'vars' and 'vps' to the Variables and VariablesParseState objects in 'expCtx'. These
     * copies provide access to 'let' defined variables in sub-pipeline execution.
     */
    static void copyVariablesToExpCtx(const Variables& vars,
                                      const VariablesParseState& vps,
                                      ExpressionContext* expCtx);

    /**
     * Resolves let defined variables against 'localDoc' and stores the results in 'variables'.
     */
    void resolveLetVariables(const Document& localDoc, Variables* variables);

    /**
     * The pipeline supplied via the $lookup 'pipeline' argument. This may differ from pipeline that
     * is executed in that it will not include optimizations or resolved views.
     */
    std::string getUserPipelineDefinition();

    NamespaceString _fromNs;
    NamespaceString _resolvedNs;
    FieldPath _as;
    boost::optional<BSONObj> _additionalFilter;

    // For use when $lookup is specified with localField/foreignField syntax.
    boost::optional<FieldPath> _localField;
    boost::optional<FieldPath> _foreignField;

    // Holds 'let' defined variables defined both in this stage and in parent pipelines. These are
    // copied to the '_fromExpCtx' ExpressionContext's 'variables' and 'variablesParseState' for use
    // in foreign pipeline execution.
    Variables _variables;
    VariablesParseState _variablesParseState;

    // The ExpressionContext used when performing aggregation pipelines against the '_resolvedNs'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline to perform against the '_resolvedNs' namespace. Referenced view
    // namespaces have been resolved.
    std::vector<BSONObj> _resolvedPipeline;
    // The aggregation pipeline defined with the user request, prior to optimization and view
    // resolution.
    std::vector<BSONObj> _userPipeline;

    std::vector<LetVariable> _letVariables;

    boost::intrusive_ptr<DocumentSourceMatch> _matchSrc;
    boost::intrusive_ptr<DocumentSourceUnwind> _unwindSrc;

    // The following members are used to hold onto state across getNext() calls when '_unwindSrc' is
    // not null.
    long long _cursorIndex = 0;
    std::unique_ptr<Pipeline, Pipeline::Deleter> _pipeline;
    boost::optional<Document> _input;
    boost::optional<Document> _nextValue;
};

}  // namespace mongo
