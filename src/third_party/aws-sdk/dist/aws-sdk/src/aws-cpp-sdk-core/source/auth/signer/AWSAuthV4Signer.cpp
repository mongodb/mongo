/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/signer/AWSAuthV4Signer.h>
#include <aws/core/auth/signer/AWSAuthSignerCommon.h>
#include <aws/core/auth/signer/AWSAuthSignerHelper.h>

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha256HMAC.h>
#include <aws/core/endpoint/internal/AWSEndpointAttribute.h>

#include <aws/crt/auth/Credentials.h>
#include <aws/crt/http/HttpRequestResponse.h>

#include <iomanip>
#include <cstring>

using namespace Aws;
using namespace Aws::Client;
using namespace Aws::Auth;
using namespace Aws::Http;
using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

static const char* X_AMZ_SIGNED_HEADERS = "X-Amz-SignedHeaders";
static const char* X_AMZ_ALGORITHM = "X-Amz-Algorithm";
static const char* X_AMZ_CREDENTIAL = "X-Amz-Credential";
static const char* UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";
static const char* STREAMING_UNSIGNED_PAYLOAD_TRAILER = "STREAMING-UNSIGNED-PAYLOAD-TRAILER";
static const char* X_AMZ_SIGNATURE = "X-Amz-Signature";
static const char* USER_AGENT = "user-agent";
static const char* EMPTY_STRING_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

static const char v4LogTag[] = "AWSAuthV4Signer";
static const char v4AsymmetricLogTag[] = "AWSAuthSymmetricV4Signer";

namespace Aws
{
    namespace Auth
    {
        const char SIGV4_SIGNER[] = "SignatureV4";
        const char ASYMMETRIC_SIGV4_SIGNER[] = "AsymmetricSignatureV4";
    }
}

AWSAuthV4Signer::AWSAuthV4Signer(const std::shared_ptr<Auth::AWSCredentialsProvider>& credentialsProvider, const char* serviceName,
    const Aws::String& region, PayloadSigningPolicy signingPolicy, bool urlEscapePath, AWSSigningAlgorithm signingAlgorithm) :
    m_includeSha256HashHeader(true),
    m_signingAlgorithm(signingAlgorithm),
    m_credentialsProvider(credentialsProvider),
    m_serviceName(serviceName),
    m_region(region),
    m_unsignedHeaders({USER_AGENT, Aws::Auth::AWSAuthHelper::X_AMZN_TRACE_ID}),
    m_payloadSigningPolicy(signingPolicy),
    m_urlEscapePath(urlEscapePath)
{
    //go ahead and warm up the signing cache.
    if (credentialsProvider)
    {
        ComputeHash(credentialsProvider->GetAWSCredentials().GetAWSSecretKey(), DateTime::CalculateGmtTimestampAsString(Aws::Auth::AWSAuthHelper::SIMPLE_DATE_FORMAT_STR), region, m_serviceName);
    }
}

AWSAuthV4Signer::~AWSAuthV4Signer()
{
    // empty destructor in .cpp file to keep from needing the implementation of (AWSCredentialsProvider, Sha256, Sha256HMAC) in the header file
}

