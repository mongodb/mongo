/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/iot/MqttClient.h>

#include <aws/crt/Api.h>
#include <aws/crt/auth/Credentials.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/http/HttpRequestResponse.h>

#if !BYO_CRYPTO

namespace Aws
{
    namespace Iot
    {
        WebsocketConfig::WebsocketConfig(
            const Crt::String &signingRegion,
            Crt::Io::ClientBootstrap *bootstrap,
            Crt::Allocator *allocator) noexcept
            : SigningRegion(signingRegion), ServiceName("iotdevicegateway")
        {
            Crt::Auth::CredentialsProviderChainDefaultConfig config;
            config.Bootstrap = bootstrap;

            CredentialsProvider =
                Crt::Auth::CredentialsProvider::CreateCredentialsProviderChainDefault(config, allocator);

            Signer = Aws::Crt::MakeShared<Crt::Auth::Sigv4HttpRequestSigner>(allocator, allocator);

            auto credsProviderRef = CredentialsProvider;
            auto signingRegionCopy = SigningRegion;
            auto serviceNameCopy = ServiceName;
            CreateSigningConfigCb = [allocator, credsProviderRef, signingRegionCopy, serviceNameCopy]()
            {
                auto signerConfig = Aws::Crt::MakeShared<Crt::Auth::AwsSigningConfig>(allocator);
                signerConfig->SetRegion(signingRegionCopy);
                signerConfig->SetService(serviceNameCopy);
                signerConfig->SetSigningAlgorithm(Crt::Auth::SigningAlgorithm::SigV4);
                signerConfig->SetSignatureType(Crt::Auth::SignatureType::HttpRequestViaQueryParams);
                signerConfig->SetOmitSessionToken(true);
                signerConfig->SetCredentialsProvider(credsProviderRef);

                return signerConfig;
            };
        }

        WebsocketConfig::WebsocketConfig(const Crt::String &signingRegion, Crt::Allocator *allocator) noexcept
            : WebsocketConfig(signingRegion, Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap(), allocator)
        {
        }

        WebsocketConfig::WebsocketConfig(
            const Crt::String &signingRegion,
            const std::shared_ptr<Crt::Auth::ICredentialsProvider> &credentialsProvider,
            Crt::Allocator *allocator) noexcept
            : CredentialsProvider(credentialsProvider),
              Signer(Aws::Crt::MakeShared<Crt::Auth::Sigv4HttpRequestSigner>(allocator, allocator)),
              SigningRegion(signingRegion), ServiceName("iotdevicegateway")
        {
            auto credsProviderRef = CredentialsProvider;
            auto signingRegionCopy = SigningRegion;
            auto serviceNameCopy = ServiceName;
            CreateSigningConfigCb = [allocator, credsProviderRef, signingRegionCopy, serviceNameCopy]()
            {
                auto signerConfig = Aws::Crt::MakeShared<Crt::Auth::AwsSigningConfig>(allocator);
                signerConfig->SetRegion(signingRegionCopy);
                signerConfig->SetService(serviceNameCopy);
                signerConfig->SetSigningAlgorithm(Crt::Auth::SigningAlgorithm::SigV4);
                signerConfig->SetSignatureType(Crt::Auth::SignatureType::HttpRequestViaQueryParams);
                signerConfig->SetOmitSessionToken(true);
                signerConfig->SetCredentialsProvider(credsProviderRef);

                return signerConfig;
            };
        }

        WebsocketConfig::WebsocketConfig(
            const std::shared_ptr<Crt::Auth::ICredentialsProvider> &credentialsProvider,
            const std::shared_ptr<Crt::Auth::IHttpRequestSigner> &signer,
            Iot::CreateSigningConfig createSigningConfig) noexcept
            : CredentialsProvider(credentialsProvider), Signer(signer),
              CreateSigningConfigCb(std::move(createSigningConfig)), ServiceName("iotdevicegateway")
        {
        }
    } // namespace Iot
} // namespace Aws

#endif // !BYO_CRYPTO
