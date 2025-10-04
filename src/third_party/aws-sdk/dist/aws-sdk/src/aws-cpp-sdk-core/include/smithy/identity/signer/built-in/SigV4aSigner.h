/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <smithy/identity/signer/AwsSignerBase.h>
#include <smithy/identity/identity/AwsCredentialIdentityBase.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/crt/auth/Credentials.h>

#include <aws/core/http/HttpRequest.h>
#include <aws/core/auth/signer/AWSAuthSignerHelper.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <condition_variable>
#include <mutex>

namespace smithy {
    static const char* UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";
    static const char* EMPTY_STRING_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char v4AsymmetricLogTag[] = "AWSAuthSymmetricV4Signer";
    static const char* USER_AGENT = "user-agent";
    static const char* X_AMZN_TRACE_ID = "x-amzn-trace-id";

    /**
     * A smithy SigV4 signer wrapper on top of legacy SDK SigV4 signer
     * TODO: refactor into own signer using smithy design
     */
    class AwsSigV4aSigner : public AwsSignerBase<AwsCredentialIdentityBase> {
        
    public:
        using SigV4aAuthSchemeParameters = smithy::DefaultAuthSchemeResolverParameters;
        explicit AwsSigV4aSigner(const Aws::String& serviceName, const Aws::String& region)
            :  m_serviceName(serviceName), m_region(region)
        {
        }

        SigningFutureOutcome sign(std::shared_ptr<HttpRequest> httpRequest, const AwsCredentialIdentityBase& identity, SigningProperties properties) override
        {
            
            auto signPayloadIt = properties.find("SignPayload");
            bool signPayload = signPayloadIt != properties.end() ? signPayloadIt->second.get<Aws::String>() == "true" : false;

            assert(httpRequest);
            assert(identity.expiration().has_value());

            auto &request = *httpRequest;
           
            auto crtCredentials = Aws::MakeShared<Aws::Crt::Auth::Credentials>(v4AsymmetricLogTag,
                Aws::Crt::ByteCursorFromCString(identity.accessKeyId().c_str()),
                Aws::Crt::ByteCursorFromCString(identity.secretAccessKey().c_str()),
                Aws::Crt::ByteCursorFromCString((*identity.sessionToken()).c_str()),
                (*identity.expiration()).Seconds());

            Aws::Crt::Auth::AwsSigningConfig awsSigningConfig;
            
            bool success = createAwsSigningConfig(crtCredentials, request, awsSigningConfig, signPayload);

            if(!success)
            {
                AWS_LOGSTREAM_ERROR(v4AsymmetricLogTag, "Failed to get Auth configuration");

                return SigningError(Aws::Client::CoreErrors::MEMORY_ALLOCATION, "", "Failed to get Auth configuration", false);
            }

            std::shared_ptr<Aws::Crt::Http::HttpRequest> crtHttpRequest = request.ToCrtHttpRequest();

            auto sigv4HttpRequestSigner = Aws::MakeShared<Aws::Crt::Auth::Sigv4HttpRequestSigner>(v4AsymmetricLogTag);
            //This is an async call, so we need to wait till we have received an outcome
            Aws::String errorMessage;
            bool processed = false;
            //producer function
            sigv4HttpRequestSigner->SignRequest(crtHttpRequest, awsSigningConfig,
                [&request, &success, &errorMessage, &processed, this](const std::shared_ptr<Aws::Crt::Http::HttpRequest>& signedCrtHttpRequest, int errorCode) {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [&]{ return !processed; });
                    success = (errorCode == AWS_ERROR_SUCCESS);
                    if (success)
                    {
                        if (m_signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaHeaders)
                        {
                            for (size_t i = 0; i < signedCrtHttpRequest->GetHeaderCount(); i++)
                            {
                                Aws::Crt::Optional<Aws::Crt::Http::HttpHeader> httpHeader = signedCrtHttpRequest->GetHeader(i);
                                request.SetHeaderValue(Aws::String(reinterpret_cast<const char*>(httpHeader->name.ptr), httpHeader->name.len),
                                    Aws::String(reinterpret_cast<const char*>(httpHeader->value.ptr), httpHeader->value.len));
                            }
                        }
                        else if (m_signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaQueryParams)
                        {
                            Aws::Http::URI newPath(reinterpret_cast<const char*>(signedCrtHttpRequest->GetPath()->ptr));
                            request.GetUri().SetQueryString(newPath.GetQueryString());
                        }
                        else
                        {
                            errorMessage = "No action to take when signature type is neither \"HttpRequestViaHeaders\" nor \"HttpRequestViaQueryParams\"";
                            AWS_LOGSTREAM_ERROR(v4AsymmetricLogTag, errorMessage);
                            success = false;
                        }
                    }
                    else
                    {
                        Aws::OStringStream errStream;
                        errStream << "Encountered internal error during signing process with AWS signature version 4 (Asymmetric):" << aws_error_str(errorCode);
                        errorMessage = errStream.str();
                        AWS_LOGSTREAM_ERROR(v4AsymmetricLogTag, errorMessage);
                    }

                    processed = true;
                    m_cv.notify_all();
                }
            );

