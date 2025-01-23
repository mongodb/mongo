/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/http/HttpTypes.h>

namespace Aws
{
    namespace Http
    {
        class HttpRequest;
    } // namespace Http

    namespace Auth
    {
        class AWSAuthHelper
        {
        public:
            /**
             * Helper functions used across different signers
             */
            static Aws::String CanonicalizeRequestSigningString(Aws::Http::HttpRequest &request, bool urlEscapePath);
            static Aws::Http::HeaderValueCollection CanonicalizeHeaders(Http::HeaderValueCollection &&headers);

            /**
             * Static const variables used across different signers
             */
            static const char* EQ;
            static const char* AWS_HMAC_SHA256;
            static const char* AWS4_REQUEST;
            static const char* SIGNED_HEADERS;
            static const char* CREDENTIAL;
            static const char* NEWLINE;
            static const char* X_AMZN_TRACE_ID;
            static const char* X_AMZ_CONTENT_SHA256;
            static const char* SIGNING_KEY;
            static const char* SIMPLE_DATE_FORMAT_STR;
        };
    } // namespace Client
} // namespace Aws

