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

#include "mongo/db/extension/host/signature_validator.h"

#include "mongo/db/extension/host/load_extension_test_util.h"
#include "mongo/db/extension/host/mongot_extension_signing_key.h"
#include "mongo/db/extension/host/rnp/rnp.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <filesystem>
#include <fstream>

namespace mongo::extension::host {
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

class SignatureValidatorTest : public TestWithTempDirectory {
public:
    SignatureValidatorTest() {}

    void setUp() override {
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
        _previousExtensionsSignaturePublicKeyPath =
            serverGlobalParams.extensionsSignaturePublicKeyPath;
        serverGlobalParams.extensionsSignaturePublicKeyPath =
            mongo::extension::host::test_util::getPublicKeyPath();
#endif
    }

    void tearDown() override {
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
        if (!_previousExtensionsSignaturePublicKeyPath.empty()) {
            serverGlobalParams.extensionsSignaturePublicKeyPath =
                _previousExtensionsSignaturePublicKeyPath;
        }
#endif
    }

    void disableFeatureFlag() {
        _featureFlagExtensionsApiSignatureValidation = RAIIServerParameterControllerForTest{
            "featureFlagExtensionsApiSignatureValidation", false};
    }

private:
    std::string _previousExtensionsSignaturePublicKeyPath{""};
    RAIIServerParameterControllerForTest _featureFlagExtensionsApiSignatureValidation{
        "featureFlagExtensionsApiSignatureValidation", true};
};

class SignatureValidatorForTest : public SignatureValidator {
public:
    SignatureValidatorForTest(bool secureMode) : SignatureValidator(secureMode) {}
    ~SignatureValidatorForTest() override = default;

    bool secureModeEnabled() const {
        return _secureMode;
    }

    bool skipValidation() const {
        return _skipValidation;
    }

