/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/signer/AWSAuthEventStreamV4Signer.h>
#include <aws/core/auth/signer/AWSAuthSignerCommon.h>
#include <aws/core/auth/signer/AWSAuthSignerHelper.h>

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/crypto/Sha256HMAC.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/core/utils/event/EventMessage.h>
#include <aws/core/utils/event/EventHeader.h>

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

static const char* EVENT_STREAM_CONTENT_SHA256 = "STREAMING-AWS4-HMAC-SHA256-EVENTS";
static const char* EVENT_STREAM_PAYLOAD = "AWS4-HMAC-SHA256-PAYLOAD";
static const char* v4StreamingLogTag = "AWSAuthEventStreamV4Signer";

namespace Aws
{
    namespace Auth
    {
        const char EVENTSTREAM_SIGV4_SIGNER[] = "EventStreamSignatureV4";
        const char EVENTSTREAM_SIGNATURE_HEADER[] = ":chunk-signature";
        const char EVENTSTREAM_DATE_HEADER[] = ":date";
    }
}

AWSAuthEventStreamV4Signer::AWSAuthEventStreamV4Signer(const std::shared_ptr<Auth::AWSCredentialsProvider>&
        credentialsProvider, const char* serviceName, const Aws::String& region) :
    m_serviceName(serviceName),
    m_region(region),
    m_credentialsProvider(credentialsProvider)
{

    m_unsignedHeaders.emplace_back(Aws::Auth::AWSAuthHelper::X_AMZN_TRACE_ID);
    m_unsignedHeaders.emplace_back(USER_AGENT_HEADER);
}

bool AWSAuthEventStreamV4Signer::SignRequest(Aws::Http::HttpRequest& request, const char* region, const char* serviceName, bool /* signBody */) const
{
    AWSCredentials credentials = m_credentialsProvider->GetAWSCredentials();

    //don't sign anonymous requests
    if (credentials.GetAWSAccessKeyId().empty() || credentials.GetAWSSecretKey().empty())
    {
        return true;
    }

    if (!credentials.GetSessionToken().empty())
    {
        request.SetAwsSessionToken(credentials.GetSessionToken());
    }

    request.SetHeaderValue(Aws::Auth::AWSAuthHelper::X_AMZ_CONTENT_SHA256, EVENT_STREAM_CONTENT_SHA256);

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
    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Canonical Header String: " << canonicalHeadersString);

    //calculate signed headers parameter
    Aws::String signedHeadersValue = signedHeadersStream.str();
    //remove that last semi-colon
    if (!signedHeadersValue.empty())
    {
        signedHeadersValue.pop_back();
    }

    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Signed Headers value:" << signedHeadersValue);

    //generate generalized canonicalized request string.
    Aws::String canonicalRequestString = Aws::Auth::AWSAuthHelper::CanonicalizeRequestSigningString(request, true/* m_urlEscapePath */);

    //append v4 stuff to the canonical request string.
    canonicalRequestString.append(canonicalHeadersString);
    canonicalRequestString.append(Aws::Auth::AWSAuthHelper::NEWLINE);
    canonicalRequestString.append(signedHeadersValue);
    canonicalRequestString.append(Aws::Auth::AWSAuthHelper::NEWLINE);
    canonicalRequestString.append(EVENT_STREAM_CONTENT_SHA256);

    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Canonical Request String: " << canonicalRequestString);

    //now compute sha256 on that request string
    auto sha256Digest = HashingUtils::CalculateSHA256(canonicalRequestString);
    if (sha256Digest.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Failed to hash (sha256) request string");
        AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "The request string is: \"" << canonicalRequestString << "\"");
        return false;
    }

    Aws::String canonicalRequestHash = HashingUtils::HexEncode(sha256Digest);
    Aws::String simpleDate = now.ToGmtString(Aws::Auth::AWSAuthHelper::SIMPLE_DATE_FORMAT_STR);

    Aws::String signingRegion = region ? region : m_region;
    Aws::String signingServiceName = serviceName ? serviceName : m_serviceName;
    Aws::String stringToSign = GenerateStringToSign(dateHeaderValue, simpleDate, canonicalRequestHash, signingRegion, signingServiceName);
    auto finalSignature = GenerateSignature(credentials, stringToSign, simpleDate, signingRegion, signingServiceName);

    Aws::StringStream ss;
    ss << Aws::Auth::AWSAuthHelper::AWS_HMAC_SHA256 << " " << Aws::Auth::AWSAuthHelper::CREDENTIAL << Aws::Auth::AWSAuthHelper::EQ << credentials.GetAWSAccessKeyId() << "/" << simpleDate
        << "/" << signingRegion << "/" << signingServiceName << "/" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST << ", " << Aws::Auth::AWSAuthHelper::SIGNED_HEADERS << Aws::Auth::AWSAuthHelper::EQ
        << signedHeadersValue << ", " << SIGNATURE << Aws::Auth::AWSAuthHelper::EQ << HashingUtils::HexEncode(finalSignature);

    auto awsAuthString = ss.str();
    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Signing request with: " << awsAuthString);
    request.SetAwsAuthorization(awsAuthString);
    request.SetSigningAccessKey(credentials.GetAWSAccessKeyId());
    request.SetSigningRegion(signingRegion);
    return true;
}

