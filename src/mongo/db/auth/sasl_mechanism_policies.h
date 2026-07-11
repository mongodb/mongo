// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/hash_block.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

struct AWSIAMPolicy {
    static constexpr std::string_view getName() {
        return auth::kMechanismMongoAWS;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText};
    }
    static int securityLevel() {
        return 1;
    }
    static constexpr bool isInternalAuthMech() {
        return false;
    }
};

struct PLAINPolicy {
    static constexpr std::string_view getName() {
        return auth::kMechanismSaslPlain;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{};
    }
    static int securityLevel() {
        return 0;
    }
    static constexpr bool isInternalAuthMech() {
        return false;
    }
};

struct SCRAMSHA1Policy {
    using HashBlock = SHA1Block;

    static constexpr std::string_view getName() {
        return auth::kMechanismScramSha1;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }
    static int securityLevel() {
        return 2;
    }
    static constexpr bool isInternalAuthMech() {
        return false;
    }
};

struct SCRAMSHA256Policy {
    using HashBlock = SHA256Block;

    static constexpr std::string_view getName() {
        return auth::kMechanismScramSha256;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }
    static int securityLevel() {
        return 2;
    }
    static constexpr bool isInternalAuthMech() {
        return true;
    }
};

struct GSSAPIPolicy {
    static constexpr std::string_view getName() {
        return auth::kMechanismGSSAPI;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }
    static int securityLevel() {
        return 3;
    }
    static constexpr bool isInternalAuthMech() {
        return false;
    }
};

struct X509Policy {
    static constexpr std::string_view getName() {
        return auth::kMechanismMongoX509;
    }

    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }

    static int securityLevel() {
        return 3;
    }

    static constexpr bool isInternalAuthMech() {
        return true;
    }
};

}  // namespace mongo