bool AWSAuthV4Signer::SignRequestWithSigV4a(Aws::Http::HttpRequest& request, const char* region, const char* serviceName,
    bool signBody, long long expirationTimeInSeconds, Aws::Crt::Auth::SignatureType signatureType) const
{
    AWSCredentials credentials = GetCredentials(request.GetServiceSpecificParameters());
    auto crtCredentials = Aws::MakeShared<Aws::Crt::Auth::Credentials>(v4AsymmetricLogTag,
        Aws::Crt::ByteCursorFromCString(credentials.GetAWSAccessKeyId().c_str()),
        Aws::Crt::ByteCursorFromCString(credentials.GetAWSSecretKey().c_str()),
        Aws::Crt::ByteCursorFromCString(credentials.GetSessionToken().c_str()),
        credentials.GetExpiration().Seconds());

    Aws::Crt::Auth::AwsSigningConfig awsSigningConfig;
    awsSigningConfig.SetSigningAlgorithm(static_cast<Aws::Crt::Auth::SigningAlgorithm>(AWSSigningAlgorithm::ASYMMETRIC_SIGV4));
    awsSigningConfig.SetSignatureType(signatureType);
    awsSigningConfig.SetRegion(region);
    awsSigningConfig.SetService(serviceName);
    awsSigningConfig.SetSigningTimepoint(GetSigningTimestamp().UnderlyingTimestamp());
    awsSigningConfig.SetUseDoubleUriEncode(m_urlEscapePath);
    awsSigningConfig.SetShouldNormalizeUriPath(true);
    awsSigningConfig.SetOmitSessionToken(false);
    awsSigningConfig.SetShouldSignHeaderUserData(reinterpret_cast<void*>(const_cast<Aws::Set<Aws::String>*>(&m_unsignedHeaders)));
    awsSigningConfig.SetShouldSignHeaderCallback([](const Aws::Crt::ByteCursor *name, void *user_data) {
        Aws::Set<Aws::String>* unsignedHeaders = static_cast<Aws::Set<Aws::String>*>(user_data);
        Aws::String headerKey(reinterpret_cast<const char*>(name->ptr), name->len);
        return unsignedHeaders->find(Aws::Utils::StringUtils::ToLower(headerKey.c_str())) == unsignedHeaders->cend();
    });
    if (signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaHeaders)
    {
        Aws::String payloadHash(UNSIGNED_PAYLOAD);
        if(signBody || request.GetUri().GetScheme() != Http::Scheme::HTTPS)
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
                    << " http scheme=" << Http::SchemeMapper::ToString(request.GetUri().GetScheme()));
        }
        awsSigningConfig.SetSignedBodyValue(payloadHash.c_str());
        awsSigningConfig.SetSignedBodyHeader(m_includeSha256HashHeader ? Aws::Crt::Auth::SignedBodyHeaderType::XAmzContentSha256 : Aws::Crt::Auth::SignedBodyHeaderType::None);
    }
    else if (signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaQueryParams)
    {
        if (ServiceRequireUnsignedPayload(serviceName))
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
    awsSigningConfig.SetExpirationInSeconds(static_cast<uint64_t>(expirationTimeInSeconds));
    awsSigningConfig.SetCredentials(crtCredentials);

    std::shared_ptr<Aws::Crt::Http::HttpRequest> crtHttpRequest = request.ToCrtHttpRequest();
    bool success = true;
    m_crtSigner.SignRequest(crtHttpRequest, awsSigningConfig,
        [&request, &success, signatureType](const std::shared_ptr<Aws::Crt::Http::HttpRequest>& signedCrtHttpRequest, int errorCode) {
            success = (errorCode == AWS_ERROR_SUCCESS);
            if (success)
            {
                if (signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaHeaders)
                {
                    for (size_t i = 0; i < signedCrtHttpRequest->GetHeaderCount(); i++)
                    {
                        Aws::Crt::Optional<Aws::Crt::Http::HttpHeader> httpHeader = signedCrtHttpRequest->GetHeader(i);
                        request.SetHeaderValue(Aws::String(reinterpret_cast<const char*>(httpHeader->name.ptr), httpHeader->name.len),
                            Aws::String(reinterpret_cast<const char*>(httpHeader->value.ptr), httpHeader->value.len));
                    }
                }
                else if (signatureType == Aws::Crt::Auth::SignatureType::HttpRequestViaQueryParams)
                {
                    Aws::Http::URI newPath(reinterpret_cast<const char*>(signedCrtHttpRequest->GetPath()->ptr));
                    request.GetUri().SetQueryString(newPath.GetQueryString());
                }
                else
                {
                    AWS_LOGSTREAM_ERROR(v4AsymmetricLogTag, "No action to take when signature type is neither \"HttpRequestViaHeaders\" nor \"HttpRequestViaQueryParams\"");
                    success = false;
                }
            }
            else
            {
                AWS_LOGSTREAM_ERROR(v4AsymmetricLogTag, "Encountered internal error during signing process with AWS signature version 4 (Asymmetric):" << aws_error_str(errorCode));
            }
        }
    );
    return success;
}

