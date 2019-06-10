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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"

namespace mongo {

class DocumentSourcePlanCacheStats final : public DocumentSource {
public:
    static const char* kStageName;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec) {
            return std::make_unique<LiteParsed>(request.getNamespaceString());
        }

        explicit LiteParsed(NamespaceString nss) : _nss(std::move(nss)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            // There are no foreign collections.
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos) const override {
            return {Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::planCacheRead)};
        }

        bool isInitialSource() const final {
            return true;
        }

        bool allowedToForwardFromMongos() const override {
            // $planCacheStats must be run locally on a mongod.
            return false;
        }

        bool allowedToPassthroughFromMongos() const override {
            // $planCacheStats must be run locally on a mongod.
            return false;
        }

        void assertSupportsReadConcern(const repl::ReadConcernArgs& readConcern) const {
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Aggregation stage " << kStageName
                                  << " requires read concern local but found "
                                  << readConcern.toString(),
                    readConcern.getLevel() == repl::ReadConcernLevel::kLocalReadConcern);
        }

    private:
        const NamespaceString _nss;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    virtual ~DocumentSourcePlanCacheStats() = default;

    GetNextResult getNext() override;

    StageConstraints constraints(
        Pipeline::SplitState = Pipeline::SplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     // This stage must run on a mongod, and will fail at parse time
                                     // if an attempt is made to run it on mongos.
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed};

        constraints.requiresInputDocSource = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return kStageName;
    }

    /**
     * Absorbs a subsequent $match, in order to avoid copying the entire contents of the plan cache
     * prior to filtering.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) override;

    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const override;

private:
    DocumentSourcePlanCacheStats(const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const override {
        MONGO_UNREACHABLE;  // Should call serializeToArray instead.
    }

    // The result set for this change is produced through the mongo process interface on the first
    // call to getNext(), and then held by this data member.
    std::vector<BSONObj> _results;

    // Whether '_results' has been populated yet.
    bool _haveRetrievedStats = false;

    // Used to spool out '_results' as calls to getNext() are made.
    std::vector<BSONObj>::iterator _resultsIter;

    // $planCacheStats can push a match down into the plan cache layer, in order to avoid copying
    // the entire contents of the cache.
    boost::intrusive_ptr<DocumentSourceMatch> _absorbedMatch;
};

}  // namespace mongo
