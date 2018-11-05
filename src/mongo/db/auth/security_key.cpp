
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/security_key.h"

#include <string>
#include <sys/stat.h>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/client/authenticate.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_scram_server_conversation.h"
#include "mongo/db/auth/security_file.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/server_options.h"
#include "mongo/util/icu.h"
#include "mongo/util/log.h"
#include "mongo/util/password_digest.h"

namespace mongo {
namespace {
constexpr size_t kMinKeyLength = 6;
constexpr size_t kMaxKeyLength = 1024;

class CredentialsGenerator {
public:
    explicit CredentialsGenerator(StringData filename)
        : _salt1(scram::Presecrets<SHA1Block>::generateSecureRandomSalt()),
          _salt256(scram::Presecrets<SHA256Block>::generateSecureRandomSalt()),
          _filename(filename) {}

    boost::optional<User::CredentialData> generate(const std::string& password) {
        if (password.size() < kMinKeyLength || password.size() > kMaxKeyLength) {
            error() << " security key in " << _filename << " has length " << password.size()
                    << ", must be between 6 and 1024 chars";
            return boost::none;
        }

        auto swSaslPassword = saslPrep(password);
        if (!swSaslPassword.isOK()) {
            error() << "Could not prep security key file for SCRAM-SHA-256: "
                    << swSaslPassword.getStatus();
            return boost::none;
        }
        const auto passwordDigest = mongo::createPasswordDigest(
            internalSecurity.user->getName().getUser().toString(), password);

        User::CredentialData credentials;
        if (!_copyCredentials(
                credentials.scram_sha1,
                scram::Secrets<SHA1Block>::generateCredentials(
                    _salt1, passwordDigest, saslGlobalParams.scramSHA1IterationCount.load())))
            return boost::none;

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
            error() << "Could not generate valid credentials from key in " << _filename;
            return false;
        }

        return true;
    }

    const std::vector<uint8_t> _salt1;
    const std::vector<uint8_t> _salt256;
    const StringData _filename;
};

}  // namespace

using std::string;

bool setUpSecurityKey(const string& filename) {
    auto swKeyStrings = mongo::readSecurityFile(filename);
    if (!swKeyStrings.isOK()) {
        log() << swKeyStrings.getStatus().reason();
        return false;
    }

    auto keyStrings = std::move(swKeyStrings.getValue());

    if (keyStrings.size() > 2) {
        error() << "Only two keys are supported in the security key file, " << keyStrings.size()
                << " are specified in " << filename;
        return false;
    }

    CredentialsGenerator generator(filename);
    auto credentials = generator.generate(keyStrings.front());
    if (!credentials) {
        return false;
    }

    internalSecurity.user->setCredentials(std::move(*credentials));

    if (keyStrings.size() == 2) {
        credentials = generator.generate(keyStrings[1]);
        if (!credentials) {
            return false;
        }

        internalSecurity.alternateCredentials = std::move(*credentials);
    }

    int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_keyFile ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile) {
        auth::setInternalAuthKeys(keyStrings);
    }

    return true;
}

}  // namespace mongo
