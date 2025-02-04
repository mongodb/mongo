/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <smithy/interceptor/InterceptorContext.h>

namespace smithy
{
    namespace interceptor
    {
        class Interceptor
        {
        public:
            virtual ~Interceptor() = default;

            using ModifyRequestOutcome = Aws::Utils::Outcome<std::shared_ptr<Aws::Http::HttpRequest>, Aws::Client::AWSError<Aws::Client::CoreErrors>>;
            virtual ModifyRequestOutcome ModifyBeforeSigning(InterceptorContext& context) = 0;

            using ModifyResponseOutcome = Aws::Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, Aws::Client::AWSError<Aws::Client::CoreErrors>>;
            virtual ModifyResponseOutcome ModifyBeforeDeserialization(InterceptorContext& context) = 0;
        };
    }
}