// this works regardless if the current machine is Big/Little Endian
static void WriteBigEndian(Aws::String& str, uint64_t n)
{
    int shift = 56;
    while(shift >= 0)
    {
        str.push_back((n >> shift) & 0xFF);
        shift -= 8;
    }
}

bool AWSAuthEventStreamV4Signer::SignEventMessage(Event::Message& message, Aws::String& priorSignature) const
{
    using Event::EventHeaderValue;

    Aws::StringStream stringToSign;
    stringToSign << EVENT_STREAM_PAYLOAD << Aws::Auth::AWSAuthHelper::NEWLINE;
    const DateTime now = GetSigningTimestamp();
    const auto simpleDate = now.ToGmtString(Aws::Auth::AWSAuthHelper::SIMPLE_DATE_FORMAT_STR);
    stringToSign << now.ToGmtString(DateFormat::ISO_8601_BASIC) << Aws::Auth::AWSAuthHelper::NEWLINE
        <<  simpleDate << "/" << m_region << "/"
        << m_serviceName << "/aws4_request" << Aws::Auth::AWSAuthHelper::NEWLINE << priorSignature << Aws::Auth::AWSAuthHelper::NEWLINE;


    Aws::String nonSignatureHeaders;
    nonSignatureHeaders.push_back(char(sizeof(EVENTSTREAM_DATE_HEADER) - 1)); // length of the string
    nonSignatureHeaders += EVENTSTREAM_DATE_HEADER;
    nonSignatureHeaders.push_back(static_cast<char>(EventHeaderValue::EventHeaderType::TIMESTAMP)); // type of the value
    WriteBigEndian(nonSignatureHeaders, static_cast<uint64_t>(now.Millis())); // the value of the timestamp in big-endian

    auto nonSignatureHeadersHash = HashingUtils::CalculateSHA256(nonSignatureHeaders);
    if (nonSignatureHeadersHash.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Failed to hash (sha256) non-signature headers.");
        return false;
    }

    stringToSign << HashingUtils::HexEncode(nonSignatureHeadersHash) << Aws::Auth::AWSAuthHelper::NEWLINE;

    ByteBuffer payloadHash;
    if (!message.GetEventPayload().empty())
    {
        // use a preallocatedStreamBuf to avoid making a copy.
        // The Hashing API requires either Aws::String or IStream as input.
        // TODO: the hashing API should be accept 'unsigned char*' as input.
        Utils::Stream::PreallocatedStreamBuf streamBuf(message.GetEventPayload().data(), message.GetEventPayload().size());
        Aws::IOStream payload(&streamBuf);
        payloadHash = HashingUtils::CalculateSHA256(payload);
    }
    else
    {
        // only a signature and a date will be in a frame
        AWS_LOGSTREAM_INFO(v4StreamingLogTag, "Signing an event with an empty payload");

        payloadHash = HashingUtils::CalculateSHA256(""); // SHA256 of an empty buffer
    }

    if (payloadHash.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Failed to hash (sha256) non-signature headers.");
        return false;
    }
    stringToSign << HashingUtils::HexEncode(payloadHash);
    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Payload hash  - " << HashingUtils::HexEncode(payloadHash));

    Aws::String canonicalRequestString = stringToSign.str();
    AWS_LOGSTREAM_TRACE(v4StreamingLogTag, "EventStream Event Canonical Request String: " << canonicalRequestString);
    Aws::Utils::ByteBuffer finalSignatureDigest = GenerateSignature(m_credentialsProvider->GetAWSCredentials(), canonicalRequestString, simpleDate, m_region, m_serviceName);
    const auto finalSignature = HashingUtils::HexEncode(finalSignatureDigest);
    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Final computed signing hash: " << finalSignature);
    priorSignature = finalSignature;

    message.InsertEventHeader(EVENTSTREAM_DATE_HEADER, EventHeaderValue(now.Millis(), EventHeaderValue::EventHeaderType::TIMESTAMP));
    message.InsertEventHeader(EVENTSTREAM_SIGNATURE_HEADER, std::move(finalSignatureDigest));

    AWS_LOGSTREAM_INFO(v4StreamingLogTag, "Event chunk final signature - " << finalSignature);
    return true;
}

