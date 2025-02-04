#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/io/TlsOptions.h>

#include <chrono>
#include <functional>

struct aws_credentials;
struct aws_credentials_provider;

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            class ClientBootstrap;
        }

        namespace Http
        {
            class HttpClientConnectionProxyOptions;
        }

        namespace Auth
        {
            /**
             * A class to hold the basic components necessary for various AWS authentication protocols.
             */
            class AWS_CRT_CPP_API Credentials
            {
              public:
                Credentials(const aws_credentials *credentials) noexcept;
                Credentials(
                    ByteCursor access_key_id,
                    ByteCursor secret_access_key,
                    ByteCursor session_token,
                    uint64_t expiration_timepoint_in_seconds,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Create new anonymous Credentials.
                 * Use anonymous Credentials when you want to skip signing.
                 * @param allocator
                 */
                Credentials(Allocator *allocator = ApiAllocator()) noexcept;

                ~Credentials();

                Credentials(const Credentials &) = delete;
                Credentials(Credentials &&) = delete;
                Credentials &operator=(const Credentials &) = delete;
                Credentials &operator=(Credentials &&) = delete;

                /**
                 * Gets the value of the access key component of aws credentials
                 */
                ByteCursor GetAccessKeyId() const noexcept;

                /**
                 * Gets the value of the secret access key component of aws credentials
                 */
                ByteCursor GetSecretAccessKey() const noexcept;

                /**
                 * Gets the value of the session token of aws credentials
                 */
                ByteCursor GetSessionToken() const noexcept;

                /**
                 * Gets the expiration timestamp for the credentials, or UINT64_MAX if no expiration
                 */
                uint64_t GetExpirationTimepointInSeconds() const noexcept;

                /**
                 * Validity check - returns true if the instance is valid, false otherwise
                 */
                explicit operator bool() const noexcept;

                /**
                 * Returns the underlying credentials implementation.
                 */
                const aws_credentials *GetUnderlyingHandle() const noexcept { return m_credentials; }

              private:
                const aws_credentials *m_credentials;
            };

            /**
             * Callback invoked by credentials providers when resolution succeeds (credentials will be non-null)
             * or fails (credentials will be null)
             */
            using OnCredentialsResolved = std::function<void(std::shared_ptr<Credentials>, int errorCode)>;

            /**
             * Invoked when the native delegate credentials provider needs to fetch a credential.
             */
            using GetCredentialsHandler = std::function<std::shared_ptr<Credentials>()>;

            /**
             * Base interface for all credentials providers.  Credentials providers are objects that
             * retrieve AWS credentials from some source.
             */
            class AWS_CRT_CPP_API ICredentialsProvider : public std::enable_shared_from_this<ICredentialsProvider>
            {
              public:
                virtual ~ICredentialsProvider() = default;

                /**
                 * Asynchronous method to query for AWS credentials based on the internal provider implementation.
                 */
                virtual bool GetCredentials(const OnCredentialsResolved &onCredentialsResolved) const = 0;

                /**
                 * Returns the underlying credentials provider implementation.  Support for credentials providers
                 * not based on a C implementation is theoretically possible, but requires some re-implementation to
                 * support provider chains and caching (whose implementations rely on links to C implementation
                 * providers)
                 */
                virtual aws_credentials_provider *GetUnderlyingHandle() const noexcept = 0;

                /**
                 * Validity check method
                 */
                virtual bool IsValid() const noexcept = 0;
            };

            /**
             * Configuration options for the static credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderStaticConfig
            {
                CredentialsProviderStaticConfig()
                {
                    AWS_ZERO_STRUCT(AccessKeyId);
                    AWS_ZERO_STRUCT(SecretAccessKey);
                    AWS_ZERO_STRUCT(SessionToken);
                }

                /**
                 * The value of the access key component for the provider's static aws credentials
                 */
                ByteCursor AccessKeyId;

                /**
                 * The value of the secret access key component for the provider's  static aws credentials
                 */
                ByteCursor SecretAccessKey;

                /**
                 * The value of the session token for the provider's  static aws credentials
                 */
                ByteCursor SessionToken;
            };

            /**
             * Configuration options for the profile credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderProfileConfig
            {
                CredentialsProviderProfileConfig() : Bootstrap(nullptr), TlsContext(nullptr)
                {
                    AWS_ZERO_STRUCT(ProfileNameOverride);
                    AWS_ZERO_STRUCT(ConfigFileNameOverride);
                    AWS_ZERO_STRUCT(CredentialsFileNameOverride);
                }

                /**
                 * Override profile name to use (instead of default) when the provider sources credentials
                 */
                ByteCursor ProfileNameOverride;

                /**
                 * Override file path (instead of '~/.aws/config' for the aws config file to use during
                 * credential sourcing
                 */
                ByteCursor ConfigFileNameOverride;

                /**
                 * Override file path (instead of '~/.aws/credentials' for the aws credentials file to use during
                 * credential sourcing
                 */
                ByteCursor CredentialsFileNameOverride;

                /**
                 * Connection bootstrap to use for any network connections made while sourcing credentials.
                 * (for example, a profile that uses assume-role will need to query STS).
                 */
                Io::ClientBootstrap *Bootstrap;

                /**
                 * Client TLS context to use for any secure network connections made while sourcing credentials
                 * (for example, a profile that uses assume-role will need to query STS).
                 *
                 * If a TLS context is needed, and you did not pass one in, it will be created automatically.
                 * However, you are encouraged to pass in a shared one since these are expensive objects.
                 * If using BYO_CRYPTO, you must provide the TLS context since it cannot be created automatically.
                 */
                Io::TlsContext *TlsContext;
            };

            /**
             * Configuration options for the Ec2 instance metadata service credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderImdsConfig
            {
                CredentialsProviderImdsConfig() : Bootstrap(nullptr) {}

                /**
                 * Connection bootstrap to use to create the http connection required to
                 * query credentials from the Ec2 instance metadata service
                 *
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;
            };

            /**
             * Configuration options for a chain-of-responsibility-based credentials provider.
             * This provider works by traversing the chain and returning the first positive
             * result.
             */
            struct AWS_CRT_CPP_API CredentialsProviderChainConfig
            {
                CredentialsProviderChainConfig() : Providers() {}

                /**
                 * The sequence of providers that make up the chain.
                 */
                Vector<std::shared_ptr<ICredentialsProvider>> Providers;
            };

            /**
             * Configuration options for a provider that caches the results of another provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderCachedConfig
            {
                CredentialsProviderCachedConfig() : Provider(), CachedCredentialTTL() {}

                /**
                 * The provider to cache credentials from
                 */
                std::shared_ptr<ICredentialsProvider> Provider;

                /**
                 * How long a cached credential set will be used for
                 */
                std::chrono::milliseconds CachedCredentialTTL;
            };

            /**
             * Configuration options for a provider that implements a cached provider chain
             * based on the AWS SDK defaults:
             *
             *   Cache-Of(Environment -> Profile -> IMDS)
             */
            struct AWS_CRT_CPP_API CredentialsProviderChainDefaultConfig
            {
                CredentialsProviderChainDefaultConfig() : Bootstrap(nullptr), TlsContext(nullptr) {}

                /**
                 * Connection bootstrap to use for any network connections made while sourcing credentials.
                 *
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;

                /**
                 * Client TLS context to use for any secure network connections made while sourcing credentials.
                 *
                 * If not provided the default chain will construct a new one, but these
                 * are expensive objects so you are encouraged to pass in a shared one.
                 * Must be provided if using BYO_CRYPTO.
                 */
                Io::TlsContext *TlsContext;
            };

            /**
             * Configuration options for the X509 credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderX509Config
            {
                CredentialsProviderX509Config()
                    : Bootstrap(nullptr), TlsOptions(), ThingName(), RoleAlias(), Endpoint(), ProxyOptions()
                {
                }

                /**
                 * Connection bootstrap to use to create the http connection required to
                 * query credentials from the x509 provider
                 *
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;

                /* TLS connection options that have been initialized with your x509 certificate and private key */
                Io::TlsConnectionOptions TlsOptions;

                /* IoT thing name you registered with AWS IOT for your device, it will be used in http request header */
                String ThingName;

                /* Iot role alias you created with AWS IoT for your IAM role, it will be used in http request path */
                String RoleAlias;

                /**
                 * AWS account specific endpoint that can be acquired using AWS CLI following instructions from the demo
                 * example: c2sakl5huz0afv.credentials.iot.us-east-1.amazonaws.com
                 *
                 * This a different endpoint than the IoT data mqtt broker endpoint.
                 */
                String Endpoint;

                /**
                 * (Optional) Http proxy configuration for the http request that fetches credentials
                 */
                Optional<Http::HttpClientConnectionProxyOptions> ProxyOptions;
            };

            /**
             * Configuration options for the delegate credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderDelegateConfig
            {
                /* handler to provider credentials */
                GetCredentialsHandler Handler;
            };

            /**
             * A pair defining an identity provider and a valid login token sourced from it.
             */
            struct AWS_CRT_CPP_API CognitoLoginPair
            {

                /**
                 * Name of an identity provider
                 */
                String IdentityProviderName;

                /**
                 * Valid login token source from the identity provider
                 */
                String IdentityProviderToken;
            };

            /**
             * Configuration options for the Cognito credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderCognitoConfig
            {
                CredentialsProviderCognitoConfig();

                /**
                 * Cognito service regional endpoint to source credentials from.
                 */
                String Endpoint;

                /**
                 * Cognito identity to fetch credentials relative to.
                 */
                String Identity;

                /**
                 * Optional set of identity provider token pairs to allow for authenticated identity access.
                 */
                Optional<Vector<CognitoLoginPair>> Logins;

                /**
                 * Optional ARN of the role to be assumed when multiple roles were received in the token from the
                 * identity provider.
                 */
                Optional<String> CustomRoleArn;

                /**
                 * Connection bootstrap to use to create the http connection required to
                 * query credentials from the cognito provider
                 *
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;

                /**
                 * TLS configuration for secure socket connections.
                 */
                Io::TlsContext TlsCtx;

                /**
                 * (Optional) Http proxy configuration for the http request that fetches credentials
                 */
                Optional<Http::HttpClientConnectionProxyOptions> ProxyOptions;
            };

            /**
             * Configuration options for the STS credentials provider
             */
            struct AWS_CRT_CPP_API CredentialsProviderSTSConfig
            {
                CredentialsProviderSTSConfig();

                /**
                 * Credentials provider to be used to sign the requests made to STS to fetch credentials.
                 */
                std::shared_ptr<ICredentialsProvider> Provider;

                /**
                 * Arn of the role to assume by fetching credentials for
                 */
                String RoleArn;

                /**
                 * Assumed role session identifier to be associated with the sourced credentials
                 */
                String SessionName;

                /**
                 * How long sourced credentials should remain valid for, in seconds.  900 is the minimum allowed value.
                 */
                uint16_t DurationSeconds;

                /**
                 * Connection bootstrap to use to create the http connection required to
                 * query credentials from the STS provider
                 *
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;

                /**
                 * TLS configuration for secure socket connections.
                 */
                Io::TlsContext TlsCtx;

                /**
                 * (Optional) Http proxy configuration for the http request that fetches credentials
                 */
                Optional<Http::HttpClientConnectionProxyOptions> ProxyOptions;
            };

            /**
             * Simple credentials provider implementation that wraps one of the internal C-based implementations.
             *
             * Contains a set of static factory methods for building each supported provider, as well as one for the
             * default provider chain.
             */
            class AWS_CRT_CPP_API CredentialsProvider : public ICredentialsProvider
            {
              public:
                CredentialsProvider(aws_credentials_provider *provider, Allocator *allocator = ApiAllocator()) noexcept;

                virtual ~CredentialsProvider();

                CredentialsProvider(const CredentialsProvider &) = delete;
                CredentialsProvider(CredentialsProvider &&) = delete;
                CredentialsProvider &operator=(const CredentialsProvider &) = delete;
                CredentialsProvider &operator=(CredentialsProvider &&) = delete;

                /**
                 * Asynchronous method to query for AWS credentials based on the internal provider implementation.
                 */
                virtual bool GetCredentials(const OnCredentialsResolved &onCredentialsResolved) const override;

                /**
                 * Returns the underlying credentials provider implementation.
                 */
                virtual aws_credentials_provider *GetUnderlyingHandle() const noexcept override { return m_provider; }

                /**
                 * Validity check method
                 */
                virtual bool IsValid() const noexcept override { return m_provider != nullptr; }

                /*
                 * Factory methods for all of the basic credentials provider types
                 */

                /**
                 * Creates a provider that returns a fixed set of credentials
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderStatic(
                    const CredentialsProviderStaticConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates an anonymous provider that have anonymous credentials
                 * Use anonymous credentials when you want to skip signing
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderAnonymous(
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that returns credentials sourced from environment variables
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderEnvironment(
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that returns credentials sourced from config files
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderProfile(
                    const CredentialsProviderProfileConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that returns credentials sourced from Ec2 instance metadata service
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderImds(
                    const CredentialsProviderImdsConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that sources credentials by querying a series of providers and
                 * returning the first valid credential set encountered
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderChain(
                    const CredentialsProviderChainConfig &config,
                    Allocator *allocator = ApiAllocator());

                /*
                 * Creates a provider that puts a simple time-based cache in front of its queries
                 * to a subordinate provider.
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderCached(
                    const CredentialsProviderCachedConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates the SDK-standard default credentials provider which is a cache-fronted chain of:
                 *
                 *   Environment -> Profile -> IMDS/ECS
                 *
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderChainDefault(
                    const CredentialsProviderChainDefaultConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that sources credentials from the IoT X509 provider service
                 *
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderX509(
                    const CredentialsProviderX509Config &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that sources credentials from the provided function.
                 *
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderDelegate(
                    const CredentialsProviderDelegateConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that sources credentials from the Cognito Identity service
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderCognito(
                    const CredentialsProviderCognitoConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a provider that sources credentials from STS
                 */
                static std::shared_ptr<ICredentialsProvider> CreateCredentialsProviderSTS(
                    const CredentialsProviderSTSConfig &config,
                    Allocator *allocator = ApiAllocator());

              private:
                static void s_onCredentialsResolved(aws_credentials *credentials, int error_code, void *user_data);

                Allocator *m_allocator;
                aws_credentials_provider *m_provider;
            };
        } // namespace Auth
    } // namespace Crt
} // namespace Aws
