// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/signature_validator.h"

#include "mongo/db/extension/host/load_extension_test_util.h"
#include "mongo/db/extension/host/mongot_extension_signing_key.h"
#include "mongo/db/extension/host/rnp/rnp.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <filesystem>
#include <fstream>

namespace mongo::extension::host {
namespace {
static inline const std::string kTestFooLibExtensionName = "libfoo_mongo_extension.so";
static inline const std::string kTestReadNDocumentsLibExtensionName =
    "libread_n_documents_mongo_extension.so";

/**
 * Copies a signed test extension and its detached signature into 'destDir', returning the path to
 * the copied .so. The copy inherits the source's read-only mode (no group/other write) and is owned
 * by the test user, so it passes the validator's permission gate by default.
 */
std::filesystem::path copySignedExtension(const std::filesystem::path& destDir,
                                          const std::string& libName) {
    namespace fs = std::filesystem;
    fs::create_directories(destDir);
    const fs::path dest = destDir / libName;
    const auto src = test_util::getExtensionPath(libName);
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
    fs::copy_file(std::string{src} + ".sig",
                  std::string{dest} + ".sig",
                  fs::copy_options::overwrite_existing);
    return dest;
}

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
        _featureFlagExtensionsApiSignatureValidation =
            unittest::ServerParameterGuard{"featureFlagExtensionsApiSignatureValidation", false};
    }

private:
    std::string _previousExtensionsSignaturePublicKeyPath{""};
    unittest::ServerParameterGuard _featureFlagExtensionsApiSignatureValidation{
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
 * for both extension libfoo_mongo_extension.so and libread_n_documents_mongo_extension.so which are
 * signed with the same key. This simulates loading multiple extensions in a loop and validating
 * their signature with the same signature validator, as is done at server start-up time.
 */
TEST_F(SignatureValidatorTest,
       InsecureModeNonEmptyPublicKeyValidateMultipleTestExtensionsSucceeds) {
    SignatureValidatorForTest signatureValidator(false);
    ASSERT_FALSE(signatureValidator.secureModeEnabled());
    ASSERT_FALSE(signatureValidator.skipValidation());
    signatureValidator.validateExtensionSignature(
        kTestFooLibExtensionName, test_util::getExtensionPath(kTestFooLibExtensionName));
    signatureValidator.validateExtensionSignature(
        kTestFooLibExtensionName, test_util::getExtensionPath(kTestReadNDocumentsLibExtensionName));
}

/**
 * InsecureModeReturnsProcFdPathForValidExtension: a successfully validated extension yields a
 * "/proc/self/fd/N" path pinned to the verified bytes, which the loader hands to dlopen so the
 * bytes verified are the bytes loaded.
 */
TEST_F(SignatureValidatorTest, InsecureModeReturnsProcFdPathForValidExtension) {
    const std::string extensionName = "foo_extension_copy.so";
    const auto extensionPath = copySignedExtension(getTempDirPath(), kTestFooLibExtensionName);
    // The copied .so keeps the source's filename internally, but we exercise the API name argument.
    const std::filesystem::path renamed = getTempDirPath() / extensionName;
    std::filesystem::rename(extensionPath, renamed);
    std::filesystem::rename(std::string{extensionPath} + ".sig", std::string{renamed} + ".sig");

    SignatureValidatorForTest signatureValidator(false);
    ASSERT_FALSE(signatureValidator.skipValidation());
    const ValidatedExtension verifiedFile =
        signatureValidator.validateExtensionSignature(extensionName, renamed.string());
    ASSERT_TRUE(verifiedFile.path().starts_with("/proc/self/fd/"));
}

/**
 * SkippedValidationReturnsOriginalPath: when validation is skipped there is nothing to protect, so
 * the original on-disk path is returned unchanged (and no file is even opened).
 */
TEST_F(SignatureValidatorTest, SkippedValidationReturnsOriginalPath) {
    serverGlobalParams.extensionsSignaturePublicKeyPath = "";
    SignatureValidatorForTest signatureValidator(false);
    ASSERT_TRUE(signatureValidator.skipValidation());
    ASSERT_EQ(signatureValidator.validateExtensionSignature("foo.so", "/some/path/foo.so").path(),
              "/some/path/foo.so");
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
 * non-existent file path fails when the validator tries to open it. The correct file name for
 * extension foo is libfoo_mongo_extension.so.
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
            10929850);
    }
#endif
    {
        SignatureValidatorForTest signatureValidator(true);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath),
            AssertionException,
            10929850);
    }
}

