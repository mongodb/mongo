/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/signer/AWSAuthSignerHelper.h>
#include <aws/core/http/HttpTypes.h>

#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/StringUtils.h>

namespace Aws
{
namespace Auth
{

const char* AWSAuthHelper::EQ = "=";
const char* AWSAuthHelper::AWS_HMAC_SHA256 = "AWS4-HMAC-SHA256";
const char* AWSAuthHelper::AWS4_REQUEST = "aws4_request";
const char* AWSAuthHelper::SIGNED_HEADERS = "SignedHeaders";
const char* AWSAuthHelper::CREDENTIAL = "Credential";
const char* AWSAuthHelper::NEWLINE = "\n";
const char* AWSAuthHelper::X_AMZN_TRACE_ID = "x-amzn-trace-id";
const char* AWSAuthHelper::X_AMZ_CONTENT_SHA256 = "x-amz-content-sha256";
const char* AWSAuthHelper::SIGNING_KEY = "AWS4";
const char* AWSAuthHelper::SIMPLE_DATE_FORMAT_STR = "%Y%m%d";

Aws::String Aws::Auth::AWSAuthHelper::CanonicalizeRequestSigningString(Aws::Http::HttpRequest& request, bool urlEscapePath)
{
    request.CanonicalizeRequest();
    Aws::StringStream signingStringStream;
    signingStringStream << Aws::Http::HttpMethodMapper::GetNameForHttpMethod(request.GetMethod());

    Aws::Http::URI uriCpy = request.GetUri();
    // Many AWS services do not decode the URL before calculating SignatureV4 on their end.
    // This results in the signature getting calculated with a double encoded URL.
    // That means we have to double encode it here for the signature to match on the service side.
    if(urlEscapePath)
    {
        // RFC3986 is how we encode the URL before sending it on the wire.
        uriCpy.SetPath(uriCpy.GetURLEncodedPathRFC3986());
        // However, SignatureV4 uses this URL encoding scheme
        signingStringStream << AWSAuthHelper::NEWLINE << uriCpy.GetURLEncodedPath() << AWSAuthHelper::NEWLINE;
    }
    else
    {
        // For the services that DO decode the URL first; we don't need to double encode it.
        signingStringStream << AWSAuthHelper::NEWLINE << uriCpy.GetURLEncodedPath() << AWSAuthHelper::NEWLINE;
    }

    if (request.GetQueryString().find('=') != std::string::npos)
    {
        signingStringStream << request.GetQueryString().substr(1) << AWSAuthHelper::NEWLINE;
    }
    else if (request.GetQueryString().size() > 1)
    {
        signingStringStream << request.GetQueryString().substr(1) << "=" << AWSAuthHelper::NEWLINE;
    }
    else
    {
        signingStringStream << AWSAuthHelper::NEWLINE;
    }

    return signingStringStream.str();
}

Aws::Http::HeaderValueCollection Aws::Auth::AWSAuthHelper::CanonicalizeHeaders(Aws::Http::HeaderValueCollection&& headers)
{
    Aws::Http::HeaderValueCollection canonicalHeaders;
    for (const auto& header : headers)
    {
        auto trimmedHeaderName = Aws::Utils::StringUtils::Trim(header.first.c_str());
        auto trimmedHeaderValue = Aws::Utils::StringUtils::Trim(header.second.c_str());

        //multiline gets converted to line1,line2,etc...
        auto headerMultiLine = Aws::Utils::StringUtils::SplitOnLine(trimmedHeaderValue);
        Aws::String headerValue = headerMultiLine.size() == 0 ? "" : headerMultiLine[0];

        if (headerMultiLine.size() > 1)
        {
            for(size_t i = 1; i < headerMultiLine.size(); ++i)
            {
                headerValue += " ";
                headerValue += Aws::Utils::StringUtils::Trim(headerMultiLine[i].c_str());
            }
        }

        //duplicate spaces need to be converted to one.
        Aws::String::iterator new_end =
            std::unique(headerValue.begin(), headerValue.end(),
                [=](char lhs, char rhs) { return (lhs == rhs) && (lhs == ' '); }
        );
        headerValue.erase(new_end, headerValue.end());

        canonicalHeaders[trimmedHeaderName] = headerValue;
    }

    return canonicalHeaders;
}

} // namespace Auth
} // namespace Aws