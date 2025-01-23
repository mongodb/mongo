/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include "aws/core/auth/signer/AWSAuthSignerBase.h"

#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSSet.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha256HMAC.h>

#include <aws/crt/auth/Sigv4Signing.h>

#include <memory>

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
        class AWSCredentials;
        class AWSCredentialsProvider;

        AWS_CORE_API extern const char EVENTSTREAM_SIGV4_SIGNER[];
    } // namespace Auth

    namespace Client
    {
        /**
         * AWS Auth EventStream v4 Signer implementation of the AWSAuthSigner interface.
         */
        class AWS_CORE_API AWSAuthEventStreamV4Signer : public AWSAuthSigner
        {
        public:
            AWSAuthEventStreamV4Signer(const std::shared_ptr<Auth::AWSCredentialsProvider>& credentialsProvider,
                    const char* serviceName, const Aws::String& region);

            const char* GetName() const override { return Aws::Auth::EVENTSTREAM_SIGV4_SIGNER; }

            bool SignEventMessage(Aws::Utils::Event::Message&, Aws::String& priorSignature) const override;

            bool SignRequest(Aws::Http::HttpRequest& request) const override
            {
                return SignRequest(request, m_region.c_str(), m_serviceName.c_str(), true);
            }

            bool SignRequest(Aws::Http::HttpRequest& request, bool signBody) const override
            {
                return SignRequest(request, m_region.c_str(), m_serviceName.c_str(), signBody);
            }

            bool SignRequest(Aws::Http::HttpRequest& request, const char* region, bool signBody) const override
            {
                return SignRequest(request, region, m_serviceName.c_str(), signBody);
            }

            bool SignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, bool signBody) const override;

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

            bool ShouldSignHeader(const Aws::String& header) const;
        private:
            Utils::ByteBuffer GenerateSignature(const Aws::Auth::AWSCredentials& credentials,
                    const Aws::String& stringToSign, const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const;
            Utils::ByteBuffer GenerateSignature(const Aws::String& stringToSign, const Aws::Utils::ByteBuffer& key) const;
            Aws::String GenerateStringToSign(const Aws::String& dateValue, const Aws::String& simpleDate,
                    const Aws::String& canonicalRequestHash, const Aws::String& region,
                    const Aws::String& serviceName) const;
            Aws::Utils::ByteBuffer ComputeHash(const Aws::String& secretKey, const Aws::String& simpleDate) const;
            Aws::Utils::ByteBuffer ComputeHash(const Aws::String& secretKey,
                    const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const;
            const Aws::String m_serviceName;
            const Aws::String m_region;
            mutable Utils::Threading::ReaderWriterLock m_derivedKeyLock;
            mutable Aws::Utils::ByteBuffer m_derivedKey;
            mutable Aws::String m_currentDateStr;
            mutable Aws::String m_currentSecretKey;
            Aws::Vector<Aws::String> m_unsignedHeaders;
            std::shared_ptr<Auth::AWSCredentialsProvider> m_credentialsProvider;
        };
    } // namespace Client
} // namespace Aws

