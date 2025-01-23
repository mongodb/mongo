/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <smithy/interceptor/Interceptor.h>

#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/crypto/MD5.h>
#include <aws/core/utils/crypto/CRC32.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha1.h>
#include <aws/core/utils/crypto/PrecalculatedHash.h>
#include <aws/core/platform/Environment.h>

#include <iomanip>

namespace smithy
{
    namespace client
    {
        static const char AWS_SMITHY_CLIENT_CHECKSUM[] = "AwsSmithyClientChecksums";

        static const char CHECKSUM_CONTENT_MD5_HEADER[] = "content-md5";

        class ChecksumInterceptor: public smithy::interceptor::Interceptor
        {
        public:
            using HeaderValueCollection = Aws::Http::HeaderValueCollection;
            using HashingUtils = Aws::Utils::HashingUtils;
            using MD5 = Aws::Utils::Crypto::MD5;
            using CRC32 = Aws::Utils::Crypto::CRC32;
            using CRC32C = Aws::Utils::Crypto::CRC32C;
            using Sha256 = Aws::Utils::Crypto::Sha256;
            using Sha1 = Aws::Utils::Crypto::Sha1;
            using PrecalculatedHash = Aws::Utils::Crypto::PrecalculatedHash;

            ~ChecksumInterceptor() override = default;
            ChecksumInterceptor() = default;
            ChecksumInterceptor(const ChecksumInterceptor& other) = delete;
            ChecksumInterceptor(ChecksumInterceptor&& other) noexcept = default;
            ChecksumInterceptor& operator=(const ChecksumInterceptor& other) = delete;
            ChecksumInterceptor& operator=(ChecksumInterceptor&& other) noexcept = default;

            static std::shared_ptr<Aws::IOStream> GetBodyStream(const Aws::AmazonWebServiceRequest& request)
            {
                if (request.GetBody() != nullptr)
                {
                    return request.GetBody();
                }
                // Return an empty string stream for no body
                return Aws::MakeShared<Aws::StringStream>(AWS_SMITHY_CLIENT_CHECKSUM, "");
            }

