/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/identity/AwsBearerTokenIdentityBase.h>

namespace smithy {
    class AwsBearerTokenIdentity : public AwsBearerTokenIdentityBase {
    public:
      virtual const Aws::String &token() const override;

      virtual Aws::Crt::Optional<AwsIdentity::DateTime>
      expiration() const override;

      Aws::String &token() { return m_token; }

      Aws::Crt::Optional<AwsIdentity::DateTime> &expiration()
      {
          return m_expiration;
      }

    protected:
        Aws::String m_token;
        Aws::Crt::Optional<AwsIdentity::DateTime> m_expiration;
    };
}

#include <smithy/identity/identity/impl/AwsBearerTokenIdentityImpl.h>