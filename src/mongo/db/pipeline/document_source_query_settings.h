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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

/**
 * The $querySettings stage is an alias for a $queue stage containing all QueryShapeConfigurations
 * stored in 'querySettings' cluster parameter, followed by an optional $addFields stage.
 */
class DocumentSourceQuerySettings final {
public:
    static constexpr StringData kStageName = "$querySettings"_sd;
    static constexpr StringData kDebugQueryShapeFieldName = "debugQueryShape"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec) {
            uassert(7746800,
                    "$querySettings stage expects a document as argument",
                    spec.type() == BSONType::Object);
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

    /**
     * Returns the stage constraints used to override 'DocumentSourceQueue'. The 'kLocalOnly' host
     * type requirement is needed to ensure that the reported query settings are consistent with
     * what's present on the current node. Without this, it's possible that '$querySettings' might
     * report configurations which are present on 'mongod' instances, but not yet present on
     * 'mongos' ones and consequently won't be enforced.
     */
    static StageConstraints constraints() {
        StageConstraints constraints{DocumentSource::StreamType::kStreaming,
                                     DocumentSource::PositionRequirement::kFirst,
                                     DocumentSource::HostTypeRequirement::kLocalOnly,
                                     DocumentSource::DiskUseRequirement::kNoDiskUse,
                                     DocumentSource::FacetRequirement::kNotAllowed,
                                     DocumentSource::TransactionRequirement::kAllowed,
                                     DocumentSource::LookupRequirement::kAllowed,
                                     DocumentSource::UnionRequirement::kAllowed};
        constraints.requiresInputDocSource = false;
        constraints.isIndependentOfAnyCollection = true;
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
};

}  // namespace mongo
