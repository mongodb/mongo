// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_compressor_registry.h"

#include "mongo/transport/message_compressor_noop.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

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
    auto compressor = std::make_unique<NoopMessageCompressor>();
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
    auto compressor = std::make_unique<NoopMessageCompressor>();
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
