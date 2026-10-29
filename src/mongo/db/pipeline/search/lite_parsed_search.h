// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

static constexpr std::string_view kReturnStoredSourceFieldName = "returnStoredSource"sv;
static constexpr std::string_view kScoreDetailsFieldName = "scoreDetails"sv;

/**
 * A 'LiteParsed' representation of a search stage. This is the parent class for the
 * $listSearchIndexes stage.
 */
template <typename Derived>
class LiteParsedSearchStage : public LiteParsedDocumentSourceDefault<Derived> {
public:
    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
        // There are no foreign namespaces.
        return stdx::unordered_set<NamespaceString>{};
    }

    PrivilegeVector requiredPrivileges(bool isMongos,
                                       bool bypassDocumentValidation) const override {
        return {Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::find)};
    }

    bool isInitialSource() const final {
        return true;
    }

    bool isSearchStage() const final {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return this->onlyReadConcernLocalSupported(
            this->getParseTimeName(), level, isImplicitDefault);
    }

    void assertSupportsMultiDocumentTransaction() const override {
        this->transactionNotSupported(this->getParseTimeName());
    }

    explicit LiteParsedSearchStage(const BSONElement& spec, NamespaceString nss)
        : LiteParsedDocumentSourceDefault<Derived>(spec), _nss(std::move(nss)) {}

    bool requiresAuthzChecks() const override {
        return true;
    }

    void validate(const OperationContext* opCtx) const override {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << this->getParseTimeName() << " value must be an object. Found: "
                              << typeName(this->_originalBson.type()),
                this->_originalBson.type() == BSONType::object);

        search_helpers::validateInternalSearchFieldsNotSetByUser(
            opCtx, this->_originalBson.embeddedObject());
    }

    // All search stages are unsupported on timeseries collections.
    LiteParsedDocumentSource::Constraints constraints() const override {
        return {.canRunOnTimeseries = false};
    }

protected:
    // Returns true if the stage spec has returnStoredSource: true, which applies an implicit
    // projection that modifies output fields.
    bool hasReturnStoredSource() const {
        if (this->_originalBson.type() == BSONType::object) {
            auto specObj = this->_originalBson.Obj();
            if (specObj.hasField(kReturnStoredSourceFieldName)) {
                auto rss = specObj[kReturnStoredSourceFieldName];
                return rss.isBoolean() && rss.boolean();
            }
        }
        return false;
    }

    // Returns true if the stage spec has scoreDetails: true inside the mongotQuery.
    bool hasScoreDetails() const {
        if (this->_originalBson.type() == BSONType::object) {
            auto specObj = this->_originalBson.Obj();
            // SearchLiteParsed's BSON shape mirrors DocumentSourceSearch::hasScoreDetails():
            // top-level $search spec is the mongotQuery, so 'scoreDetails' lives directly on it.
            if (specObj.hasField(kScoreDetailsFieldName)) {
                auto sd = specObj[kScoreDetailsFieldName];
                return sd.isBoolean() && sd.boolean();
            }
        }
        return false;
    }

private:
    const NamespaceString _nss;
};

#define DEFINE_LITE_PARSED_SEARCH_STAGE_DERIVED(stageName)                                        \
    DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(stageName);                                              \
    class stageName##LiteParsed final : public LiteParsedSearchStage<stageName##LiteParsed> {     \
    public:                                                                                       \
        stageName##LiteParsed(const mongo::BSONElement& originalBson, mongo::NamespaceString nss) \
            : LiteParsedSearchStage(originalBson, nss) {}                                         \
        static std::unique_ptr<stageName##LiteParsed> parse(                                      \
            const mongo::NamespaceString& nss,                                                    \
            const mongo::BSONElement& spec,                                                       \
            const mongo::LiteParserOptions& options) {                                            \
            return std::make_unique<stageName##LiteParsed>(spec, nss);                            \
        }                                                                                         \
        std::unique_ptr<mongo::StageParams> getStageParams() const final {                        \
            return std::make_unique<stageName##StageParams>(_originalBson);                       \
        }                                                                                         \
    };

}  // namespace mongo