bool AWSAuthV4Signer::ShouldSignHeader(const Aws::String& header) const
{
    return m_unsignedHeaders.find(Aws::Utils::StringUtils::ToLower(header.c_str())) == m_unsignedHeaders.cend();
}

bool AWSAuthV4Signer::SignRequestWithCreds(Aws::Http::HttpRequest& request, const AWSCredentials& credentials,
                                           const char* region, const char* serviceName, bool signBody) const
{
    Aws::String signingRegion = region ? region : m_region;
    Aws::String signingServiceName = serviceName ? serviceName : m_serviceName;

    //don't sign anonymous requests
    if (credentials.GetAWSAccessKeyId().empty() || credentials.GetAWSSecretKey().empty())
    {
        return true;
    }

    request.SetSigningAccessKey(credentials.GetAWSAccessKeyId());
    request.SetSigningRegion(signingRegion);

    Aws::String payloadHash(UNSIGNED_PAYLOAD);
    switch(m_payloadSigningPolicy)
    {
        case PayloadSigningPolicy::Always:
            signBody = true;
            break;
        case PayloadSigningPolicy::Never:
            signBody = false;
            break;
        case PayloadSigningPolicy::RequestDependent:
            // respect the request setting
        default:
            break;
    }

    if (m_signingAlgorithm == AWSSigningAlgorithm::ASYMMETRIC_SIGV4)
    {
        return SignRequestWithSigV4a(request, signingRegion.c_str(), signingServiceName.c_str(), signBody,
            0 /* expirationTimeInSeconds doesn't matter for HttpRequestViaHeaders */, Aws::Crt::Auth::SignatureType::HttpRequestViaHeaders);
    }

    if (!credentials.GetSessionToken().empty())
    {
        request.SetAwsSessionToken(credentials.GetSessionToken());
    }

    if(signBody || request.GetUri().GetScheme() != Http::Scheme::HTTPS)
    {
        payloadHash = ComputePayloadHash(request);
        if (payloadHash.empty())
        {
            // this indicates a hashing error occurred, which was logged
            return false;
        }

        Aws::String checksumHeaderKey = Aws::String("x-amz-checksum-") + request.GetRequestHash().first;
        const auto headers = request.GetHeaders();
        if (request.GetRequestHash().second != nullptr && !request.HasHeader(checksumHeaderKey.c_str()))
        {
            Aws::String checksumHeaderValue;
            if (request.GetRequestHash().first == "sha256") {
                // we already calculated the payload hash so just reverse the hex string to
                // a ByteBuffer and Base64Encode it - otherwise we're re-hashing the content
                checksumHeaderValue = HashingUtils::Base64Encode(HashingUtils::HexDecode(payloadHash));
            } else {
                // if it is one of the other hashes, we must be careful if there is no content body
                const auto& body = request.GetContentBody();
                checksumHeaderValue = (body)
                    ? HashingUtils::Base64Encode(request.GetRequestHash().second->Calculate(*body).GetResult())
                    : HashingUtils::Base64Encode(request.GetRequestHash().second->Calculate({}).GetResult());
            }
            request.SetHeaderValue(checksumHeaderKey, checksumHeaderValue);
            request.SetRequestHash("", nullptr);
        }
    }
    else
    {
        AWS_LOGSTREAM_DEBUG(v4LogTag, "Note: Http payloads are not being signed. signPayloads=" << signBody
                << " http scheme=" << Http::SchemeMapper::ToString(request.GetUri().GetScheme()));
        if (request.GetRequestHash().second != nullptr)
        {
            payloadHash = STREAMING_UNSIGNED_PAYLOAD_TRAILER;
            Aws::String checksumHeaderValue = Aws::String("x-amz-checksum-") + request.GetRequestHash().first;
            request.DeleteHeader(checksumHeaderValue.c_str());
            request.SetHeaderValue(Http::AWS_TRAILER_HEADER, checksumHeaderValue);
            request.SetTransferEncoding(CHUNKED_VALUE);
            request.HasContentEncoding()
                ? request.SetContentEncoding(Aws::String{Http::AWS_CHUNKED_VALUE} + "," + request.GetContentEncoding())
                : request.SetContentEncoding(Http::AWS_CHUNKED_VALUE);

            if (request.HasHeader(Http::CONTENT_LENGTH_HEADER)) {
                request.SetHeaderValue(Http::DECODED_CONTENT_LENGTH_HEADER, request.GetHeaderValue(Http::CONTENT_LENGTH_HEADER));
                request.DeleteHeader(Http::CONTENT_LENGTH_HEADER);
            }
        }
    }

    if(m_includeSha256HashHeader)
    {
        request.SetHeaderValue(Aws::Auth::AWSAuthHelper::X_AMZ_CONTENT_SHA256, payloadHash);
    }

    //calculate date header to use in internal signature (this also goes into date header).
    DateTime now = GetSigningTimestamp();
    Aws::String dateHeaderValue = now.ToGmtString(DateFormat::ISO_8601_BASIC);
    request.SetHeaderValue(AWS_DATE_HEADER, dateHeaderValue);

    Aws::StringStream headersStream;
    Aws::StringStream signedHeadersStream;

    for (const auto& header : Aws::Auth::AWSAuthHelper::CanonicalizeHeaders(request.GetHeaders()))
    {
        if(ShouldSignHeader(header.first))
        {
            headersStream << header.first.c_str() << ":" << header.second.c_str() << Aws::Auth::AWSAuthHelper::NEWLINE;
            signedHeadersStream << header.first.c_str() << ";";
        }
    }

    Aws::String canonicalHeadersString = headersStream.str();
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Canonical Header String: " << canonicalHeadersString);

    //calculate signed headers parameter
    Aws::String signedHeadersValue = signedHeadersStream.str();
    //remove that last semi-colon
    if (!signedHeadersValue.empty())
    {
        signedHeadersValue.pop_back();
    }

    AWS_LOGSTREAM_DEBUG(v4LogTag, "Signed Headers value:" << signedHeadersValue);

    //generate generalized canonicalized request string.
    Aws::String canonicalRequestString = Aws::Auth::AWSAuthHelper::CanonicalizeRequestSigningString(request, m_urlEscapePath);

    //append v4 stuff to the canonical request string.
    canonicalRequestString.append(canonicalHeadersString);
    canonicalRequestString.append(Aws::Auth::AWSAuthHelper::NEWLINE);
    canonicalRequestString.append(signedHeadersValue);
    canonicalRequestString.append(Aws::Auth::AWSAuthHelper::NEWLINE);
    canonicalRequestString.append(payloadHash);

    AWS_LOGSTREAM_DEBUG(v4LogTag, "Canonical Request String: " << canonicalRequestString);

    //now compute sha256 on that request string
    auto sha256Digest = HashingUtils::CalculateSHA256(canonicalRequestString);
    if (sha256Digest.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Failed to hash (sha256) request string");
        AWS_LOGSTREAM_DEBUG(v4LogTag, "The request string is: \"" << canonicalRequestString << "\"");
        return false;
    }

    Aws::String canonicalRequestHash = HashingUtils::HexEncode(sha256Digest);
    Aws::String simpleDate = now.ToGmtString(Aws::Auth::AWSAuthHelper::SIMPLE_DATE_FORMAT_STR);

    Aws::String stringToSign = GenerateStringToSign(dateHeaderValue, simpleDate, canonicalRequestHash, signingRegion, signingServiceName);
    auto finalSignature = GenerateSignature(credentials, stringToSign, simpleDate, signingRegion, signingServiceName);

    Aws::StringStream ss;
    ss << Aws::Auth::AWSAuthHelper::AWS_HMAC_SHA256 << " " << Aws::Auth::AWSAuthHelper::CREDENTIAL << Aws::Auth::AWSAuthHelper::EQ << credentials.GetAWSAccessKeyId() << "/" << simpleDate
        << "/" << signingRegion << "/" << signingServiceName << "/" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST << ", " << Aws::Auth::AWSAuthHelper::SIGNED_HEADERS << Aws::Auth::AWSAuthHelper::EQ
        << signedHeadersValue << ", " << SIGNATURE << Aws::Auth::AWSAuthHelper::EQ << finalSignature;

    auto awsAuthString = ss.str();
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Signing request with: " << awsAuthString);
    request.SetAwsAuthorization(awsAuthString);
    return true;
}

