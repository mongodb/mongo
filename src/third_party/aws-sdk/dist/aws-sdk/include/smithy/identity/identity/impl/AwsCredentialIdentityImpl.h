/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <smithy/identity/identity/AwsCredentialIdentity.h>

namespace smithy {
    inline Aws::String AwsCredentialIdentity::accessKeyId() const {
        return m_accessKeyId;
    }

    inline Aws::String AwsCredentialIdentity::secretAccessKey() const {
        return m_secretAccessKey;
    }

    inline Aws::Crt::Optional<Aws::String> AwsCredentialIdentity::sessionToken() const {
        return m_sessionToken;
    }

    inline Aws::Crt::Optional<AwsIdentity::DateTime> AwsCredentialIdentity::expiration() const {
        return m_expiration;
    }
}