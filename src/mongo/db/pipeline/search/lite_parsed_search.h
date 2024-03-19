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

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {
/**
 * A 'LiteParsed' representation of either a $search or $searchMeta stage.
 * This is the parent class for the $listSearchIndexes stage.
 */
class LiteParsedSearchStage : public LiteParsedDocumentSource {
public:
    static std::unique_ptr<LiteParsedSearchStage> parse(const NamespaceString& nss,
                                                        const BSONElement& spec) {
        return std::make_unique<LiteParsedSearchStage>(spec.fieldName(), nss);
    }

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

    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return onlyReadConcernLocalSupported(getParseTimeName(), level, isImplicitDefault);
    }

    void assertSupportsMultiDocumentTransaction() const override {
        transactionNotSupported(getParseTimeName());
    }

    explicit LiteParsedSearchStage(std::string parseTimeName, NamespaceString nss)
        : LiteParsedDocumentSource(std::move(parseTimeName)), _nss(std::move(nss)) {}

private:
    const NamespaceString _nss;
};
}  // namespace mongo
