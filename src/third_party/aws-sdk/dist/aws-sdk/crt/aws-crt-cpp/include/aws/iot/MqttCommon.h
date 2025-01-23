#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Config.h>
#include <aws/crt/Exports.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/mqtt/MqttClient.h>

#if !BYO_CRYPTO

namespace Aws
{
    namespace Iot
    {

        using CreateSigningConfig = std::function<std::shared_ptr<Crt::Auth::ISigningConfig>(void)>;

        /**
         * Class encapsulating configuration for establishing an Aws IoT mqtt connection via websockets
         */
        struct AWS_CRT_CPP_API WebsocketConfig
        {
            /**
             * Create a websocket configuration for use with the default credentials provider chain. Signing region
             * will be used for Sigv4 signature calculations.
             *
             * @param signingRegion Aws region that is being connected to.  Required in order to properly sign the
             * handshake upgrade request
             * @param bootstrap client bootstrap to establish any connections needed by the default credentials
             * provider chain which will get built for the user
             * @param allocator memory allocator to use
             */
            WebsocketConfig(
                const Crt::String &signingRegion,
                Crt::Io::ClientBootstrap *bootstrap,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Create a websocket configuration for use with the default credentials provider chain and default
             * ClientBootstrap. Signing region will be used for Sigv4 signature calculations.
             *
             * For more information on the default ClientBootstrap see
             * Aws::Crt::ApiHandle::GetOrCreateDefaultClientBootstrap
             *
             * @param signingRegion Aws region that is being connected to.  Required in order to properly sign the
             * handshake upgrade request
             * @param allocator memory allocator to use
             */
            WebsocketConfig(const Crt::String &signingRegion, Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Create a websocket configuration for use with a custom credentials provider. Signing region will be used
             * for Sigv4 signature calculations.
             *
             * @param signingRegion Aws region that is being connected to.  Required in order to properly sign the
             * handshake upgrade request
             * @param credentialsProvider credentials provider to source AWS credentials from
             * @param allocator memory allocator to use
             */
            WebsocketConfig(
                const Crt::String &signingRegion,
                const std::shared_ptr<Crt::Auth::ICredentialsProvider> &credentialsProvider,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Create a websocket configuration for use with a custom credentials provider, and a custom signer.
             *
             * You'll need to provide a function for use with creating a signing Config and pass it to
             * createSigningConfig.
             *
             * This is useful for cases use with:
             * https://docs.aws.amazon.com/iot/latest/developerguide/custom-auth.html
             *
             * @param credentialsProvider credentials provider
             * @param signer HTTP request signer
             * @param createSigningConfig function that creates a signing config
             */
            WebsocketConfig(
                const std::shared_ptr<Crt::Auth::ICredentialsProvider> &credentialsProvider,
                const std::shared_ptr<Crt::Auth::IHttpRequestSigner> &signer,
                CreateSigningConfig createSigningConfig) noexcept;

            std::shared_ptr<Crt::Auth::ICredentialsProvider> CredentialsProvider;
            std::shared_ptr<Crt::Auth::IHttpRequestSigner> Signer;
            CreateSigningConfig CreateSigningConfigCb;

            /**
             * @deprecated Specify ProxyOptions to use a proxy with your websocket connection.
             *
             * If MqttClientConnectionConfigBuilder::m_proxyOptions is valid, then that will be used over
             * this value.
             */
            Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> ProxyOptions;
            Crt::String SigningRegion;
            Crt::String ServiceName;
        };

        /**
         * A simple struct to hold the options for creating a PKCS12 builder. Used to differentiate the
         * PKCS12 builder from other options in the mTLS builders.
         */
        struct Pkcs12Options
        {
            Crt::String pkcs12_file;
            Crt::String pkcs12_password;
        };

    } // namespace Iot
} // namespace Aws

#endif // !BYO_CRYPTO
