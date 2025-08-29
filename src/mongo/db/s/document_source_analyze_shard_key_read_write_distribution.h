/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution_gen.h"
#include "mongo/stdx/unordered_set.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace analyze_shard_key {

class DocumentSourceAnalyzeShardKeyReadWriteDistribution final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_analyzeShardKeyReadWriteDistribution"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options);

        explicit LiteParsed(std::string parseTimeName,
                            NamespaceString nss,
                            DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec)
            : LiteParsedDocumentSource(std::move(parseTimeName)), _nss(std::move(nss)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {
                Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::analyzeShardKey)};
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

    private:
        const NamespaceString _nss;
    };

    DocumentSourceAnalyzeShardKeyReadWriteDistribution(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec)
        : DocumentSource(kStageName, pExpCtx), _spec(std::move(spec)) {}

    ~DocumentSourceAnalyzeShardKeyReadWriteDistribution() override = default;

    StageConstraints constraints(PipelineSplitState = PipelineSplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed};

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    auto& getSpec() const {
        return _spec;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceAnalyzeShardKeyReadWriteDistribution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec _spec;
};

}  // namespace analyze_shard_key
}  // namespace mongo