            ModifyRequestOutcome ModifyBeforeSigning(interceptor::InterceptorContext& context) override
            {
                const auto& httpRequest = context.GetTransmitRequest();
                const auto& request = context.GetModeledRequest();
                if (httpRequest == nullptr)
                {
                    return Aws::Client::AWSError<Aws::Client::CoreErrors>{Aws::Client::CoreErrors::VALIDATION,
                        "ValidationErrorException",
                        "Checksum request validation missing request",
                        false};
                }
                Aws::String checksumAlgorithmName = Aws::Utils::StringUtils::ToLower(
                    request.GetChecksumAlgorithmName().c_str());
                if (request.GetServiceSpecificParameters())
                {
                    auto requestChecksumOverride = request.GetServiceSpecificParameters()->parameterMap.find(
                        "overrideChecksum");
                    if (requestChecksumOverride != request.GetServiceSpecificParameters()->parameterMap.end())
                    {
                        checksumAlgorithmName = requestChecksumOverride->second;
                    }
                }

                bool shouldSkipChecksum = request.GetServiceSpecificParameters() &&
                    request.GetServiceSpecificParameters()->parameterMap.find("overrideChecksumDisable") !=
                    request.GetServiceSpecificParameters()->parameterMap.end();

                //Check if user has provided the checksum algorithm
                if (!checksumAlgorithmName.empty() && !shouldSkipChecksum)
                {
                    // Check if user has provided a checksum value for the specified algorithm
                    const Aws::String checksumType = "x-amz-checksum-" + checksumAlgorithmName;
                    const HeaderValueCollection& headers = request.GetHeaders();
                    const auto checksumHeader = headers.find(checksumType);
                    bool checksumValueAndAlgorithmProvided = checksumHeader != headers.end();

                    // For non-streaming payload, the resolved checksum location is always header.
                    // For streaming payload, the resolved checksum location depends on whether it is an unsigned payload, we let AwsAuthSigner decide it.
                    if (request.IsStreaming() && checksumValueAndAlgorithmProvided)
                    {
                        const auto hash = Aws::MakeShared<PrecalculatedHash>(
                            AWS_SMITHY_CLIENT_CHECKSUM, checksumHeader->second);
                        httpRequest->SetRequestHash(checksumAlgorithmName, hash);
                    }
                    else if (checksumValueAndAlgorithmProvided)
                    {
                        httpRequest->SetHeaderValue(checksumType, checksumHeader->second);
                    }
                    else if (checksumAlgorithmName == "crc32")
                    {
                        if (request.IsStreaming())
                        {
                            httpRequest->SetRequestHash(checksumAlgorithmName,
                                                        Aws::MakeShared<CRC32>(AWS_SMITHY_CLIENT_CHECKSUM));
                        }
                        else
                        {
                            httpRequest->SetHeaderValue(checksumType,
                                                        HashingUtils::Base64Encode(
                                                            HashingUtils::CalculateCRC32(*(GetBodyStream(request)))));
                        }
                    }
                    else if (checksumAlgorithmName == "crc32c")
                    {
                        if (request.IsStreaming())
                        {
                            httpRequest->SetRequestHash(checksumAlgorithmName,
                                                        Aws::MakeShared<CRC32C>(AWS_SMITHY_CLIENT_CHECKSUM));
                        }
                        else
                        {
                            httpRequest->SetHeaderValue(checksumType,
                                                        HashingUtils::Base64Encode(
                                                            HashingUtils::CalculateCRC32C(*(GetBodyStream(request)))));
                        }
                    }
                    else if (checksumAlgorithmName == "sha256")
                    {
                        if (request.IsStreaming())
                        {
                            httpRequest->SetRequestHash(checksumAlgorithmName,
                                                        Aws::MakeShared<Sha256>(AWS_SMITHY_CLIENT_CHECKSUM));
                        }
                        else
                        {
                            httpRequest->SetHeaderValue(checksumType,
                                                        HashingUtils::Base64Encode(
                                                            HashingUtils::CalculateSHA256(*(GetBodyStream(request)))));
                        }
                    }
                    else if (checksumAlgorithmName == "sha1")
                    {
                        if (request.IsStreaming())
                        {
                            httpRequest->SetRequestHash(checksumAlgorithmName,
                                                        Aws::MakeShared<Sha1>(AWS_SMITHY_CLIENT_CHECKSUM));
                        }
                        else
                        {
                            httpRequest->SetHeaderValue(checksumType,
                                                        HashingUtils::Base64Encode(
                                                            HashingUtils::CalculateSHA1(*(GetBodyStream(request)))));
                        }
                    }
                    else if (checksumAlgorithmName == "md5" && headers.find(CHECKSUM_CONTENT_MD5_HEADER) == headers.end())
                    {
                        httpRequest->SetHeaderValue(CHECKSUM_CONTENT_MD5_HEADER,
                                                    HashingUtils::Base64Encode(
                                                        HashingUtils::CalculateMD5(*(GetBodyStream(request)))));
                    }
                    else
                    {
                        AWS_LOGSTREAM_WARN(AWS_SMITHY_CLIENT_CHECKSUM,
                                           "Checksum algorithm: " << checksumAlgorithmName <<
                                           "is not supported by SDK.");
                    }
                }

                // Response checksums
                if (request.ShouldValidateResponseChecksum())
                {
                    for (const Aws::String& responseChecksumAlgorithmName : request.GetResponseChecksumAlgorithmNames())
                    {
                        checksumAlgorithmName = Aws::Utils::StringUtils::ToLower(responseChecksumAlgorithmName.c_str());

                        if (checksumAlgorithmName == "crc32c")
                        {
                            std::shared_ptr<CRC32C> crc32c = Aws::MakeShared<
                                CRC32C>(AWS_SMITHY_CLIENT_CHECKSUM);
                            httpRequest->AddResponseValidationHash("crc32c", crc32c);
                        }
                        else if (checksumAlgorithmName == "crc32")
                        {
                            std::shared_ptr<CRC32> crc32 = Aws::MakeShared<
                                CRC32>(AWS_SMITHY_CLIENT_CHECKSUM);
                            httpRequest->AddResponseValidationHash("crc32", crc32);
                        }
                        else if (checksumAlgorithmName == "sha1")
                        {
                            std::shared_ptr<Sha1> sha1 = Aws::MakeShared<Sha1>(
                                AWS_SMITHY_CLIENT_CHECKSUM);
                            httpRequest->AddResponseValidationHash("sha1", sha1);
                        }
                        else if (checksumAlgorithmName == "sha256")
                        {
                            std::shared_ptr<Sha256> sha256 = Aws::MakeShared<
                                Sha256>(AWS_SMITHY_CLIENT_CHECKSUM);
                            httpRequest->AddResponseValidationHash("sha256", sha256);
                        }
                        else
                        {
                            AWS_LOGSTREAM_WARN(AWS_SMITHY_CLIENT_CHECKSUM,
                                               "Checksum algorithm: " << checksumAlgorithmName <<
                                               " is not supported in validating response body yet.");
                        }
                    }
                }
                return httpRequest;
            }

            ModifyResponseOutcome ModifyBeforeDeserialization(interceptor::InterceptorContext& context) override
            {
                const auto httpRequest = context.GetTransmitRequest();
                const auto httpResponse = context.GetTransmitResponse();
                if (httpRequest == nullptr || httpResponse == nullptr)
                {
                    return Aws::Client::AWSError<Aws::Client::CoreErrors>{Aws::Client::CoreErrors::VALIDATION,
                        "ValidationErrorException",
                        "Checksum response validation missing request or response",
                        false};
                }
                for (const auto& hashIterator : httpRequest->GetResponseValidationHashes())
                {
                    Aws::String checksumHeaderKey = Aws::String("x-amz-checksum-") + hashIterator.first;
                    // TODO: If checksum ends with -#, then skip
                    if (httpResponse->HasHeader(checksumHeaderKey.c_str()))
                    {
                        const Aws::String& checksumHeaderValue = httpResponse->GetHeader(checksumHeaderKey);
                        if (HashingUtils::Base64Encode(hashIterator.second->GetHash().GetResult()) !=
                            checksumHeaderValue)
                        {
                            auto error = Aws::Client::AWSError<Aws::Client::CoreErrors>{
                                    Aws::Client::CoreErrors::VALIDATION, "",
                                    "Response checksums mismatch",
                                    false/*retryable*/};
                            error.SetResponseHeaders(httpResponse->GetHeaders());
                            error.SetResponseCode(httpResponse->GetResponseCode());
                            error.SetRemoteHostIpAddress(
                            httpResponse->GetOriginatingRequest().GetResolvedRemoteHost());

                            AWS_LOGSTREAM_ERROR(AWS_SMITHY_CLIENT_CHECKSUM, error);
                            return {error};
                        }
                        // Validate only a single checksum returned in an HTTP response
                        break;
                    }
                }
                return httpResponse;
            }
        };
    }
}
