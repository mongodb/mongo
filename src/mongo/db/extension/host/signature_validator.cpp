// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/extension/host/signature_validator.h"

#include "mongo/db/extension/host/mongot_extension_signing_key.h"
#include "mongo/db/extension/host/rnp/rnp.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>
#include <sys/stat.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {


namespace {
void verifyExtensionPermissions(const std::string& extensionName,
                                const std::string& extensionPath,
                                int extensionFd) {
    // Refuse to load a file that an untrusted party could mutate underneath us between verification
    // and dlopen. The file must be a regular file owned by a trusted principal (root or this
    // process's effective user) and must not be group/other-writable. Owner-write is acceptable
    // because only a trusted owner (or root) could exercise it. chmod is governed by ownership
    // rather than the write bit, so requiring a trusted owner is what keeps this permission
    // snapshot durable through the subsequent dlopen.
    struct stat fileStat;
    uassert(10929851,
            fmt::format("Failed to verify extension signature for extension: {}. Could not stat "
                        "'{}': {}",
                        extensionName,
                        extensionPath,
                        errorMessage(lastSystemError())),
            ::fstat(extensionFd, &fileStat) == 0);
    uassert(10929852,
            fmt::format("Failed to verify extension signature for extension: {}. '{}' is not a "
                        "regular file",
                        extensionName,
                        extensionPath),
            S_ISREG(fileStat.st_mode));
    uassert(10929853,
            fmt::format("Failed to verify extension signature for extension: {}. '{}' must be "
                        "owned by root or the server's user",
                        extensionName,
                        extensionPath),
            fileStat.st_uid == 0 || fileStat.st_uid == ::geteuid());
    uassert(10929854,
            fmt::format("Failed to verify extension signature for extension: {}. '{}' must not be "
                        "writable by group or other users",
                        extensionName,
                        extensionPath),
            (fileStat.st_mode & (S_IWGRP | S_IWOTH)) == 0);
}
}  // namespace

SignatureValidator::SignatureValidator()
    : SignatureValidator([]() {
#ifdef MONGO_CONFIG_EXT_SIG_SECURE
          return true;
#else
          return false;
#endif
      }()) {
}

SignatureValidator::SignatureValidator(bool secureMode)
    : _secureMode(secureMode), _skipValidation([&]() {
          /**
           * featureFlagExtensionsApiSignatureValidation is only configurable at start-up, and can
           * never be modified at runtime. This feature flag is in place to allow us to disable
           * signature verification if an issue is found during the Atlas roll-out of 9.0. If an
           * issue is found during roll-out, we'll continue loading extensions on Atlas, but we'll
           * flip this feature flag off to skip signature verification. Instead, we'll continue to
           * rely on the automation agent to verify the authenticity of the binaries as we did in
           * the 8.3 release.
           *
           * Once the signature verification feature is full verified by Atlas roll-out, we'll
           * remove this feature flag so signature verification is always mandatory in on-premise
           * deployments.
           */
          if (!feature_flags::gFeatureFlagExtensionsApiSignatureValidation.isEnabled()) {
              LOGV2_DEBUG(11528919,
                          4,
                          "featureFlagExtensionsApiSignatureValidation is disabled, skipping "
                          "signature validation");
              return true;
          }
          // In secure build mode, signature validation is always required.
          if (secureMode) {
              return false;
          }
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
          // In insecure build mode, signature validation should be skipped if
          // extensionSignaturePublicKeyPath has not been provided to the server.
          return secureMode ? false : serverGlobalParams.extensionsSignaturePublicKeyPath.empty();
#endif
          return false;
      }()) {
    LOGV2_DEBUG(11528804, 4, "Initializing SignatureValidator");

    if (_skipValidation) {
        LOGV2_DEBUG(11528805, 4, "Skipping signature validation");
        return;
    }
    /* initialize Rnp context and import validation public key into the keyring (i.e rnpCtx) */
    _rnpCtx.initialize();
    _rnpCtx.importKey(_getValidationPublicKeyAsRnpInput());
}