/**
 * RejectsGroupOrOtherWritableExtension: an extension file that is group- or other-writable is
 * rejected, since such a file could be overwritten in place by another user between signature
 * verification and dlopen. The permission gate runs before key-specific verification, so it is
 * exercised in both secure and insecure mode.
 */
TEST_F(SignatureValidatorTest, RejectsGroupOrOtherWritableExtension) {
    namespace fs = std::filesystem;
    int i = 0;
    for (const auto writeBit : {fs::perms::group_write, fs::perms::others_write}) {
        // Distinct subdirectory per iteration so we never have to overwrite a read-only copy.
        const auto extensionPath =
            copySignedExtension(getTempDirPath() / std::to_string(i++), kTestFooLibExtensionName);
        fs::permissions(extensionPath, writeBit, fs::perm_options::add);
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
        {
            SignatureValidatorForTest signatureValidator(false);
            ASSERT_THROWS_CODE(signatureValidator.validateExtensionSignature(
                                   kTestFooLibExtensionName, extensionPath.string()),
                               AssertionException,
                               10929854);
        }
#endif
        {
            SignatureValidatorForTest signatureValidator(true);
            ASSERT_THROWS_CODE(signatureValidator.validateExtensionSignature(
                                   kTestFooLibExtensionName, extensionPath.string()),
                               AssertionException,
                               10929854);
        }
    }
}

/**
 * ValidatingExtensionWithMissingSignatureFails: tests that validating an existing extension whose
 * detached signature file is absent fails with the signature-not-found error. This check is
 * independent of the signing key, so it is exercised in both secure and insecure mode.
 */
TEST_F(SignatureValidatorTest, ValidatingExtensionWithMissingSignatureFails) {
    const std::string extensionName = "foo_extension_copy.so";
    const std::filesystem::path extensionPath = getTempDirPath() / extensionName;
    // Copy only the .so (no .sig) so the file opens and passes the permission gate, but the
    // signature lookup fails.
    std::filesystem::copy_file(test_util::getExtensionPath(kTestFooLibExtensionName),
                               extensionPath,
                               std::filesystem::copy_options::overwrite_existing);
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
    {
        SignatureValidatorForTest signatureValidator(false);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath.string()),
            AssertionException,
            11528923);
    }
#endif
    {
        SignatureValidatorForTest signatureValidator(true);
        ASSERT_THROWS_CODE(
            signatureValidator.validateExtensionSignature(extensionName, extensionPath.string()),
            AssertionException,
            11528923);
    }
}


#ifndef MONGO_CONFIG_EXT_SIG_SECURE
/**
 * InsecureModeValidatingFooExtensionWithReadNDocumentsSignatureFails: tests that validating the foo
 * extension with the mongot signing key fails. This is expected, since the test foo extension is
 * signed with a different key than the mongot-extension signing key.
 */
TEST_F(SignatureValidatorTest, InsecureModeValidatingFooExtensionWithReadNDocumentsSignatureFails) {
    const std::string extensionName = "foo_extension_copy.so";
    const std::filesystem::path extensionPath = getTempDirPath() / extensionName;

    // 1) Copy foo extension to our test extension path.
    std::filesystem::copy_file(test_util::getExtensionPath(kTestFooLibExtensionName),
                               extensionPath,
                               std::filesystem::copy_options::overwrite_existing);

    // 2) Copy foo extension's signature file to the extension path.
    const std::filesystem::path kExtensionSignaturePath = std::string(extensionPath) + ".sig";

    std::filesystem::copy_file(std::string{test_util::getExtensionPath(kTestFooLibExtensionName)} +
                                   ".sig",
                               kExtensionSignaturePath,
                               std::filesystem::copy_options::overwrite_existing);

    SignatureValidatorForTest signatureValidator(false);
    // 3) Sanity check, validating test extension with its signature succeeds.
    signatureValidator.validateExtensionSignature(extensionName, extensionPath);

    // 4) Swap out the signature file with the contents of a different extension's signature
    // file.
    std::filesystem::remove(kExtensionSignaturePath);
    std::filesystem::copy_file(
        std::string{test_util::getExtensionPath(kTestReadNDocumentsLibExtensionName)} + ".sig",
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
    const std::filesystem::path extensionSignaturePath = std::string{extensionPath} + ".sig";

    std::filesystem::copy_file(std::string{test_util::getExtensionPath(kTestFooLibExtensionName)} +
                                   ".sig",
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