bool AWSAuthV4Signer::SignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, bool signBody) const
{
    AWSCredentials credentials = GetCredentials(request.GetServiceSpecificParameters());
    return SignRequestWithCreds(request, credentials, region, serviceName, signBody);
}

bool AWSAuthV4Signer::PresignRequest(Aws::Http::HttpRequest& request, long long expirationTimeInSeconds) const
{
    return PresignRequest(request, m_region.c_str(), expirationTimeInSeconds);
}

bool AWSAuthV4Signer::PresignRequest(Aws::Http::HttpRequest& request, const char* region, long long expirationInSeconds) const
{
    return PresignRequest(request, region, m_serviceName.c_str(), expirationInSeconds);
}

bool AWSAuthV4Signer::PresignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, long long expirationTimeInSeconds) const
{
    Aws::String signingRegion = region ? region : m_region;
    Aws::String signingServiceName = serviceName ? serviceName : m_serviceName;
    AWSCredentials credentials = GetCredentials(request.GetServiceSpecificParameters());

    //don't sign anonymous requests
    if (credentials.GetAWSAccessKeyId().empty() || credentials.GetAWSSecretKey().empty())
    {
        return true;
    }

    if (m_signingAlgorithm == AWSSigningAlgorithm::ASYMMETRIC_SIGV4)
    {
        return SignRequestWithSigV4a(request, signingRegion.c_str(), signingServiceName.c_str(), false /* signBody doesn't matter for HttpRequestViaHeaders */,
            expirationTimeInSeconds, Aws::Crt::Auth::SignatureType::HttpRequestViaQueryParams);
    }

    Aws::StringStream intConversionStream;
    intConversionStream << expirationTimeInSeconds;
    request.AddQueryStringParameter(Http::X_AMZ_EXPIRES_HEADER, intConversionStream.str());

    if (!credentials.GetSessionToken().empty())
    {
        request.AddQueryStringParameter(Http::AWS_SECURITY_TOKEN, credentials.GetSessionToken());
    }

    //calculate date header to use in internal signature (this also goes into date header).
    DateTime now = GetSigningTimestamp();
    Aws::String dateQueryValue = now.ToGmtString(DateFormat::ISO_8601_BASIC);
    request.AddQueryStringParameter(Http::AWS_DATE_HEADER, dateQueryValue);

    Aws::StringStream headersStream;
    Aws::StringStream signedHeadersStream;
    for (const auto& header : Aws::Auth::AWSAuthHelper::CanonicalizeHeaders(request.GetHeaders()))
    {
        if(ShouldSignHeader(header.first))
        {
            headersStream << header.first.c_str() << ":" << header.second.c_str() << Aws::Auth::AWSAuthHelper::NEWLINE;
            signedHeadersStream << header.first.c_str() << ";";
        }
    }

    Aws::String canonicalHeadersString = headersStream.str();
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Canonical Header String: " << canonicalHeadersString);

    //calculate signed headers parameter
    Aws::String signedHeadersValue(signedHeadersStream.str());
    //remove that last semi-colon
    if (!signedHeadersValue.empty())
    {
        signedHeadersValue.pop_back();
    }

    request.AddQueryStringParameter(X_AMZ_SIGNED_HEADERS, signedHeadersValue);
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Signed Headers value: " << signedHeadersValue);

    Aws::StringStream ss;
    Aws::String simpleDate = now.ToGmtString(Aws::Auth::AWSAuthHelper::SIMPLE_DATE_FORMAT_STR);
    ss << credentials.GetAWSAccessKeyId() << "/" << simpleDate
        << "/" << signingRegion << "/" << signingServiceName << "/" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST;

    request.AddQueryStringParameter(X_AMZ_ALGORITHM, Aws::Auth::AWSAuthHelper::AWS_HMAC_SHA256);
    request.AddQueryStringParameter(X_AMZ_CREDENTIAL, ss.str());
    ss.str("");

    request.SetSigningAccessKey(credentials.GetAWSAccessKeyId());
    request.SetSigningRegion(signingRegion);

    //generate generalized canonicalized request string.
    Aws::String canonicalRequestString = Aws::Auth::AWSAuthHelper::CanonicalizeRequestSigningString(request, m_urlEscapePath);

    //append v4 stuff to the canonical request string.
    canonicalRequestString.append(canonicalHeadersString);
    canonicalRequestString.append(Aws::Auth::AWSAuthHelper::NEWLINE);
    canonicalRequestString.append(signedHeadersValue);
    canonicalRequestString.append(Aws::Auth::AWSAuthHelper::NEWLINE);
    if (ServiceRequireUnsignedPayload(signingServiceName))
    {
        canonicalRequestString.append(UNSIGNED_PAYLOAD);
    }
    else
    {
        canonicalRequestString.append(EMPTY_STRING_SHA256);
    }
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Canonical Request String: " << canonicalRequestString);

    //now compute sha256 on that request string
    auto sha256Digest = HashingUtils::CalculateSHA256(canonicalRequestString);
    if (sha256Digest.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Failed to hash (sha256) request string");
        AWS_LOGSTREAM_DEBUG(v4LogTag, "The request string is: \"" << canonicalRequestString << "\"");
        return false;
    }

    auto canonicalRequestHash = HashingUtils::HexEncode(sha256Digest);

    auto stringToSign = GenerateStringToSign(dateQueryValue, simpleDate, canonicalRequestHash, signingRegion, signingServiceName);
    auto finalSigningHash = GenerateSignature(credentials, stringToSign, simpleDate, signingRegion, signingServiceName);
    if (finalSigningHash.empty())
    {
        return false;
    }

    //add that the signature to the query string
    request.AddQueryStringParameter(X_AMZ_SIGNATURE, finalSigningHash);

    return true;
}

