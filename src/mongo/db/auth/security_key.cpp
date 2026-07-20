// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/security_key.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/internal_auth.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/security_file.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/icu.h"
#include "mongo/util/password_digest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {
constexpr size_t kMinKeyLength = 6;
constexpr size_t kMaxKeyLength = 1024;

class CredentialsGenerator {
public:
    explicit CredentialsGenerator(std::string_view filename)
        : _salt256(scram::Presecrets<SHA256Block>::generateSecureRandomSalt()),
          _filename(filename) {}

    boost::optional<User::CredentialData> generate(const std::string& password) {
        if (password.size() < kMinKeyLength || password.size() > kMaxKeyLength) {
            LOGV2_ERROR(20255,
                        "Security key size is out range",
                        "filename"_attr = _filename,
                        "size"_attr = password.size(),
                        "minimumLength"_attr = kMinKeyLength,
                        "maximumLength"_attr = kMaxKeyLength);
            return boost::none;
        }

        auto swSaslPassword = icuSaslPrep(password);
        if (!swSaslPassword.isOK()) {
            LOGV2_ERROR(20256,
                        "Could not prep security key file for SCRAM-SHA-256",
                        "error"_attr = swSaslPassword.getStatus());
            return boost::none;
        }

        User::CredentialData credentials;

        if (!_copyCredentials(credentials.scram_sha256,
                              scram::Secrets<SHA256Block>::generateCredentials(
                                  _salt256,
                                  swSaslPassword.getValue(),
                                  saslGlobalParams.scramSHA256IterationCount.load())))
            return boost::none;

        return credentials;
    }

private:
    template <typename CredsTarget, typename CredsSource>
    bool _copyCredentials(CredsTarget&& target, const CredsSource&& source) {
        target.iterationCount = source[scram::kIterationCountFieldName].Int();
        target.salt = source[scram::kSaltFieldName].String();
        target.storedKey = source[scram::kStoredKeyFieldName].String();
        target.serverKey = source[scram::kServerKeyFieldName].String();
        if (!target.isValid()) {
            LOGV2_ERROR(20257,
                        "Could not generate valid credentials from key",
                        "filename"_attr = _filename);
            return false;
        }

        return true;
    }

    const std::vector<uint8_t> _salt256;
    const std::string_view _filename;
};

}  // namespace

using std::string;

bool setUpSecurityKey(const string& filename, ClusterAuthMode mode) {
    auto swKeyStrings = mongo::readSecurityFile(filename);
    if (!swKeyStrings.isOK()) {
        return false;
    }

    auto keyStrings = std::move(swKeyStrings.getValue());

    if (keyStrings.size() > 2) {
        LOGV2_ERROR(20258,
                    "Only two keys are supported in the security key file",
                    "numKeys"_attr = keyStrings.size(),
                    "filename"_attr = filename);
        return false;
    }

    CredentialsGenerator generator(filename);
    auto credentials = generator.generate(keyStrings.front());
    if (!credentials) {
        return false;
    }

    internalSecurity.credentials = credentials;
    (*internalSecurity.getUser())->setCredentials(credentials.value());

    if (keyStrings.size() == 2) {
        credentials = generator.generate(keyStrings[1]);
        if (!credentials) {
            return false;
        }

        internalSecurity.alternateCredentials = std::move(*credentials);
    }

    if (mode.sendsKeyFile()) {
        auth::setInternalAuthKeys(keyStrings);
    }

    return true;
}

}  // namespace mongo
