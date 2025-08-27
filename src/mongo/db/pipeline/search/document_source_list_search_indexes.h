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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source.h"

#include <boost/optional/optional.hpp>

namespace mongo {
class DocumentSourceListSearchIndexesSpec;

class DocumentSourceListSearchIndexes final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$listSearchIndexes"_sd;
    static constexpr StringData kCursorFieldName = "cursor"_sd;
    static constexpr StringData kFirstBatchFieldName = "firstBatch"_sd;

    static void validateListSearchIndexesSpec(const DocumentSourceListSearchIndexesSpec* spec);
    /**
     * A 'LiteParsed' representation of the $listSearchIndexes stage.
     */
    class LiteParsedListSearchIndexes final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsedListSearchIndexes> parse(
            const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
            return std::make_unique<LiteParsedListSearchIndexes>(spec.fieldName(), nss);
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            // There are no foreign namespaces.
            return stdx::unordered_set<NamespaceString>{};
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {Privilege(ResourcePattern::forDatabaseName(_nss.dbName()),
                              ActionType::listSearchIndexes)};
        }

        bool isInitialSource() const final {
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return onlyReadConcernLocalSupported(getParseTimeName(), level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(getParseTimeName());
        }

        bool isSearchStage() const final {
            return true;
        }

        explicit LiteParsedListSearchIndexes(std::string parseTimeName, NamespaceString nss)
            : LiteParsedDocumentSource(std::move(parseTimeName)), _nss(std::move(nss)) {}

    private:
        const NamespaceString _nss;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceListSearchIndexes(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                    BSONObj cmdObj)
        : DocumentSource(kStageName, pExpCtx), _cmdObj(cmdObj.getOwned()) {}

    const char* getSourceName() const override {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceListSearchIndexesToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    BSONObj _cmdObj;
};

}  // namespace mongo
