/**
 *    Copyright 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/platform/decimal128_bson_test.h"

#include <array>
#include <cmath>
#include <utility>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/json.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace {
using namespace mongo;

BSONObj convertHexStringToBsonObj(StringData hexString) {
    const char* p = hexString.rawData();
    size_t bufferSize = hexString.size() / 2;
    auto buffer = SharedBuffer::allocate(bufferSize);

    for (unsigned int i = 0; i < bufferSize; i++) {
        buffer.get()[i] = fromHex(p);
        p += 2;
    }

    return BSONObj(std::move(buffer));
}

// Reconcile format differences between test data and output data.
std::string trimWhiteSpace(std::string str) {
    std::string result;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] != ' ') {
            result += str[i];
        }
    }
    return result;
}

TEST(Decimal128BSONTest, TestsConstructingDecimalWithBsonDump) {
    BSONObj allData = fromjson(testData);
    BSONObj data = allData.getObjectField("valid");
    BSONObjIterator it(data);

    while (it.moreWithEOO()) {
        BSONElement testCase = it.next();
        if (testCase.eoo()) {
            break;
        }
        if (testCase.type() == Object) {
            BSONObj b = testCase.Obj();
            BSONElement desc = b.getField("description");
            BSONElement bson = b.getField("bson");
            BSONElement extjson = b.getField("extjson");
            BSONElement canonical_extjson = b.getField("canonical_extjson");

            log() << "Test - " << desc.str();

            StringData hexString = bson.valueStringData();
            BSONObj d = convertHexStringToBsonObj(hexString);
            std::string outputJson = d.jsonString();
            std::string expectedJson;

            if (!canonical_extjson.eoo()) {
                expectedJson = canonical_extjson.str();
            } else {
                expectedJson = extjson.str();
            }

            ASSERT_EQ(trimWhiteSpace(outputJson), trimWhiteSpace(expectedJson));
            log() << "PASSED";
        }
    }
}
}  // namespace
