#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>

#include <aws/auth/signing_config.h>

#include <functional>
#include <memory>

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            class HttpRequest;
        }

        namespace Auth
        {
            /**
             * RTTI indicator for signing configuration.  We currently only support a single type (AWS), but
             * we could expand to others in the future if needed.
             */
            enum class SigningConfigType
            {
                Aws = AWS_SIGNING_CONFIG_AWS
            };

            /**
             * HTTP signing callback.  The second parameter is an aws error code,  The signing was successful
             * if the error code is AWS_ERROR_SUCCESS.
             */
            using OnHttpRequestSigningComplete =
                std::function<void(const std::shared_ptr<Aws::Crt::Http::HttpRequest> &, int)>;

            /**
             * Base class for all different signing configurations.  Type functions as a
             * primitive RTTI for downcasting.
             */
            class AWS_CRT_CPP_API ISigningConfig
            {
              public:
                ISigningConfig() = default;
                ISigningConfig(const ISigningConfig &) = delete;
                ISigningConfig(ISigningConfig &&) = delete;
                ISigningConfig &operator=(const ISigningConfig &) = delete;
                ISigningConfig &operator=(ISigningConfig &&) = delete;

                virtual ~ISigningConfig() = default;

                /**
                 * RTTI query for the SigningConfig hierarchy
                 * @return the type of signing configuration
                 */
                virtual SigningConfigType GetType(void) const = 0;
            };

            /**
             * Abstract base for all http request signers.  Asynchronous interface.  Intended to
             * be a tight wrapper around aws-c-* signer implementations.
             */
            class AWS_CRT_CPP_API IHttpRequestSigner
            {
              public:
                IHttpRequestSigner() = default;
                IHttpRequestSigner(const IHttpRequestSigner &) = delete;
                IHttpRequestSigner(IHttpRequestSigner &&) = delete;
                IHttpRequestSigner &operator=(const IHttpRequestSigner &) = delete;
                IHttpRequestSigner &operator=(IHttpRequestSigner &&) = delete;

                virtual ~IHttpRequestSigner() = default;

                /**
                 * Signs an http request based on the signing implementation and supplied configuration
                 * @param request http request to sign
                 * @param config base signing configuration.  Actual type should match the configuration expected
                 * by the signer implementation
                 * @param completionCallback completion function to invoke when signing has completed or failed
                 * @return true if the signing process was kicked off, false if there was a synchronous failure.
                 */
                virtual bool SignRequest(
                    const std::shared_ptr<Aws::Crt::Http::HttpRequest> &request,
                    const ISigningConfig &config,
                    const OnHttpRequestSigningComplete &completionCallback) = 0;

                /**
                 * @return Whether or not the signer is in a valid state
                 */
                virtual bool IsValid() const = 0;
            };

        } // namespace Auth
    } // namespace Crt
} // namespace Aws