bool AWSAuthV4Signer::ServiceRequireUnsignedPayload(const Aws::String& serviceName) const
{
    // S3 uses a magic string (instead of the empty string) for its body hash for presigned URLs as outlined here:
    // https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-query-string-auth.html
    // this is true for PUT, POST, GET, DELETE and HEAD operations.
    // However, other services (for example RDS) implement the specification as outlined here:
    // https://docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html
    // which states that body-less requests should use the empty-string SHA256 hash.
    return "s3" == serviceName || "s3-object-lambda" == serviceName;
}

Aws::String AWSAuthV4Signer::GenerateSignature(const AWSCredentials &credentials,
    const String &stringToSign,
    const String &simpleDate) const
{
    return GenerateSignature(credentials,
        stringToSign,
        simpleDate,
        GetRegion(),
        GetServiceName());
}

Aws::String AWSAuthV4Signer::GenerateSignature(const AWSCredentials& credentials, const Aws::String& stringToSign,
        const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const
{
    auto key = ComputeHash(credentials.GetAWSSecretKey(), simpleDate, region, serviceName);
    return GenerateSignature(stringToSign, key);
}

Aws::String AWSAuthV4Signer::GenerateSignature(const Aws::String& stringToSign, const ByteBuffer& key) const
{
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Final String to sign: " << stringToSign);

    Aws::StringStream ss;

    //now we finally sign our request string with our hex encoded derived hash.
    auto finalSigningDigest = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)stringToSign.c_str(), stringToSign.length()), key);
    if (finalSigningDigest.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Unable to hmac (sha256) final string");
        AWS_LOGSTREAM_DEBUG(v4LogTag, "The final string is: \"" << stringToSign << "\"");
        return {};
    }

    auto finalSigningHash = HashingUtils::HexEncode(finalSigningDigest);
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Final computed signing hash: " << finalSigningHash);

    return finalSigningHash;
}