    using SignatureValidator::_getValidationPublicKeyAsRnpInput;
};

// Secure mode tests.
/**
 * SecureModeBaseline: tests that the SignatureValidator initializes correctly in secure mode.
 * We expect that secureModeEnabled() should be true, and skipValidation() should be false. Note,
 * that we are unable to fully test signature verification in secure mode, because an extension
 * that's been signed with the mongot-extension signing key is not available to us here. Instead, we
 * verify that the signature verification is not skipped, and that the RnpInput that is generated
 * during initalization matches the mongot-extension signing key.
 */
TEST_F(SignatureValidatorTest, SecureModeBaseline) {
    SignatureValidatorForTest signatureValidator(true);
    ASSERT_TRUE(signatureValidator.secureModeEnabled());
    ASSERT_FALSE(signatureValidator.skipValidation());

    const auto rnpInput = signatureValidator._getValidationPublicKeyAsRnpInput();
    rnp::RnpOutput output;
    output.pipeFromInput(rnpInput);
    ASSERT_TRUE(output.getAsStringView() == kMongoExtensionSigningPublicKey);
}

/**
 * SecureModeFeatureFlagDisabled: tests that the SignatureValidator initialized in secure mode obeys
 * featureFlagExtensionsApiSignatureValidation.
 * We expect that skipValidation() should be true, and that validateExtensionSignature() runs
 * succesfully even if the provided arguments are invalid.
 */
TEST_F(SignatureValidatorTest, SecureModeFeatureFlagDisabled) {
    disableFeatureFlag();
    SignatureValidatorForTest signatureValidator(true);
    ASSERT_TRUE(signatureValidator.secureModeEnabled());
    ASSERT_TRUE(signatureValidator.skipValidation());
    signatureValidator.validateExtensionSignature("foo", "bar");
}

/**
 * SecureModeValidatingTestExtensionAgainstMongotExtensionKeyFails: tests that verifying a test
 * extension, which is not signed with the mongot-extension signing key fails.
 */
TEST_F(SignatureValidatorTest, SecureModeValidatingTestExtensionAgainstMongotExtensionKeyFails) {
    SignatureValidatorForTest signatureValidator(true);
    ASSERT_THROWS_CODE(
        signatureValidator.validateExtensionSignature(
            kTestFooLibExtensionName, test_util::getExtensionPath(kTestFooLibExtensionName)),
        AssertionException,
        11528920);
}

// Insecure mode tests.
// Note, any insecure mode tests are disabled when built in insecure mode, since SignatureValidator
// internally uses this macro to guard access to the extensionsSignaturePublicKeyPath.
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
/**
 * InsecureModeEmptyPublicKeySkipsValidation: tests that initializing a SignatureValidator in
 * insecure mode, without providing an extensionsSignaturePublicKeyPath in the serverGlobalParams
 * results in validation being skipped.
 */
TEST_F(SignatureValidatorTest, InsecureModeEmptyPublicKeySkipsValidation) {
    serverGlobalParams.extensionsSignaturePublicKeyPath = "";
    SignatureValidatorForTest signatureValidator(false);
    ASSERT_FALSE(signatureValidator.secureModeEnabled());
    ASSERT_TRUE(signatureValidator.skipValidation());

    signatureValidator.validateExtensionSignature("foo", "bar");
}

/**
 * InsecureModeEmptyPublicKeyWithFeatureFlagDisabledSkipsValidation: tests that initializing a
 * SignatureValidator in insecure mode obeys featureFlagExtensionsApiSignatureValidation. We expect
 * that validation is skipped regardless of whether or not extensionsSignaturePublicKeyPath was
 * provided in the serverGlobalParams.
 */
TEST_F(SignatureValidatorTest, InsecureModeWithFeatureFlagDisabledSkipsValidation) {
    disableFeatureFlag();

    auto testValidationSkipped = [&]() {
        SignatureValidatorForTest signatureValidator(false);
        ASSERT_FALSE(signatureValidator.secureModeEnabled());
        ASSERT_TRUE(signatureValidator.skipValidation());

        signatureValidator.validateExtensionSignature("foo", "bar");
    };
    // Test with signature public key path provided in serverGlobalParams (note, defaults to
    // non-empty).
    testValidationSkipped();
    // Test with empty signature public key path provided in serverGlobalParams.
    serverGlobalParams.extensionsSignaturePublicKeyPath = "";
    testValidationSkipped();
}

/**
 * InsecureModeNonEmptyPublicKeyValidateTestExtensionSucceeds: tests that initializing a
 * SignatureValidator in insecure mode with a non-empty public key results in validation not being
 * skipped. This tests validates the signature for extension Foo, which is expected to succeed.
 * Note, extensionsSignaturePublicKeyPath is found in the serverGlobalParams, and defaults to
 * non-empty in this unit test.
 */
TEST_F(SignatureValidatorTest, InsecureModeNonEmptyPublicKeyValidateTestExtensionSucceeds) {
    SignatureValidatorForTest signatureValidator(false);
    ASSERT_FALSE(signatureValidator.secureModeEnabled());
    ASSERT_FALSE(signatureValidator.skipValidation());
    signatureValidator.validateExtensionSignature(
        kTestFooLibExtensionName, test_util::getExtensionPath(kTestFooLibExtensionName));
}

/**
 * InsecureModeNonEmptyPublicKeyValidateMultipleTestExtensionsSucceeds: tests that initializing a
 * SignatureValidator in insecure mode with a non-empty public key is able to validate the signature
 * for both extension libfoo_mongo_extension.so and libmongothost_extension.so which are signed with
 * the same key. This simulates loading multiple extensions in a loop and validating their signature
 * with the same signature validator, as is done at server start-up time.
 */
TEST_F(SignatureValidatorTest,
       InsecureModeNonEmptyPublicKeyValidateMultipleTestExtensionsSucceeds) {
    SignatureValidatorForTest signatureValidator(false);
    ASSERT_FALSE(signatureValidator.secureModeEnabled());
    ASSERT_FALSE(signatureValidator.skipValidation());
    signatureValidator.validateExtensionSignature(
        kTestFooLibExtensionName, test_util::getExtensionPath(kTestFooLibExtensionName));
    signatureValidator.validateExtensionSignature(
        kTestFooLibExtensionName, test_util::getExtensionPath(kTestMongotHostLibExtensionName));
}
#endif

/**
 * ValidatingExtensionWithInvalidNameFails: tests that validating a signature with an extension name
 * specified without the '.so' extension fails.
 */
TEST_F(SignatureValidatorTest, ValidatingExtensionWithInvalidNameFails) {
    const std::string extensionName = "foo";
    const std::filesystem::path extensionPath = getTempDirPath() / extensionName;
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
    {
        SignatureValidatorForTest signatureValidator(false);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath),
            AssertionException,
            11528810);
    }
