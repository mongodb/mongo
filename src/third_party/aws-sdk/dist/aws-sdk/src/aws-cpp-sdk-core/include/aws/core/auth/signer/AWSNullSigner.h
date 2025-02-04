/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include "aws/core/auth/signer/AWSAuthSignerBase.h"

namespace Aws
{
    namespace Http
    {
        class HttpRequest;
    } // namespace Http

    namespace Utils
    {
        namespace Event
        {
            class Message;
        }
    } // namespace Utils

    namespace Auth
    {
        AWS_CORE_API extern const char NULL_SIGNER[];
    } // namespace Auth

    namespace Client
    {
        /**
         * A no-op implementation of the AWSAuthSigner interface
         */
        class AWS_CORE_API AWSNullSigner : public AWSAuthSigner
        {
        public:
            /**
             * AWSNullSigner's implementation of virtual function from base class
             * Here the returned value is specified in Aws::Auth::NULL_SIGNER.
             */
            const char* GetName() const override { return Aws::Auth::NULL_SIGNER; }

            /**
             * Do nothing
             */
            bool SignRequest(Aws::Http::HttpRequest&) const override { return true; }

            /**
             * Do nothing
             */
            bool SignEventMessage(Aws::Utils::Event::Message&, Aws::String& /* priorSignature */) const override { return true; }

            /**
             * Do nothing
             */
            bool PresignRequest(Aws::Http::HttpRequest&, long long) const override { return false; }

            /**
             * Do nothing
             */
            bool PresignRequest(Aws::Http::HttpRequest&, const char*, long long) const override { return false; }

            /**
             * Do nothing
             */
            bool PresignRequest(Aws::Http::HttpRequest&, const char*, const char*, long long) const override { return false; }
        };

    } // namespace Client
} // namespace Aws