Aws::String AWSAuthV4Signer::ComputePayloadHash(Aws::Http::HttpRequest& request) const
{
    const std::shared_ptr<Aws::IOStream>& body = request.GetContentBody();
    if (!body)
    {
        AWS_LOGSTREAM_DEBUG(v4LogTag, "Using cached empty string sha256 " << EMPTY_STRING_SHA256 << " because payload is empty.");
        return EMPTY_STRING_SHA256;
    }

    // compute hash on payload if it exists
    auto sha256Digest =  HashingUtils::CalculateSHA256(*request.GetContentBody());
    body->clear();      // clears ios_flags
    body->seekg(0);

    if (sha256Digest.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Unable to hash (sha256) request body");
        return {};
    }

    Aws::String payloadHash(HashingUtils::HexEncode(sha256Digest));
    AWS_LOGSTREAM_DEBUG(v4LogTag, "Calculated sha256 " << payloadHash << " for payload.");
    return payloadHash;
}

Aws::String AWSAuthV4Signer::GenerateStringToSign(const Aws::String& dateValue, const Aws::String& simpleDate,
        const Aws::String& canonicalRequestHash, const Aws::String& region, const Aws::String& serviceName) const
{
    //generate the actual string we will use in signing the final request.
    Aws::StringStream ss;

    ss << Aws::Auth::AWSAuthHelper::AWS_HMAC_SHA256 << Aws::Auth::AWSAuthHelper::NEWLINE << dateValue << Aws::Auth::AWSAuthHelper::NEWLINE << simpleDate << "/" << region << "/"
        << serviceName << "/" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST << Aws::Auth::AWSAuthHelper::NEWLINE << canonicalRequestHash;

    return ss.str();
}