SignatureValidator::~SignatureValidator() {}

ValidatedExtension SignatureValidator::validateExtensionSignature(
    const std::string& extensionName, const std::string& extensionPath) const {
    if (_skipValidation) {
        LOGV2(11528806, "Skipping signature validation");
        // Nothing is being verified, so there is no time-of-check/time-of-use window to protect;
        // load the extension directly from its on-disk path (no descriptor to own).
        return ValidatedExtension{extensionPath};
    }

    LOGV2(11528830,
          "Verifying signature for extension",
          "extensionName"_attr = extensionName,
          "path"_attr = extensionPath);

    uassert(11528810,
            fmt::format("Failed to verify extension signature for extension: {}. Extension path "
                        "did not end with .so, got: '{}'",
                        extensionName,
                        extensionPath),
            extensionPath.ends_with(".so"));

    // Open the extension and pin it to a file descriptor. We verify the signature against the file
    // descriptor directly, and callers ultimately load from the bytes behind this exact descriptor.
    // This  prevents a local attacker with write access to the path from swapping or overwriting
    // the library between verification and dlopen.
    const int extensionFd = ::open(extensionPath.c_str(), O_RDONLY | O_CLOEXEC);
    uassert(10929850,
            fmt::format("Failed to verify extension signature for extension: {}. Could not open "
                        "path '{}': {}",
                        extensionName,
                        extensionPath,
                        errorMessage(lastSystemError())),
            extensionFd >= 0);

    ValidatedExtension verifiedFile{extensionFd};
    verifyExtensionPermissions(extensionName, extensionPath, extensionFd);
    // The detached signature lives alongside the extension on disk, keyed off the original path.
    const std::string extensionSignaturePath = extensionPath + ".sig";
    uassert(11528923,
            fmt::format("Failed to verify extension signature for extension: {}. Extension "
                        "signature path '{}' did not exist",
                        extensionName,
                        extensionSignaturePath),
            std::filesystem::exists(extensionSignaturePath));

    try {
        _rnpCtx.verifyDetachedSignature(verifiedFile.path(), extensionSignaturePath);
    } catch (DBException& exc) {
        uasserted(11528920,
                  fmt::format("Failed to verify extension signature for extension: {}, with "
                              "signature: {}. Reason: {}",
                              extensionName,
                              extensionSignaturePath,
                              exc.what()));
    }

    return verifiedFile;
}

/**
 * This function should only be called if we aren't skipping validation (i.e_skipValidation=false),
 * since we expect a non-empty extensionValidationPublicKeyPath in insecure mode.
 */
rnp::RnpInput SignatureValidator::_getValidationPublicKeyAsRnpInput() const {
    if (_secureMode) {
        static const std::string kPublicKey(kMongoExtensionSigningPublicKey);
        return rnp::RnpInput::createFromMemory(kPublicKey, false);
    }
    // Note, extensionsSignaturePublicKeyPath only exists when MONGO_CONFIG_EXT_SIG_SECURE is not
    // defined. When built in secure mode, this code is unreachable.
#ifndef MONGO_CONFIG_EXT_SIG_SECURE
    const auto& extensionValidationPublicKeyPath =
        serverGlobalParams.extensionsSignaturePublicKeyPath;
#else
    const auto& extensionValidationPublicKeyPath = std::string("");
#endif
    tassert(11528904,
            "extensionsSignaturePublicKeyPath was empty!",
            !extensionValidationPublicKeyPath.empty());
    LOGV2_DEBUG(11528905,
                4,
                "SignatureValidator using public key path",
                "extensionValidationPublicKeyPath"_attr = extensionValidationPublicKeyPath);
    return rnp::RnpInput::createFromPath(extensionValidationPublicKeyPath);
}
}  // namespace mongo::extension::host
