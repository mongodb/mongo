/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/coll_mod.h"

#include <boost/optional.hpp>

#include "mongo/db/coll_mod_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(CollModOptionTest, isConvertingIndexToUnique) {
    IDLParserContext ctx("collMod");
    auto requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}}");
    auto request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true, hidden: true}}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson(
        "{index: {keyPattern: {a: 1}, unique: true, hidden: true}, validationAction: 'warn'}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: true}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: false}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_TRUE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{index: {keyPattern: {a: 1}, prepareUnique: true}}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));

    requestObj = fromjson("{validationAction: 'warn'}");
    request = CollModRequest::parse(ctx, requestObj);
    ASSERT_FALSE(isCollModIndexUniqueConversion(request));
}

TEST(CollModOptionTest, makeDryRunRequest) {
    IDLParserContext ctx("collMod");
    auto requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}}");
    auto request = CollModRequest::parse(ctx, requestObj);
    auto dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true, hidden: true}}");
    request = CollModRequest::parse(ctx, requestObj);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_FALSE(dryRunRequest.getIndex()->getHidden());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson(
        "{index: {keyPattern: {a: 1}, unique: true, hidden: true}, validationAction: 'warn'}");
    request = CollModRequest::parse(ctx, requestObj);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_FALSE(dryRunRequest.getIndex()->getHidden());
    ASSERT_FALSE(dryRunRequest.getValidationAction());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());

    requestObj = fromjson("{index: {keyPattern: {a: 1}, unique: true}, dryRun: false}");
    request = CollModRequest::parse(ctx, requestObj);
    dryRunRequest = makeCollModDryRunRequest(request);
    ASSERT_TRUE(dryRunRequest.getIndex()->getKeyPattern()->binaryEqual(fromjson("{a: 1}")));
    ASSERT_TRUE(dryRunRequest.getIndex()->getUnique() && *dryRunRequest.getIndex()->getUnique());
    ASSERT_TRUE(dryRunRequest.getDryRun() && *dryRunRequest.getDryRun());
}
}  // namespace
}  // namespace mongo
