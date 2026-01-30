/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host/catalog_context.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo::extension {
namespace {

TEST(CatalogContextTest, CatalogContextWithValidFields) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();
    const auto dbNameSd = "test"_sd;
    const auto collNameSd = "namespace"_sd;
    const auto expectedUUID = UUID::gen();

    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest(dbNameSd, collNameSd),
        SerializationContext());

    expCtx->setUUID(expectedUUID);
    expCtx->setInRouter(true);
    expCtx->setExplain(ExplainOptions::Verbosity::kExecAllPlans);

    const auto catalogContext = mongo::extension::host::CatalogContext(*expCtx);
    const auto& extensionCatalogContext = catalogContext.getAsBoundaryType();
    const auto& extensionNamespaceString = extensionCatalogContext.namespaceString;

    ASSERT_EQUALS(byteViewAsStringView(extensionNamespaceString.databaseName),
                  std::string_view(dbNameSd));
    ASSERT_EQUALS(byteViewAsStringView(extensionNamespaceString.collectionName),
                  std::string_view(collNameSd));

    std::string uuidAsString(byteViewAsStringView(extensionCatalogContext.uuidString));
    auto statusWithUUID = UUID::parse(uuidAsString);
    ASSERT_TRUE(statusWithUUID.isOK());
    ASSERT_EQUALS(expectedUUID, statusWithUUID.getValue());

    ASSERT_EQUALS(extensionCatalogContext.inRouter, 1);
    ASSERT_EQUALS(extensionCatalogContext.verbosity,
                  ::MongoExtensionExplainVerbosity::kExecAllPlans);
}

TEST(CatalogContextTest, CatalogContextWithEmptyFields) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();
    const auto dbNameSd = ""_sd;
    const auto collNameSd = ""_sd;

    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest(dbNameSd, collNameSd),
        SerializationContext());

    const auto catalogContext = mongo::extension::host::CatalogContext(*expCtx);
    const auto& extensionCatalogContext = catalogContext.getAsBoundaryType();
    const auto& extensionNamespaceString = extensionCatalogContext.namespaceString;
    ASSERT_EQUALS(extensionNamespaceString.databaseName.len, 0);
    ASSERT_EQUALS(extensionNamespaceString.collectionName.len, 0);
    ASSERT_EQUALS(extensionCatalogContext.uuidString.len, 0);
    ASSERT_EQUALS(extensionCatalogContext.inRouter, 0);
    ASSERT_EQUALS(extensionCatalogContext.verbosity, ::MongoExtensionExplainVerbosity::kNotExplain);
}
}  // namespace
}  // namespace mongo::extension
