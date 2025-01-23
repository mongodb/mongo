/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/crt/Optional.h>

#include <aws/core/utils/DateTime.h>

namespace smithy {
    class AwsIdentity {
    public:
        using DateTime = Aws::Utils::DateTime;

        virtual ~AwsIdentity(){};
        virtual Aws::Crt::Optional<DateTime> expiration() const {
            return Aws::Crt::Optional<DateTime>();
        };
    };
}