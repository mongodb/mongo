#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>

#include <aws/crt/DateTime.h>
#include <aws/crt/Types.h>
#include <aws/crt/auth/Signing.h>

struct aws_signing_config_aws;

namespace Aws
{
    namespace Crt
    {
        namespace Auth
        {
            class Credentials;
            class ICredentialsProvider;

            /**
             * Enumeration indicating what version of the AWS signing process we should use.
             */
            enum class SigningAlgorithm
            {
                /**
                 * Standard AWS Sigv4 signing using a symmetric secret, per
                 * https://docs.aws.amazon.com/general/latest/gr/signature-version-4.html
                 */
                SigV4 = AWS_SIGNING_ALGORITHM_V4,

                /**
                 * A variant of AWS Sigv4 signing that uses ecdsa signatures based on an ECC key, rather than relying on
                 * a shared secret.
                 */
                SigV4A = AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC,
            };

            /**
             * What kind of AWS signature should be computed?
             */
            enum class SignatureType
            {
                /**
                 * A signature for a full http request should be computed, with header updates applied to the signing
                 * result.
                 */
                HttpRequestViaHeaders = AWS_ST_HTTP_REQUEST_HEADERS,

                /**
                 * A signature for a full http request should be computed, with query param updates applied to the
                 * signing result.
                 */
                HttpRequestViaQueryParams = AWS_ST_HTTP_REQUEST_QUERY_PARAMS,

                /**
                 * Compute a signature for a payload chunk.
                 */
                HttpRequestChunk = AWS_ST_HTTP_REQUEST_CHUNK,

                /**
                 * Compute a signature for an event stream event.
                 *
                 * This option is not yet supported.
                 */
                HttpRequestEvent = AWS_ST_HTTP_REQUEST_EVENT,
            };

            /**
             * A collection of signed body constants.  Some are specific to certain
             * signature types, while others are just there to save time (empty sha, for example).
             */
            namespace SignedBodyValue
            {
                /**
                 * The SHA-256 of an empty string:
                 * 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
                 * For use with `Aws::Crt::Auth::AwsSigningConfig.SetSignedBodyValue()`.
                 */
                AWS_CRT_CPP_API const char *EmptySha256Str();

                /**
                 * 'UNSIGNED-PAYLOAD'
                 * For use with `Aws::Crt::Auth::AwsSigningConfig.SetSignedBodyValue()`.
                 */
                AWS_CRT_CPP_API const char *UnsignedPayloadStr();

                /**
                 * 'STREAMING-AWS4-HMAC-SHA256-PAYLOAD'
                 * For use with `Aws::Crt::Auth::AwsSigningConfig.SetSignedBodyValue()`.
                 */
                AWS_CRT_CPP_API const char *StreamingAws4HmacSha256PayloadStr();
                /**
                 * 'STREAMING-AWS4-HMAC-SHA256-EVENTS'
                 * For use with `Aws::Crt::Auth::AwsSigningConfig.SetSignedBodyValue()`.
                 */
                AWS_CRT_CPP_API const char *StreamingAws4HmacSha256EventsStr();

                /** @deprecated to avoid issues with /DELAYLOAD on Windows. */
                AWS_CRT_CPP_API extern const char *UnsignedPayload;
                /** @deprecated to avoid issues with /DELAYLOAD on Windows. */
                AWS_CRT_CPP_API extern const char *EmptySha256;
                /** @deprecated to avoid issues with /DELAYLOAD on Windows. */
                AWS_CRT_CPP_API extern const char *StreamingAws4HmacSha256Payload;
                /** @deprecated to avoid issues with /DELAYLOAD on Windows. */
                AWS_CRT_CPP_API extern const char *StreamingAws4HmacSha256Events;
            } // namespace SignedBodyValue

            /**
             * Controls if signing adds a header containing the canonical request's body value
             */
            enum class SignedBodyHeaderType
            {
                /**
                 * Do not add a header
                 */
                None = AWS_SBHT_NONE,

