/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_text.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <string>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test", "coll");
const BSONObj kTextIndexSpec = BSON("v" << 2 << "key"
                                        << BSON("body"
                                                << "text")
                                        << "name"
                                        << "body_text");

class ExpressionTextTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        auto opCtx = operationContext();
        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
        auto acq = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        WriteUnitOfWork wuow(opCtx);
        CollectionWriter writer{opCtx, &acq};
        auto* writableColl = writer.getWritableCollection(opCtx);
        ASSERT_OK(writableColl->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, writableColl, kTextIndexSpec));
        wuow.commit();
    }

    TextMatchExpression makeExpr(OperationContext* opCtx) {
        return TextMatchExpression(opCtx,
                                   kNss,
                                   TextMatchExpressionBase::TextParams{
                                       .query = "hello",
                                       .caseSensitive = false,
                                       .diacriticSensitive = false,
                                   });
    }
};

// Verifies that the TextMatchExpression constructor safely owns its copy of the index version and
// default language even after the underlying IndexCatalogEntry is freed by a concurrent drop.
TEST_F(ExpressionTextTest, OwnedValuesOutliveIndexCatalogEntryDrop) {
    std::string observedLanguage;

    {
        auto* fp = globalFailPointRegistry().find("hangBeforeUsingFTSIndexInfo");
        ASSERT(fp);
        auto initialTimesEntered = fp->setMode(FailPoint::alwaysOn);

        stdx::thread thread([this, &observedLanguage] {
            // No outer collection acquisition — validateFTSIndex's inner acquisition is the only
            // reference to the IndexCatalogEntry. After it closes, a concurrent drop can free the
            // entry before the constructor uses the returned values.
            ThreadClient client(getServiceContext()->getService());
            auto opCtx = client->makeOperationContext();
            auto expr = makeExpr(opCtx.get());
            observedLanguage = expr.getFTSQuery().getLanguage();
        });
        // Guards fire LIFO: fp must be disabled before joining the thread.
        ON_BLOCK_EXIT([&] { thread.join(); });
        ON_BLOCK_EXIT([&] { fp->setMode(FailPoint::off); });

        fp->waitForTimesEntered(initialTimesEntered + 1);

        // Drop the index while the thread is paused at the failpoint. The thread holds no
        // snapshot, so the catalog entry can be freed immediately.
        auto opCtx = operationContext();
        auto acq = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        WriteUnitOfWork wuow(opCtx);
        CollectionWriter writer{opCtx, &acq};
        auto* writableColl = writer.getWritableCollection(opCtx);
        auto* indexCatalog = writableColl->getIndexCatalog();
        auto* entry = indexCatalog->getWritableEntryByName(
            opCtx, "body_text", IndexCatalog::InclusionPolicy::kReady);
        ASSERT(entry);
        ASSERT_OK(indexCatalog->dropIndexEntry(opCtx, writableColl, entry));
        wuow.commit();
    }  // ON_BLOCK_EXIT: fp->setMode(off) then thread.join()

    ASSERT_EQ(observedLanguage, "english");
}

}  // namespace
}  // namespace mongo
