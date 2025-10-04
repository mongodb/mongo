/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"

#include <list>
#include <memory>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This aggregation stage is the '$listClusterCatalog' stage.
 * Lists any collection in the catalog and their related sharding informations.
 */
namespace DocumentSourceListClusterCatalog {

static constexpr StringData kStageName = "$listClusterCatalog"_sd;

class LiteParsed final : public LiteParsedDocumentSource {
public:
    static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                             const BSONElement& spec,
                                             const LiteParserOptions& options) {
        return std::make_unique<LiteParsed>(spec.fieldName(), nss);
    }

    explicit LiteParsed(std::string parseTimeName, const NamespaceString& nss)
        : LiteParsedDocumentSource(std::move(parseTimeName)) {

        if (nss.dbName() != DatabaseName::kAdmin) {
            if (nss.isCollectionlessAggregateNS()) {
                _privileges.emplace_back(Privilege(ResourcePattern::forDatabaseName(nss.dbName()),
                                                   ActionType::listCollections));
            } else {
                // This stage is designed to operate only on database-level namespaces (without
                // collections). By authorizing any users to run $listClusterCatalog when a
                // collection is specified, we allow the stage to fail correctly and warn the user
                // that only a database name should be provided instead of failing with
                // authorization issues.
                _privileges.emplace_back(Privilege());
            }
        } else {
            _privileges.emplace_back(Privilege(ResourcePattern::forClusterResource(nss.tenantId()),
                                               ActionType::listClusterCatalog));
        }
    };

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return {NamespaceString::kConfigsvrCollectionsNamespace,
                NamespaceString::kConfigsvrChunksNamespace};
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return _privileges;
    }

    bool isInitialSource() const final {
        return true;
    }

    bool generatesOwnDataOnce() const final {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        // The listCollections command that runs under the hood only accepts 'local' read concern.
        return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
    }

    void assertSupportsMultiDocumentTransaction() const override {
        transactionNotSupported(kStageName);
    }

private:
    PrivilegeVector _privileges;
};

static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

};  // namespace DocumentSourceListClusterCatalog

}  // namespace mongo