#endif
    {
        SignatureValidatorForTest signatureValidator(true);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath),
            AssertionException,
            11528810);
    }
}

/**
 * ValidatingNonExistentExtensionPathFails: tests that validating a signature providing a
 * non-existent file path fails. The correct file name for extension foo is
 * libfoo_mongo_extension.so.
 */
TEST_F(SignatureValidatorTest, ValidatingNonExistentExtensionPathFails) {
    const std::string extensionName = "foo.so";
    const std::filesystem::path extensionPath = getTempDirPath() / extensionName;
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
    {
        SignatureValidatorForTest signatureValidator(false);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath),
            AssertionException,
            11528923);
    }
#endif
    {
        SignatureValidatorForTest signatureValidator(true);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath),
            AssertionException,
            11528923);
    }
}


#ifndef MONGO_CONFIG_EXT_SIG_SECURE
/**
 * InsecureModeValidatingFooExtensionWithMongotHostSignatureFails: tests that validating the foo
 * extension with the mongot signing key fails. This is expected, since the test foo extension is
 * signed with a different key than the mongot-extension signing key.
 */
TEST_F(SignatureValidatorTest, InsecureModeValidatingFooExtensionWithMongotHostSignatureFails) {
    const std::string extensionName = "foo_extension_copy.so";
    const std::filesystem::path extensionPath = getTempDirPath() / extensionName;

    // 1) Copy foo extension to our test extension path.
    std::filesystem::copy_file(test_util::getExtensionPath(kTestFooLibExtensionName),
                               extensionPath,
                               std::filesystem::copy_options::overwrite_existing);

    // 2) Copy foo extension's signature file to the extension path.
    const std::filesystem::path kExtensionSignaturePath = extensionPath + ".sig";

    std::filesystem::copy_file(test_util::getExtensionPath(kTestFooLibExtensionName) + ".sig",
                               kExtensionSignaturePath,
                               std::filesystem::copy_options::overwrite_existing);

    SignatureValidatorForTest signatureValidator(false);
    // 3) Sanity check, validating test extension with its signature succeeds.
    signatureValidator.validateExtensionSignature(extensionName, extensionPath);

    // 4) Swap out the signature file with the contents of a different extension's signature
    // file.
    std::filesystem::remove(kExtensionSignaturePath);
    std::filesystem::copy_file(test_util::getExtensionPath(kTestMongotHostLibExtensionName) +
                                   ".sig",
                               kExtensionSignaturePath,
                               std::filesystem::copy_options::overwrite_existing);
    // 5) Validating foo extension with the wrong signature file fails.
    ASSERT_THROWS_CODE(signatureValidator.validateExtensionSignature(extensionName, extensionPath),
                       AssertionException,
                       11528920);
}

/**
 * InsecureModeValidatingFooExtensionWithInvalidKeyFails: tests that validating the foo
 * extension with an invalid PGP key fails.
 */
TEST_F(SignatureValidatorTest, InsecureModeValidatingFooExtensionWithInvalidKeyFails) {
    const std::string kExtensionName = "foo_extension_copy.so";
    const std::filesystem::path extensionPath = getTempDirPath() / kExtensionName;

    // 1) Copy foo extension to our test extension path.
    std::filesystem::copy_file(test_util::getExtensionPath(kTestFooLibExtensionName),
                               extensionPath,
                               std::filesystem::copy_options::overwrite_existing);

    // 2) Copy foo extension's signature file to the extension path.
    const std::filesystem::path extensionSignaturePath = extensionPath + ".sig";

    std::filesystem::copy_file(test_util::getExtensionPath(kTestFooLibExtensionName) + ".sig",
                               extensionSignaturePath,
                               std::filesystem::copy_options::overwrite_existing);

    const std::filesystem::path kTestPublicKeyPath = getTempDirPath() / "invalid_public_key.asc";
    {
        std::ofstream f(kTestPublicKeyPath);
        f << "INVALID PGP KEY!!!";
    }

    serverGlobalParams.extensionsSignaturePublicKeyPath = kTestPublicKeyPath;
    ASSERT_THROWS_CODE(
        [&]() {
            SignatureValidatorForTest signatureValidator(false);
            signatureValidator.validateExtensionSignature(kExtensionName, extensionSignaturePath);
        }(),
        AssertionException,
        11528906);
}
#endif
}  // namespace
}  // namespace mongo::extension::host
