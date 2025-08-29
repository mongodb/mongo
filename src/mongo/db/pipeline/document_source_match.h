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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/match_processor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceMatch : public DocumentSource {
public:
    static bool containsTextOperator(const MatchExpression& expr);

    DocumentSourceMatch(std::unique_ptr<MatchExpression> expr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    ~DocumentSourceMatch() override = default;

    static constexpr StringData kStageName = "$match"_sd;
    /**
     * Convenience method for creating a $match stage.
     */
    static boost::intrusive_ptr<DocumentSourceMatch> create(
        BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses a $match stage from 'elem'.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    /**
     * Returns a new DocumentSourceMatch with a MatchExpression that, if executed on the
     * sub-document at 'path', is equivalent to 'expression'.
     *
     * For example, if the original expression is {$and: [{'a.b': {$gt: 0}}, {'a.d': {$eq: 3}}]},
     * the new $match will have the expression {$and: [{b: {$gt: 0}}, {d: {$eq: 3}}]} after
     * descending on the path 'a'.
     *
     * Should be called _only_ on a MatchExpression that is a predicate on 'path', or subfields of
     * 'path'. It is also invalid to call this method on an expression including a $elemMatch on
     * 'path', for example: {'path': {$elemMatch: {'subfield': 3}}}
     */
    static boost::intrusive_ptr<DocumentSourceMatch> descendMatchOnPath(
        const MatchExpression* matchExpr,
        const std::string& path,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Returns a pair of pointers to $match stages, either of which can be null. The first entry in
     * the pair is a $match stage that can be moved before this stage, the second is a $match stage
     * that must remain after this stage.
     */
    static std::pair<boost::intrusive_ptr<DocumentSourceMatch>,
                     boost::intrusive_ptr<DocumentSourceMatch>>
    splitMatchByModifiedFields(const boost::intrusive_ptr<DocumentSourceMatch>& match,
                               const DocumentSource::GetModPathsReturn& modifiedPathsRet);

    /**
     * Parses 'filter' and resets the member of this source to be consistent with the new
     * MatchExpression. Takes ownership of 'filter'.
     */
    void rebuild(BSONObj filter);

    boost::intrusive_ptr<DocumentSource> optimize() final;

    const char* getSourceName() const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed,
                                ChangeStreamRequirement::kAllowlist};
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const override;

    /**
     * Attempts to combine with any subsequent $match stages, joining the query objects with a
     * $and and flattening top-level $and's in the process.
     */
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) override;

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // This stage does not modify or rename any paths.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    MatchProcessor* getMatchProcessor() {
        return _matchProcessor.get();
    }

    /**
     * Access the MatchExpression stored inside the DocumentSourceMatch. Does not release
     * ownership.
     */
    MatchExpression* getMatchExpression() const {
        return _matchProcessor->getExpression().get();
    }

    /**
     * Combines the filter in this $match with the filter of 'other' using a specified join
     * predicate, updating this match in place. This uses the stages' 'MatchExpression's, as those
     * are kept up to date during any optimizations. Currently, the join predicate can only be
     * either 'MatchExpression::MatchType::AND' or 'MatchExpression::MatchType::OR'.
     */
    void joinMatchWith(boost::intrusive_ptr<DocumentSourceMatch> other,
                       MatchExpression::MatchType joinPred);


    bool hasQuery() const override;

    /**
     * Returns the query in MatchExpression syntax.
     */
    BSONObj getQuery() const override;


    /** Returns the portion of the match that can safely be promoted to before a $redact.
     *  If this returns an empty BSONObj, no part of this match may safely be promoted.
     *
     *  To be safe to promote, removing a field from a document to be matched must not cause
     *  that document to be accepted when it would otherwise be rejected. As an example,
     *  {name: {$ne: "bob smith"}} accepts documents without a name field, which means that
     *  running this filter before a redact that would remove the name field would leak
     *  information. On the other hand, {age: {$gt:5}} is ok because it doesn't accept documents
     *  that have had their age field removed.
     */
    BSONObj redactSafePortion() const;

    static bool isTextQuery(const BSONObj& query);
    bool isTextQuery() const {
        return _isTextQuery;
    }

    /**
     * Attempt to split this $match into two stages, where the first is not dependent upon any
     * path from 'fields', and where applying them in sequence is equivalent to applying this
     * stage once.
     *
     * Will return two intrusive_ptrs to new $match stages, where the first pointer is
     * independent of 'fields', and the second is dependent. Either pointer may be null, so be
     * sure to check the return value.
     *
     * For example, {$match: {a: "foo", "b.c": 4}} split by "b" will return pointers to two
     * stages:
     * {$match: {a: "foo"}}, and {$match: {"b.c": 4}}.
     *
     * The 'renames' structure maps from a field to an alias that should be used in the
     * independent portion of the match. For example, suppose that we split by fields "a" with
     * the rename "b" => "c". The match {$match: {a: "foo", b: "bar", z: "baz"}} will split into
     * {$match: {c: "bar", z: "baz"}} and {$match: {a: "foo"}}.
     */
    std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
    splitSourceBy(const OrderedPathSet& fields, const StringMap<std::string>& renames) &&;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

protected:
    DocumentSourceMatch(const DocumentSourceMatch& other,
                        const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
        : DocumentSourceMatch(
              other.serialize().getDocument().toBson().firstElement().embeddedObject(),
              newExpCtx ? newExpCtx : other.getExpCtx()) {}

    DocumentSourceMatch(const BSONObj& query,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    const BSONObj& getPredicate() const {
        return _matchProcessor->getPredicate();
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceMatchToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    void rebuild(BSONObj predicate, std::unique_ptr<MatchExpression> expr);

    DepsTracker::State getDependencies(const MatchExpression* expr, DepsTracker* deps) const;

    std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
    splitSourceByFunc(const OrderedPathSet& fields,
                      const StringMap<std::string>& renames,
                      expression::ShouldSplitExprFunc func) &&;

    std::shared_ptr<MatchProcessor> _matchProcessor;
    SbeCompatibility _sbeCompatibility{SbeCompatibility::notCompatible};
    bool _isTextQuery{false};
};

/**
 * A DocumentSource class for all internal change stream stages that are also match stages. This
 * currently handles parsing for query stats.
 */
class DocumentSourceInternalChangeStreamMatch : public DocumentSourceMatch {
public:
    DocumentSourceInternalChangeStreamMatch(std::unique_ptr<MatchExpression> expr,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMatch(std::move(expr), expCtx) {}

    static boost::intrusive_ptr<DocumentSourceInternalChangeStreamMatch> create(
        BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Must override the serialize method, since internal change stream stages are serialized
     * differently than match stages. This function mirrors
     * DocumentSourceInternalChangeStreamStage::serialize and was added because this class cannot
     * inherit from both DocumentSourceInternalChangeStreamStage and DocumentSourceMatch.
     */
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    virtual Value doSerialize(const SerializationOptions& opts) const {
        return DocumentSourceMatch::serialize(opts);
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override;

protected:
    DocumentSourceInternalChangeStreamMatch(const BSONObj& query,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMatch(query, expCtx) {}

    DocumentSourceInternalChangeStreamMatch(
        const DocumentSourceInternalChangeStreamMatch& other,
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
        : DocumentSourceMatch(
              other.serialize().getDocument().toBson().firstElement().embeddedObject(),
              newExpCtx ? newExpCtx : other.getExpCtx()) {}
};

}  // namespace mongo
