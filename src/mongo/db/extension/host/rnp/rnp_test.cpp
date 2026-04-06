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
#include "mongo/db/extension/host/rnp/rnp.h"

#include "mongo/db/extension/host/load_extension_test_util.h"
#include "mongo/db/extension/host/mongot_extension_signing_key.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <filesystem>
#include <fstream>

namespace mongo::extension::host::rnp {
namespace {
static inline const std::string kTestFooLibExtensionName = "libfoo_mongo_extension.so";
static inline const std::string kTestMongotHostLibExtensionName = "libmongothost_extension.so";

class TestWithTempDirectory : public unittest::Test {
public:
    std::filesystem::path getTempDirPath() const {
        return std::filesystem::path(_tempDir.path());
    }

protected:
    const unittest::TempDir _tempDir{"TestWithTempDirectory"};
};

class RnpInputTest : public TestWithTempDirectory {};
class RnpContextTest : public TestWithTempDirectory {};

TEST_F(RnpInputTest, CreateFromPath_NonExistentPathFails) {
    ASSERT_THROWS_CODE(RnpInput::createFromPath((getTempDirPath() / "foo").string()),
                       AssertionException,
                       11528900);
}

TEST_F(RnpInputTest, CreateFromPath_ExistentPathSucceeds) {
    const std::string filePath = getTempDirPath() / "dummy.txt";
    {
        std::ofstream f(filePath);
        f << "foo";
    }
    ASSERT_DOES_NOT_THROW(RnpInput::createFromPath(filePath));
}

TEST_F(RnpInputTest, CreateFromMemorySucceeds) {
    const std::string inputContents = "Dummy contents";
    ASSERT_DOES_NOT_THROW(RnpInput::createFromMemory(inputContents));
    ASSERT_DOES_NOT_THROW(RnpInput::createFromMemory(inputContents, true));
}

TEST_F(RnpContextTest, ImportInvalidKeyFails) {
    const std::string filePath = getTempDirPath() / "dummy.txt";
    {
        std::ofstream f(filePath);
        f << "foo";
    }
    auto input = RnpInput::createFromPath(filePath);


    RnpContext ctx;
    ctx.initialize();
    ASSERT_THROWS_CODE(ctx.importKey(input), AssertionException, 11528906);
}

TEST_F(RnpContextTest, ImportKeyFromMemoryNoCopySucceeds) {
    const std::string kPublicKey(kMongoExtensionSigningPublicKey);
    auto input = RnpInput::createFromMemory(kPublicKey, false);

    RnpContext ctx;
    ctx.initialize();
    ctx.importKey(input);
}

TEST_F(RnpContextTest, ImportKeyFromMemoryWithCopySucceeds) {
    auto input = []() {
        return RnpInput::createFromMemory(std::string(kMongoExtensionSigningPublicKey), true);
    }();
    RnpContext ctx;
    ctx.initialize();
    ctx.importKey(input);
}

// RnpContext only supports importing one key. Calling importKey multiple is expected to fail.
TEST_F(RnpContextTest, ImportMultipleKeysFails) {
    auto input = []() {
        return RnpInput::createFromMemory(std::string(kMongoExtensionSigningPublicKey), true);
    }();
    RnpContext ctx;
    ctx.initialize();
    ctx.importKey(input);
    ASSERT_THROWS_CODE(ctx.importKey(input), AssertionException, 11528928);
}

DEATH_TEST(RnpContextDeathTest, VerifySignatureBeforeInitializationFails, "11528918") {
    RnpContext ctx;
    ctx.verifyDetachedSignature("foo", "bar");
}

TEST_F(RnpContextTest, VerifyNonExistentFilesFail) {
    RnpContext ctx;
    ctx.initialize();
    const std::string extensionFilePath = getTempDirPath() / "dummy.so";
    const std::string extensionSignatureFilePath = extensionFilePath + ".sig";

    ASSERT_THROWS_CODE(ctx.verifyDetachedSignature(extensionFilePath, extensionSignatureFilePath),
                       AssertionException,
                       11528900);
    {
        std::ofstream f(extensionFilePath);
        f << "foo";
    }
    ASSERT_THROWS_CODE(ctx.verifyDetachedSignature(extensionFilePath, extensionSignatureFilePath),
                       AssertionException,
                       11528900);
    {
        std::ofstream f(extensionSignatureFilePath);
        f << "foo";
    }
    ASSERT_THROWS_CODE(ctx.verifyDetachedSignature(extensionFilePath, extensionSignatureFilePath),
                       AssertionException,
                       11528917);
}

TEST_F(RnpContextTest, VerifyTestExtensionSucceeds) {
    RnpContext ctx;
    ctx.initialize();

    ctx.importKey(RnpInput::createFromPath(mongo::extension::host::test_util::getPublicKeyPath()));
    const std::string extensionPath = test_util::getExtensionPath(kTestFooLibExtensionName);
    const std::string extensionSignaturePath = extensionPath + ".sig";
    ctx.verifyDetachedSignature(extensionPath, extensionSignaturePath);
}

TEST_F(RnpContextTest, VerifyTestExtensionWithNoKeyImportedFails) {
    RnpContext ctx;
    ctx.initialize();
    const std::string extensionPath = test_util::getExtensionPath(kTestFooLibExtensionName);
    const std::string extensionSignaturePath = extensionPath + ".sig";
    ASSERT_THROWS_CODE(ctx.verifyDetachedSignature(extensionPath, extensionSignaturePath),
                       AssertionException,
                       11528917);
}

/**
 * VerifyFooExtensionWithMongotHostSignatureFails: This test tries to verify the signature of the
 * MongotHostLibExtension against the Foo extension. This is expected to fail, since the detached
 * signature does not correspond to the Foo extension's binary.
 */
TEST_F(RnpContextTest, VerifyFooExtensionWithMongotHostSignatureFails) {
    RnpContext ctx;
    ctx.initialize();
    ctx.importKey(RnpInput::createFromPath(mongo::extension::host::test_util::getPublicKeyPath()));

    const std::string fooExtensionPath = test_util::getExtensionPath(kTestFooLibExtensionName);
    const std::string mongotExtensionSignaturePath =
        test_util::getExtensionPath(kTestMongotHostLibExtensionName) + ".sig";
    ASSERT_THROWS_CODE(ctx.verifyDetachedSignature(fooExtensionPath, mongotExtensionSignaturePath),
                       AssertionException,
                       11528917);
}

/**
 * VerifyTestExtensionWithIncorrectKeyFails: This test tries to verify the Foo extension's signature
 * against the mongot-extension signing key. This is expected to fail, because the Foo extension is
 * not signed with the mongot-extension signing key. Instead, the Foo extension is signed by a
 * signing key that is used exclusively by our build system.
 */
TEST_F(RnpContextTest, VerifyTestExtensionWithIncorrectKeyFails) {
    RnpContext ctx;
    ctx.initialize();
    const std::string pubKeyContents(kMongoExtensionSigningPublicKey);
    ctx.importKey(RnpInput::createFromMemory(pubKeyContents));
    const std::string kExtensionPath = test_util::getExtensionPath(kTestFooLibExtensionName);
    const std::string kExtensionSignaturePath = kExtensionPath + ".sig";
    ASSERT_THROWS_CODE(ctx.verifyDetachedSignature(kExtensionPath, kExtensionSignaturePath),
                       AssertionException,
                       11528917);
}
}  // namespace
}  // namespace mongo::extension::host::rnp