Aws::Utils::ByteBuffer AWSAuthV4Signer::ComputeHash(const Aws::String& secretKey,
        const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const
{
    Aws::String signingKey(Aws::Auth::AWSAuthHelper::SIGNING_KEY);
    signingKey.append(secretKey);
    auto kDate = HashingUtils::CalculateSHA256HMAC(simpleDate, signingKey);

    if (kDate.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Failed to HMAC (SHA256) date string \"" << simpleDate << "\"");
        return {};
    }

    auto kRegion = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)region.c_str(), region.length()), kDate);
    if (kRegion.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Failed to HMAC (SHA256) region string \"" << region << "\"");
        return {};
    }

    auto kService = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)serviceName.c_str(), serviceName.length()), kRegion);
    if (kService.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Failed to HMAC (SHA256) service string \"" << m_serviceName << "\"");
        return {};
    }

    auto kFinalHash = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)Aws::Auth::AWSAuthHelper::AWS4_REQUEST, strlen(Aws::Auth::AWSAuthHelper::AWS4_REQUEST)), kService);
    if (kFinalHash.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4LogTag, "Unable to HMAC (SHA256) request string");
        AWS_LOGSTREAM_DEBUG(v4LogTag, "The request string is: \"" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST << "\"");
        return {};
    }
    return kFinalHash;
}

Aws::Auth::AWSCredentials AWSAuthV4Signer::GetCredentials(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) const {
    AWS_UNREFERENCED_PARAM(serviceSpecificParameters);
    return m_credentialsProvider->GetAWSCredentials();
}