            //consumer
            {       
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&]{ return processed; });

            }
            
            return success? SigningFutureOutcome(std::move(httpRequest)) : SigningError(Aws::Client::CoreErrors::MEMORY_ALLOCATION, "", "Failed to sign the request with sigv4", false);
        }



        virtual ~AwsSigV4aSigner() {};
    protected:

        bool createAwsSigningConfig(
            std::shared_ptr<Aws::Crt::Auth::Credentials>& crtCredentials,
            const Aws::Http::HttpRequest& request, 
            Aws::Crt::Auth::AwsSigningConfig& awsSigningConfig, 
            bool signBody) const
        {
            awsSigningConfig.SetSigningAlgorithm(static_cast<Aws::Crt::Auth::SigningAlgorithm>(Aws::Auth::AWSSigningAlgorithm::ASYMMETRIC_SIGV4));
            awsSigningConfig.SetSignatureType(m_signatureType);
            awsSigningConfig.SetRegion(m_region.c_str());
            awsSigningConfig.SetService(m_region.c_str());
            awsSigningConfig.SetSigningTimepoint(GetSigningTimestamp().UnderlyingTimestamp());
            awsSigningConfig.SetUseDoubleUriEncode(m_urlEscape);
            awsSigningConfig.SetShouldNormalizeUriPath(true);
            awsSigningConfig.SetOmitSessionToken(false);
            awsSigningConfig.SetShouldSignHeaderUserData(reinterpret_cast<void*>(const_cast<Aws::Set<Aws::String>*>(&m_unsignedHeaders)));
            awsSigningConfig.SetShouldSignHeaderCallback([](const Aws::Crt::ByteCursor *name, void *user_data) {
                Aws::Set<Aws::String>* unsignedHeaders = static_cast<Aws::Set<Aws::String>*>(user_data);
                Aws::String headerKey(reinterpret_cast<const char*>(name->ptr), name->len);
                return unsignedHeaders->find(Aws::Utils::StringUtils::ToLower(headerKey.c_str())) == unsignedHeaders->cend();
            });
            if (m_signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaHeaders)
            {
                Aws::String payloadHash(UNSIGNED_PAYLOAD);
                if(signBody || request.GetUri().GetScheme() != Aws::Http::Scheme::HTTPS)
                {
                    if (!request.GetContentBody())
                    {
                        AWS_LOGSTREAM_DEBUG(v4AsymmetricLogTag, "Using cached empty string sha256 " << EMPTY_STRING_SHA256 << " because payload is empty.");
                        payloadHash = EMPTY_STRING_SHA256;
                    }
                    else
                    {
                        // The hash will be calculated from the payload during signing.
                        payloadHash = {};
                    }
                }
                else
                {
                    AWS_LOGSTREAM_DEBUG(v4AsymmetricLogTag, "Note: Http payloads are not being signed. signPayloads=" << signBody
                            << " http scheme=" << Aws::Http::SchemeMapper::ToString(request.GetUri().GetScheme()));
                }
                awsSigningConfig.SetSignedBodyValue(payloadHash.c_str());
                awsSigningConfig.SetSignedBodyHeader(m_includeSha256HashHeader ? Aws::Crt::Auth::SignedBodyHeaderType::XAmzContentSha256 : Aws::Crt::Auth::SignedBodyHeaderType::None);
            }
            else if (m_signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaQueryParams)
            {
                if (ServiceRequireUnsignedPayload(m_serviceName))
                {
                    awsSigningConfig.SetSignedBodyValue(UNSIGNED_PAYLOAD);
                }
                else
                {
                    awsSigningConfig.SetSignedBodyValue(EMPTY_STRING_SHA256);
                }
            }
            else
            {
                AWS_LOGSTREAM_ERROR(v4AsymmetricLogTag, "The signature type should be either \"HttpRequestViaHeaders\" or \"HttpRequestViaQueryParams\"");
                return false;
            }
            awsSigningConfig.SetExpirationInSeconds(static_cast<uint64_t>(m_expirationTimeInSeconds));
            awsSigningConfig.SetCredentials(crtCredentials);
            return true;
        }


        bool ServiceRequireUnsignedPayload(const Aws::String& serviceName) const
        {
            // S3 uses a magic string (instead of the empty string) for its body hash for presigned URLs as outlined here:
            // https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-query-string-auth.html
            // this is true for PUT, POST, GET, DELETE and HEAD operations.
            // However, other services (for example RDS) implement the specification as outlined here:
            // https://docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html
            // which states that body-less requests should use the empty-string SHA256 hash.
            return "s3" == serviceName || "s3-object-lambda" == serviceName;
        }

        Aws::String m_serviceName;
        Aws::String m_region;
        //params that can be exposed later
        long long m_expirationTimeInSeconds{0};
        const bool m_includeSha256HashHeader{true};
        const bool m_urlEscape{true};
        const Aws::Set<Aws::String> m_unsignedHeaders{USER_AGENT,X_AMZN_TRACE_ID};
        const Aws::Crt::Auth::SignatureType m_signatureType{Aws::Crt::Auth::SignatureType::HttpRequestViaQueryParams};
        std::condition_variable m_cv;
        std::mutex m_mutex;
    };
}
