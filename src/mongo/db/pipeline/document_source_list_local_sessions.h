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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/session/logical_session_cache.h"

namespace mongo {

ListSessionsSpec listSessionsParseSpec(StringData stageName, const BSONElement& spec);
PrivilegeVector listSessionsRequiredPrivileges(const ListSessionsSpec& spec);
std::vector<SHA256Block> listSessionsUsersToDigests(const std::vector<ListSessionsUser>& users);

/**
 * Produces one document per session in the local cache if 'allUsers' is specified
 * as true, and returns just sessions for the currently logged in user if
 * 'allUsers' is specified as false, or not specified at all.
 */
class DocumentSourceListLocalSessions final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$listLocalSessions"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec) {

            return std::make_unique<LiteParsed>(
                spec.fieldName(),
                listSessionsParseSpec(DocumentSourceListLocalSessions::kStageName, spec));
        }

        explicit LiteParsed(std::string parseTimeName, const ListSessionsSpec& spec)
            : LiteParsedDocumentSource(std::move(parseTimeName)), _spec(spec) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return listSessionsRequiredPrivileges(_spec);
        }

        bool isInitialSource() const final {
            return true;
        }

        bool allowedToPassthroughFromMongos() const final {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const {
            return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const {
            transactionNotSupported(DocumentSourceListLocalSessions::kStageName);
        }

    private:
        const ListSessionsSpec _spec;
    };

    const char* getSourceName() const final {
        return DocumentSourceListLocalSessions::kStageName.rawData();
    }

    Value serialize(SerializationOptions opts = SerializationOptions()) const final override {
        if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
            MONGO_UNIMPLEMENTED_TASSERT(7484328);
        }
        return Value(Document{{getSourceName(), _spec.toBSON()}});
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kLocalOnly,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kNotAllowed);

        constraints.isIndependentOfAnyCollection = true;
        constraints.requiresInputDocSource = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceListLocalSessions(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                    const ListSessionsSpec& spec);

    GetNextResult doGetNext() final;

    const ListSessionsSpec _spec;
    const LogicalSessionCache* _cache;
    std::vector<LogicalSessionId> _ids;
};

}  // namespace mongo