bool AWSAuthEventStreamV4Signer::ShouldSignHeader(const Aws::String& header) const
{
    return std::find(m_unsignedHeaders.cbegin(), m_unsignedHeaders.cend(), Aws::Utils::StringUtils::ToLower(header.c_str())) == m_unsignedHeaders.cend();
}

Aws::Utils::ByteBuffer AWSAuthEventStreamV4Signer::GenerateSignature(const AWSCredentials& credentials, const Aws::String& stringToSign,
        const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const
{
    Utils::Threading::ReaderLockGuard guard(m_derivedKeyLock);
    const auto& secretKey = credentials.GetAWSSecretKey();
    if (secretKey != m_currentSecretKey || simpleDate != m_currentDateStr)
    {
        guard.UpgradeToWriterLock();
        // double-checked lock to prevent updating twice
        if (m_currentDateStr != simpleDate || m_currentSecretKey != secretKey)
        {
            m_currentSecretKey = secretKey;
            m_currentDateStr = simpleDate;
            m_derivedKey = ComputeHash(m_currentSecretKey, m_currentDateStr, region, serviceName);
        }

    }
    return GenerateSignature(stringToSign, m_derivedKey);
}

Aws::Utils::ByteBuffer AWSAuthEventStreamV4Signer::GenerateSignature(const Aws::String& stringToSign, const ByteBuffer& key) const
{
    AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "Final String to sign: " << stringToSign);

    Aws::StringStream ss;

    auto hashResult = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)stringToSign.c_str(), stringToSign.length()), key);
    if (hashResult.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Unable to hmac (sha256) final string");
        AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "The final string is: \"" << stringToSign << "\"");
        return {};
    }

    return hashResult;
}

Aws::String AWSAuthEventStreamV4Signer::GenerateStringToSign(const Aws::String& dateValue, const Aws::String& simpleDate,
        const Aws::String& canonicalRequestHash, const Aws::String& region, const Aws::String& serviceName) const
{
    //generate the actual string we will use in signing the final request.
    Aws::StringStream ss;

    ss << Aws::Auth::AWSAuthHelper::AWS_HMAC_SHA256 << Aws::Auth::AWSAuthHelper::NEWLINE << dateValue << Aws::Auth::AWSAuthHelper::NEWLINE << simpleDate << "/" << region << "/"
        << serviceName << "/" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST << Aws::Auth::AWSAuthHelper::NEWLINE << canonicalRequestHash;

    return ss.str();
}

Aws::Utils::ByteBuffer AWSAuthEventStreamV4Signer::ComputeHash(const Aws::String& secretKey,
        const Aws::String& simpleDate, const Aws::String& region, const Aws::String& serviceName) const
{
    Aws::String signingKey(Aws::Auth::AWSAuthHelper::SIGNING_KEY);
    signingKey.append(secretKey);
    auto hashResult = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)simpleDate.c_str(), simpleDate.length()),
            ByteBuffer((unsigned char*)signingKey.c_str(), signingKey.length()));

    if (hashResult.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Failed to HMAC (SHA256) date string \"" << simpleDate << "\"");
        return {};
    }

    hashResult = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)region.c_str(), region.length()), hashResult);
    if (hashResult.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Failed to HMAC (SHA256) region string \"" << region << "\"");
        return {};
    }

    hashResult = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)serviceName.c_str(), serviceName.length()), hashResult);
    if (hashResult.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Failed to HMAC (SHA256) service string \"" << m_serviceName << "\"");
        return {};
    }

    hashResult = HashingUtils::CalculateSHA256HMAC(ByteBuffer((unsigned char*)Aws::Auth::AWSAuthHelper::AWS4_REQUEST, strlen(Aws::Auth::AWSAuthHelper::AWS4_REQUEST)), hashResult);
    if (hashResult.GetLength() == 0)
    {
        AWS_LOGSTREAM_ERROR(v4StreamingLogTag, "Unable to HMAC (SHA256) request string");
        AWS_LOGSTREAM_DEBUG(v4StreamingLogTag, "The request string is: \"" << Aws::Auth::AWSAuthHelper::AWS4_REQUEST << "\"");
        return {};
    }
    return hashResult;
}
