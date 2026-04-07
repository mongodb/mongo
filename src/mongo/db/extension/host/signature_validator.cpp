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

#include "mongo/db/extension/host/mongot_extension_signing_key.h"
#include "mongo/db/extension/host/rnp/rnp.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {

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

void SignatureValidator::validateExtensionSignature(const std::string& extensionName,
                                                    const std::string& extensionPath) const {
    if (_skipValidation) {
        LOGV2_DEBUG(11528806, 4, "Skipping signature validation");
        return;
    }

    LOGV2_DEBUG(11528830,
                4,
                "Verifying signature for extension",
                "extensionName"_attr = extensionName,
                "path"_attr = extensionPath);

    const std::string extensionSignaturePath = extensionPath + ".sig";
    uassert(11528810,
            fmt::format("Failed to verify extension signature for extension: {}. Extension path "
                        "did not end with .so, got: '{}'",
                        extensionName,
                        extensionPath),
            extensionPath.ends_with(".so"));
    uassert(11528923,
            fmt::format("Failed to verify extension signature for extension: {}. Extension "
                        "signature path '{}' did not exist",
                        extensionName,
                        extensionSignaturePath),
            std::filesystem::exists(extensionSignaturePath));
    try {
        _rnpCtx.verifyDetachedSignature(extensionPath, extensionSignaturePath);
    } catch (DBException& exc) {
        uasserted(11528920,
                  fmt::format("Failed to verify extension signature for extension: {}, with "
                              "signature: {}. Reason: {}",
                              extensionName,
                              extensionSignaturePath,
                              exc.what()));
    }
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
