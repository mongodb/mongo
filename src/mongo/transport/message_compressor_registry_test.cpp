/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/stdx/memory.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

/**
 * Asserts that a value is null
 *
 * TODO: Move this into the unittest's standard ASSERT_ macros.
 */
#define ASSERT_NULL(a) ASSERT_EQ(a, static_cast<decltype(a)>(nullptr))

namespace mongo {
namespace {
TEST(MessageCompressorRegistry, RegularTest) {
    MessageCompressorRegistry registry;
    auto compressor = stdx::make_unique<NoopMessageCompressor>();
    auto compressorPtr = compressor.get();

    std::vector<std::string> compressorList = {compressorPtr->getName()};
    auto compressorListCheck = compressorList;
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    registry.finalizeSupportedCompressors().transitional_ignore();

    ASSERT_TRUE(compressorListCheck == registry.getCompressorNames());

    ASSERT_EQ(registry.getCompressor(compressorPtr->getName()), compressorPtr);
    ASSERT_EQ(registry.getCompressor(compressorPtr->getId()), compressorPtr);

    ASSERT_NULL(registry.getCompressor("fakecompressor"));
    ASSERT_NULL(registry.getCompressor(255));
}

TEST(MessageCompressorRegistry, NothingRegistered) {
    MessageCompressorRegistry registry;

    ASSERT_NULL(registry.getCompressor("noop"));
    ASSERT_NULL(registry.getCompressor(0));
}

TEST(MessageCompressorRegistry, SetSupported) {
    MessageCompressorRegistry registry;
    auto compressor = stdx::make_unique<NoopMessageCompressor>();
    auto compressorId = compressor->getId();
    auto compressorName = compressor->getName();

    std::vector<std::string> compressorList = {"foobar"};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    auto ret = registry.finalizeSupportedCompressors();
    ASSERT_NOT_OK(ret);

    ASSERT_NULL(registry.getCompressor(compressorId));
    ASSERT_NULL(registry.getCompressor(compressorName));
}
}  // namespace
}  // namespace mongo
