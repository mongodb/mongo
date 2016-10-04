/*    Copyright 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/secure_zero_memory.h"

namespace mongo {

TEST(SecureZeroMemoryTest, zeroZeroLengthNull) {
    void* ptr = nullptr;
    secureZeroMemory(ptr, 0);
    ASSERT_TRUE(true);
}

DEATH_TEST(SecureZeroMemoryTest, zeroNonzeroLengthNull, "Fatal Assertion") {
    void* ptr = nullptr;
    secureZeroMemory(ptr, 1000);
}

TEST(SecureZeroMemoryTest, dataZeroed) {
    static const size_t dataSize = 100;
    std::uint8_t data[dataSize];

    // Populate array
    for (size_t i = 0; i < dataSize; ++i) {
        data[i] = i;
    }

    // Zero array
    secureZeroMemory(data, dataSize);

    // Check contents
    for (size_t i = 0; i < dataSize; ++i) {
        ASSERT_FALSE(data[i]);
    }
}

}  // namespace mongo
