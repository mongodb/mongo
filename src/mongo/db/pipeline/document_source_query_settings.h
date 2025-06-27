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

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/util/deferred.h"

namespace mongo {
using QueryShapeConfigurationMap = stdx::unordered_map<query_shape::QueryShapeHash,
                                                       query_settings::QueryShapeConfiguration,
                                                       QueryShapeHashHasher>;

/**
 * The $querySettings stage returns all QueryShapeConfigurations stored in the cluster.
 */
class DocumentSourceQuerySettings final : public DocumentSource, public exec::agg::Stage {
public:
    static constexpr StringData kStageName = "$querySettings"_sd;
    static constexpr StringData kDebugQueryShapeFieldName = "debugQueryShape"_sd;
    static const Id& id;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * The possible states of the stage. First we are reading all the 'config.representativeQueries'
     * collection and return all QueryShapeConfigurations that have representative queries. Once the
     * cursor is exhausted, we iterate over the remaining configurations in the
     * '_queryShapeConfigsMap'.
     */
    enum class State {
        kReadingFromQueryShapeRepresentativeQueriesColl,
        kReadingFromQueryShapeConfigsMap
    };

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            uassert(7746800,
                    "$querySettings stage expects a document as argument",
                    spec.type() == BSONType::object);
            return std::make_unique<LiteParsed>(spec.fieldName(), nss.tenantId());
        }

        LiteParsed(std::string parseTimeName, const boost::optional<TenantId>& tenantId)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
              _privileges({Privilege(ResourcePattern::forClusterResource(tenantId),
                                     ActionType::querySettings)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        bool generatesOwnDataOnce() const final {
            return true;
        }

        bool isInitialSource() const override {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

    private:
        const PrivilegeVector _privileges;
    };

    DocumentSourceQuerySettings(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                bool showDebugQueryShape);

    /**
     * Returns the stage constraints used to override 'DocumentSourceQueue'. The 'kLocalOnly' host
     * type requirement is needed to ensure that the reported query settings are consistent with
     * what's present on the current node. Without this, it's possible that '$querySettings' might
     * report configurations which are present on 'mongod' instances, but not yet present on
     * 'mongos' ones and consequently won't be enforced.
     */
    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints{DocumentSource::StreamType::kStreaming,
                                     DocumentSource::PositionRequirement::kFirst,
                                     DocumentSource::HostTypeRequirement::kLocalOnly,
                                     DocumentSource::DiskUseRequirement::kNoDiskUse,
                                     DocumentSource::FacetRequirement::kNotAllowed,
                                     DocumentSource::TransactionRequirement::kAllowed,
                                     DocumentSource::LookupRequirement::kAllowed,
                                     DocumentSource::UnionRequirement::kAllowed};
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    const char* getSourceName() const final {
        return kStageName.data();
    }

    Id getId() const override {
        return id;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

private:
    GetNextResult doGetNext() final;
    void doDispose() final;

    /**
     * DocumentSource that holds a cursor over the 'queryShapeRepresentativeQueries' collection:
     * - either on local node if we are in the replica set
     * - or on the configsvr if we are in the sharded cluster deployment.
     *
     * If gFeatureFlagPQSBackfill is disabled, returns empty DocumentSourceQueue.
     */
    boost::intrusive_ptr<DocumentSource> createQueryShapeRepresentativeQueriesCursor(
        OperationContext* opCtx);

    DeferredFn<QueryShapeConfigurationMap> _queryShapeConfigsMap{[this]() {
        // Get all query shape configurations owned by 'tenantId'.
        auto tenantId = getContext()->getNamespaceString().tenantId();
        auto* opCtx = getContext()->getOperationContext();
        auto configs = query_settings::QuerySettingsService::get(opCtx)
                           .getAllQueryShapeConfigurations(tenantId)
                           .queryShapeConfigurations;

        QueryShapeConfigurationMap map;
        for (auto&& config : configs) {
            map.emplace(config.getQueryShapeHash(), std::move(config));
        }
        return map;
    }};
    QueryShapeConfigurationMap::const_iterator _iterator;

    DeferredFn<boost::intrusive_ptr<exec::agg::Stage>> _queryShapeRepresentativeQueriesCursor{
        [this]() {
            return exec::agg::buildStage(
                createQueryShapeRepresentativeQueriesCursor(getContext()->getOperationContext()));
        }};

    bool _showDebugQueryShape;
    State _state = State::kReadingFromQueryShapeRepresentativeQueriesColl;
};

}  // namespace mongo