                /**
                 * Add the "x-amz-content-sha256" header with the canonical request's body value
                 */
                XAmzContentSha256 = AWS_SBHT_X_AMZ_CONTENT_SHA256,
            };

            using ShouldSignHeaderCb = bool (*)(const Crt::ByteCursor *, void *);

            /**
             * Wrapper around the configuration structure specific to the AWS
             * Sigv4 signing process
             */
            class AWS_CRT_CPP_API AwsSigningConfig : public ISigningConfig
            {
              public:
                AwsSigningConfig(Allocator *allocator = ApiAllocator());
                virtual ~AwsSigningConfig();

                virtual SigningConfigType GetType() const noexcept override { return SigningConfigType::Aws; }

                /**
                 * @return the signing process we want to invoke
                 */
                SigningAlgorithm GetSigningAlgorithm() const noexcept;

                /**
                 * Sets the signing process we want to invoke
                 */
                void SetSigningAlgorithm(SigningAlgorithm algorithm) noexcept;

                /**
                 * @return the type of signature we want to calculate
                 */
                SignatureType GetSignatureType() const noexcept;

                /**
                 * Sets the type of signature we want to calculate
                 */
                void SetSignatureType(SignatureType signatureType) noexcept;

                /**
                 * @return the AWS region to sign against
                 */
                const Crt::String &GetRegion() const noexcept;

                /**
                 * Sets the AWS region to sign against
                 */
                void SetRegion(const Crt::String &region) noexcept;

                /**
                 * @return the (signing) name of the AWS service to sign a request for
                 */
                const Crt::String &GetService() const noexcept;

                /**
                 * Sets the (signing) name of the AWS service to sign a request for
                 */
                void SetService(const Crt::String &service) noexcept;

                /**
                 * @return the timestamp to use during the signing process.
                 */
                DateTime GetSigningTimepoint() const noexcept;

                /**
                 * Sets the timestamp to use during the signing process.
                 */
                void SetSigningTimepoint(const DateTime &date) noexcept;

                /*
                 * We assume the uri will be encoded once in preparation for transmission.  Certain services
                 * do not decode before checking signature, requiring us to actually double-encode the uri in the
                 * canonical request in order to pass a signature check.
                 */

                /**
                 * @return whether or not the signing process should perform a uri encode step before creating the
                 * canonical request.
                 */
                bool GetUseDoubleUriEncode() const noexcept;

                /**
                 * Sets whether or not the signing process should perform a uri encode step before creating the
                 * canonical request.
                 */
                void SetUseDoubleUriEncode(bool useDoubleUriEncode) noexcept;

                /**
                 * @return whether or not the uri paths should be normalized when building the canonical request
                 */
                bool GetShouldNormalizeUriPath() const noexcept;

                /**
                 * Sets whether or not the uri paths should be normalized when building the canonical request
                 */
                void SetShouldNormalizeUriPath(bool shouldNormalizeUriPath) noexcept;

                /**
                 * @return whether or not to omit the session token during signing.  Only set to true when performing
                 * a websocket handshake with IoT Core.
                 */
                bool GetOmitSessionToken() const noexcept;

                /**
                 * Sets whether or not to omit the session token during signing.  Only set to true when performing
                 * a websocket handshake with IoT Core.
                 */
                void SetOmitSessionToken(bool omitSessionToken) noexcept;

                /**
                 * @return the ShouldSignHeadersCb from the underlying config.
                 */
                ShouldSignHeaderCb GetShouldSignHeaderCallback() const noexcept;

                /**
                 * Sets a callback invoked during the signing process for white-listing headers that can be signed.
                 * If you do not set this, all headers will be signed.
                 */
                void SetShouldSignHeaderCallback(ShouldSignHeaderCb shouldSignHeaderCb) noexcept;

