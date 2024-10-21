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

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

//
// $text parsing tests.
//

class ExtensionsCallbackRealTest : public unittest::Test {
public:
    ExtensionsCallbackRealTest()
        : _nss(NamespaceString::createNamespaceString_forTest(
              "unittests.extensions_callback_real_test")) {
        _isDesugarWhereToFunctionOn = internalQueryDesugarWhereToFunction.load();
    }

    void setUp() final {
        AutoGetDb autoDb(&_opCtx, _nss.dbName(), MODE_X);
        auto database = autoDb.ensureDbExists(&_opCtx);
        {
            WriteUnitOfWork wunit(&_opCtx);
            ASSERT(database->createCollection(&_opCtx, _nss));
            wunit.commit();
        }
    }

    void tearDown() final {
        AutoGetDb autoDb(&_opCtx, _nss.dbName(), MODE_X);
        Database* database = autoDb.getDb();
        if (!database) {
            return;
        }
        {
            WriteUnitOfWork wunit(&_opCtx);
            static_cast<void>(database->dropCollection(&_opCtx, _nss));
            wunit.commit();
        }
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    const NamespaceString _nss;
    bool _isDesugarWhereToFunctionOn{false};
};

TEST_F(ExtensionsCallbackRealTest, TextNoIndex) {
    BSONObj query = fromjson("{$text: {$search:\"awesome\"}}");
    ASSERT_THROWS_CODE(StatusWithMatchExpression(
                           ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement())),
                       AssertionException,
                       ErrorCodes::IndexNotFound);
}

TEST_F(ExtensionsCallbackRealTest, TextBasic) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"english\"}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getQuery(), "awesome");
    ASSERT_EQUALS(textExpr->getFTSQuery().getLanguage(), "english");
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(),
                  TextMatchExpressionBase::kCaseSensitiveDefault);
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(),
                  TextMatchExpressionBase::kDiacriticSensitiveDefault);
}

TEST_F(ExtensionsCallbackRealTest, TextLanguageError) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"spanglish\"}}");
    ASSERT_THROWS_CODE(StatusWithMatchExpression(
                           ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement())),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ExtensionsCallbackRealTest, TextCaseSensitiveTrue) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(), true);
}

TEST_F(ExtensionsCallbackRealTest, TextCaseSensitiveFalse) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: false}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(), false);
}

TEST_F(ExtensionsCallbackRealTest, TextCaseSensitiveError) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text:{$search:\"awesome\", $caseSensitive: 0}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveTrue) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(), true);
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveFalse) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: false}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(), false);
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveError) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query = fromjson("{$text:{$search:\"awesome\", $diacriticSensitive: 0}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST_F(ExtensionsCallbackRealTest, TextDiacriticSensitiveAndCaseSensitiveTrue) {
    ASSERT_OK(dbtests::createIndex(&_opCtx,
                                   _nss.ns_forTest(),
                                   BSON("a"
                                        << "text"),
                                   false));  // isUnique

    BSONObj query =
        fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: true, $caseSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&_opCtx, &_nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getFTSQuery().getDiacriticSensitive(), true);
    ASSERT_EQUALS(textExpr->getFTSQuery().getCaseSensitive(), true);
}

//
// $where parsing tests.
//
const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("db.dummy");

TEST_F(ExtensionsCallbackRealTest, WhereExpressionDesugarsToExprAndInternalJs) {
    if (_isDesugarWhereToFunctionOn) {
        auto query1 = fromjson("{$where: 'function() { return this.x == 10; }'}");
        auto expCtx = ExpressionContextBuilder{}.opCtx(&_opCtx).ns(kTestNss).build();

        auto expr1 = unittest::assertGet(
            ExtensionsCallbackReal(&_opCtx, &_nss).parseWhere(expCtx, query1.firstElement()));

        auto expectedMatch = fromjson(
            "{$expr: {$function: {'body': 'function() { return this.x == 10; }', 'args': "
            "['$$CURRENT'], 'lang': 'js', '_internalSetObjToThis': true}}}");
        ASSERT_BSONOBJ_EQ(expr1->serialize(), expectedMatch);
    }
}

}  // namespace
}  // namespace mongo
