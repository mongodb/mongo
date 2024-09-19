/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_query_stats_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/producer_consumer_queue.h"

namespace mongo {

using namespace query_stats;

class DocumentSourceQueryStats final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$queryStats"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec);

        LiteParsed(std::string parseTimeName,
                   const boost::optional<TenantId>& tenantId,
                   TransformAlgorithmEnum algorithm,
                   std::string hmacKey)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
              _algorithm(algorithm),
              _hmacKey(hmacKey),
              _privileges(
                  algorithm == TransformAlgorithmEnum::kNone
                      ? PrivilegeVector{Privilege(ResourcePattern::forClusterResource(tenantId),
                                                  ActionType::queryStatsReadTransformed),
                                        Privilege(ResourcePattern::forClusterResource(tenantId),
                                                  ActionType::queryStatsRead)}
                      : PrivilegeVector{Privilege(ResourcePattern::forClusterResource(tenantId),
                                                  ActionType::queryStatsReadTransformed)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

        bool _transformIdentifiers;

        const TransformAlgorithmEnum _algorithm;

        std::string _hmacKey;

        const PrivilegeVector _privileges;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    ~DocumentSourceQueryStats() override = default;

    StageConstraints constraints(
        Pipeline::SplitState = Pipeline::SplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kLocalOnly,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed};

        constraints.requiresInputDocSource = false;
        constraints.isIndependentOfAnyCollection = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    DocumentSourceType getType() const override {
        return DocumentSourceType::kQueryStats;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    /*
     * CopiedPartition: This struct is representative of a copied ("materialized") partition
     * which should be loaded from the QueryStatsStore. It is used to hold a copy of the
     * QueryStatsEntries corresponding to the provided partitionId.
     * Once a CopiedPartition has been loaded from QueryStatsStore, it provides access to the
     * QueryStatsEntries of the partition without requiring holding the lock over the partition in
     * the partitioned cache.
     */
    struct CopiedPartition {
        CopiedPartition(QueryStatsStore::PartitionId partitionId)
            : statsEntries(), _readTimestamp(), _partitionId(partitionId) {}

        ~CopiedPartition() = default;

        bool isLoaded() const;

        void incrementPartitionId();

        bool isValidPartitionId(QueryStatsStore::PartitionId maxNumPartitions) const;

        const Date_t& getReadTimestamp() const;

        bool empty() const;

        void load(QueryStatsStore& queryStatsStore);

        std::deque<QueryStatsEntry> statsEntries;

    private:
        Date_t _readTimestamp;
        QueryStatsStore::PartitionId _partitionId;
        bool _isLoaded{false};
    };

    DocumentSourceQueryStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             TransformAlgorithmEnum algorithm = TransformAlgorithmEnum::kNone,
                             std::string hmacKey = {})
        : DocumentSource(kStageName, expCtx),
          _currentCopiedPartition(0),
          _transformIdentifiers(algorithm != TransformAlgorithmEnum::kNone),
          _algorithm(algorithm),
          _hmacKey(hmacKey) {}

    BSONObj computeQueryStatsKey(std::shared_ptr<const Key> key,
                                 const SerializationContext& serializationContext) const;

    GetNextResult doGetNext() final;

    boost::optional<Document> toDocument(const Date_t& partitionReadTime,
                                         const QueryStatsEntry& queryStatsEntry) const;

    // The current partition copied from query stats store to avoid holding lock during reads.
    CopiedPartition _currentCopiedPartition;

    // When true, apply hmac to field names from returned query shapes.
    bool _transformIdentifiers;

    // The type of algorithm to use for transform identifiers as an enum, currently only
    // kHmacSha256
    // ("hmac-sha-256") is supported.
    const TransformAlgorithmEnum _algorithm;

    /**
     * Key used for SHA-256 HMAC application on field names.
     */
    std::string _hmacKey;
};

}  // namespace mongo