                /**
                 * @return the should_sign_header_ud from the underlying config.
                 */
                void *GetShouldSignHeaderUserData() const noexcept;

                /**
                 * Sets the userData you could get from the ShouldSignHeaderCb callback function.
                 */
                void SetShouldSignHeaderUserData(void *userData) noexcept;

                /**
                 * @return the string used as the canonical request's body value.
                 * If string is empty, a value is be calculated from the payload during signing.
                 */
                const Crt::String &GetSignedBodyValue() const noexcept;

                /**
                 * Sets the string to use as the canonical request's body value.
                 * If an empty string is set (the default), a value will be calculated from the payload during signing.
                 * Typically, this is the SHA-256 of the (request/chunk/event) payload, written as lowercase hex.
                 * If this has been precalculated, it can be set here.
                 * Special values used by certain services can also be set (see Aws::Crt::Auth::SignedBodyValue).
                 */
                void SetSignedBodyValue(const Crt::String &signedBodyValue) noexcept;

                /**
                 * @return the name of the header to add that stores the signed body value
                 */
                SignedBodyHeaderType GetSignedBodyHeader() const noexcept;

                /**
                 * Sets the name of the header to add that stores the signed body value
                 */
                void SetSignedBodyHeader(SignedBodyHeaderType signedBodyHeader) noexcept;

                /**
                 * @return (Query param signing only) Gets the amount of time, in seconds, the (pre)signed URI will be
                 * good for
                 */
                uint64_t GetExpirationInSeconds() const noexcept;

                /**
                 * (Query param signing only) Sets the amount of time, in seconds, the (pre)signed URI will be good for
                 */
                void SetExpirationInSeconds(uint64_t expirationInSeconds) noexcept;

                /*
                 * For Sigv4 signing, either the credentials provider or the credentials must be set.
                 * Credentials, if set, takes precedence over the provider.
                 */

                /**
                 *  @return the credentials provider to use for signing.
                 */
                const std::shared_ptr<ICredentialsProvider> &GetCredentialsProvider() const noexcept;

                /**
                 *  Set the credentials provider to use for signing.
                 */
                void SetCredentialsProvider(const std::shared_ptr<ICredentialsProvider> &credsProvider) noexcept;

                /**
                 *  @return the credentials to use for signing.
                 */
                const std::shared_ptr<Credentials> &GetCredentials() const noexcept;

                /**
                 *  Set the credentials to use for signing.
                 */
                void SetCredentials(const std::shared_ptr<Credentials> &credentials) noexcept;

                /// @private
                const struct aws_signing_config_aws *GetUnderlyingHandle() const noexcept;

              private:
                Allocator *m_allocator;
                std::shared_ptr<ICredentialsProvider> m_credentialsProvider;
                std::shared_ptr<Credentials> m_credentials;
                struct aws_signing_config_aws m_config;
                Crt::String m_signingRegion;
                Crt::String m_serviceName;
                Crt::String m_signedBodyValue;
            };

            /**
             * Http request signer that performs Aws Sigv4 signing.  Expects the signing configuration to be and
             * instance of AwsSigningConfig
             */
            class AWS_CRT_CPP_API Sigv4HttpRequestSigner : public IHttpRequestSigner
            {
              public:
                Sigv4HttpRequestSigner(Allocator *allocator = ApiAllocator());
                virtual ~Sigv4HttpRequestSigner() = default;

                bool IsValid() const override { return true; }

                /**
                 * Signs an http request with AWS-auth sigv4. OnCompletionCallback will be invoked upon completion.
                 */
                virtual bool SignRequest(
                    const std::shared_ptr<Aws::Crt::Http::HttpRequest> &request,
                    const ISigningConfig &config,
                    const OnHttpRequestSigningComplete &completionCallback) override;

              private:
                Allocator *m_allocator;
            };
        } // namespace Auth
    } // namespace Crt
} // namespace Aws
