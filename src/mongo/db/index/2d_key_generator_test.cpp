/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/expression_keys_private.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

using namespace mongo;

namespace {

std::string dumpKeyset(const BSONObjSet& objs) {
    std::stringstream ss;
    ss << "[ ";
    for (BSONObjSet::iterator i = objs.begin(); i != objs.end(); ++i) {
        ss << i->toString() << " ";
    }
    ss << "]";

    return ss.str();
}

bool assertKeysetsEqual(const BSONObjSet& expectedKeys, const BSONObjSet& actualKeys) {
    if (expectedKeys != actualKeys) {
        log() << "Expected: " << dumpKeyset(expectedKeys) << ", "
              << "Actual: " << dumpKeyset(actualKeys);
        return false;
    }
    return true;
}

BSONObj make2DKey(const TwoDIndexingParams& params, int x, int y, BSONElement trailingFields) {
    BSONObjBuilder bob;
    BSONObj locObj = BSON_ARRAY(x << y);
    params.geoHashConverter->hash(locObj, nullptr).appendHashMin(&bob, "");
    bob.append(trailingFields);
    return bob.obj();
}

TEST(2dKeyGeneratorTest, TrailingField) {
    BSONObj obj = fromjson("{a: [0, 0], b: 5}");
    BSONObj infoObj = fromjson("{key: {a: '2d', b: 1}}");
    TwoDIndexingParams params;
    ExpressionParams::parseTwoDParams(infoObj, &params);
    BSONObjSet actualKeys;
    std::vector<BSONObj> locs;
    ExpressionKeysPrivate::get2DKeys(obj, params, &actualKeys, &locs);

    BSONObjSet expectedKeys;
    BSONObj trailingFields = BSON("" << 5);
    expectedKeys.insert(make2DKey(params, 0, 0, trailingFields.firstElement()));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(2dKeyGeneratorTest, ArrayTrailingField) {
    BSONObj obj = fromjson("{a: [0, 0], b: [5, 6]}");
    BSONObj infoObj = fromjson("{key: {a: '2d', b: 1}}");
    TwoDIndexingParams params;
    ExpressionParams::parseTwoDParams(infoObj, &params);
    BSONObjSet actualKeys;
    std::vector<BSONObj> locs;
    ExpressionKeysPrivate::get2DKeys(obj, params, &actualKeys, &locs);

    BSONObjSet expectedKeys;
    BSONObj trailingFields = BSON("" << BSON_ARRAY(5 << 6));
    expectedKeys.insert(make2DKey(params, 0, 0, trailingFields.firstElement()));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(2dKeyGeneratorTest, ArrayOfObjectsTrailingField) {
    BSONObj obj = fromjson("{a: [0, 0], b: [{c: 5}, {c: 6}]}");
    BSONObj infoObj = fromjson("{key: {a: '2d', 'b.c': 1}}");
    TwoDIndexingParams params;
    ExpressionParams::parseTwoDParams(infoObj, &params);
    BSONObjSet actualKeys;
    std::vector<BSONObj> locs;
    ExpressionKeysPrivate::get2DKeys(obj, params, &actualKeys, &locs);

    BSONObjSet expectedKeys;
    BSONObj trailingFields = BSON("" << BSON_ARRAY(5 << 6));
    expectedKeys.insert(make2DKey(params, 0, 0, trailingFields.firstElement()));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

}  // namespace
