/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <iomanip>
#include <aws/core/http/HttpRequest.h>

#include <cassert>

#include <aws/core/http/HttpClient.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/crypto/MD5.h>
#include <aws/core/utils/logging/LogMacros.h>


namespace smithy
{
    namespace client
    {
        static const char AWS_SMITHY_CLIENT_UTILS_TAG[] = "AwsSmithyClientUtils";

        class Utils
        {
        public:
            using HttpMethod = Aws::Http::HttpMethod;
            using HeaderValueCollection = Aws::Http::HeaderValueCollection;
            using DateTime = Aws::Utils::DateTime;
            using DateFormat = Aws::Utils::DateFormat;
            using ClientError = Aws::Client::AWSError<Aws::Client::CoreErrors>;

            static void AppendHeaderValueToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
                                                   const Aws::String& header,
                                                   const Aws::String& value)
            {
                assert(httpRequest);
                assert(!header.empty());

                if (!httpRequest->HasHeader(header.c_str()))
                {
                    httpRequest->SetHeaderValue(header, value);
                }
                else
                {
                    Aws::String contentEncoding = httpRequest->GetHeaderValue(header.c_str());
                    contentEncoding.append(",").append(value);
                    httpRequest->SetHeaderValue(header, contentEncoding);
                }
            }

            static void AddContentBodyToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
                                                const std::shared_ptr<Aws::IOStream>& body,
                                                const std::shared_ptr<Aws::Http::HttpClient>& httpClient,
                                                bool needsContentMd5,
                                                bool isChunked)
            {
                assert(httpRequest);

                httpRequest->AddContentBody(body);

                //If there is no body, we have a content length of 0
                //note: we also used to remove content-type, but S3 actually needs content-type on InitiateMultipartUpload and it isn't
                //forbidden by the spec. If we start getting weird errors related to this, make sure it isn't caused by this removal.
                if (!body)
                {
                    AWS_LOGSTREAM_TRACE(AWS_SMITHY_CLIENT_UTILS_TAG, "No content body, content-length headers");

                    if (httpRequest->GetMethod() == HttpMethod::HTTP_POST || httpRequest->GetMethod() ==
                        HttpMethod::HTTP_PUT)
                    {
                        httpRequest->SetHeaderValue(Aws::Http::CONTENT_LENGTH_HEADER, "0");
                    }
                    else
                    {
                        httpRequest->DeleteHeader(Aws::Http::CONTENT_LENGTH_HEADER);
                    }
                }

                //Add transfer-encoding:chunked to header
                if (body && isChunked && !httpRequest->HasHeader(Aws::Http::CONTENT_LENGTH_HEADER))
                {
                    httpRequest->SetTransferEncoding(Aws::Http::CHUNKED_VALUE);
                }
                //in the scenario where we are adding a content body as a stream, the request object likely already
                //has a content-length header set and we don't want to seek the stream just to find this information.
                else if (body && !httpRequest->HasHeader(Aws::Http::CONTENT_LENGTH_HEADER))
                {
                    assert(httpClient);
                    if (!httpClient->SupportsChunkedTransferEncoding())
                    {
                        AWS_LOGSTREAM_WARN(AWS_SMITHY_CLIENT_UTILS_TAG,
                                           "This http client doesn't support transfer-encoding:chunked. " <<
                                           "The request may fail if it's not a seekable stream.");
                    }
                    AWS_LOGSTREAM_TRACE(AWS_SMITHY_CLIENT_UTILS_TAG,
                                        "Found body, but content-length has not been set, attempting to compute content-length");
                    body->seekg(0, body->end);
                    auto streamSize = body->tellg();
                    body->seekg(0, body->beg);
                    Aws::StringStream ss;
                    ss << streamSize;
                    httpRequest->SetContentLength(ss.str());
                }

                if (needsContentMd5 && body && !httpRequest->HasHeader(Aws::Http::CONTENT_MD5_HEADER))
                {
                    AWS_LOGSTREAM_TRACE(AWS_SMITHY_CLIENT_UTILS_TAG, "Found body, and content-md5 needs to be set" <<
                                        ", attempting to compute content-md5");

                    //changing the internal state of the hash computation is not a logical state
                    //change as far as constness goes for this class. Due to the platform specificness
                    //of hash computations, we can't control the fact that computing a hash mutates
                    //state on some platforms such as windows (but that isn't a concern of this class.

                    // TODO: check refactoring from:
                    // auto md5HashResult = const_cast<AWSClient*>(this)->m_hash->Calculate(*body);
                    Aws::Utils::Crypto::MD5 hash;
                    auto md5HashResult = hash.Calculate(*body);
                    body->clear();
                    if (md5HashResult.IsSuccess())
                    {
                        httpRequest->SetHeaderValue(Aws::Http::CONTENT_MD5_HEADER,
                                                    Aws::Utils::HashingUtils::Base64Encode(md5HashResult.GetResult()));
                    }
                }
            }

            static bool DoesResponseGenerateError(const std::shared_ptr<Aws::Http::HttpResponse>& response)
            {
                assert(response);
                if (response->HasClientError())
                    return true;

                static const int SUCCESS_RESPONSE_MIN = 200;
                static const int SUCCESS_RESPONSE_MAX = 299;

                const int responseCode = static_cast<int>(response->GetResponseCode());
                return responseCode < SUCCESS_RESPONSE_MIN || responseCode > SUCCESS_RESPONSE_MAX;
            }

            static Aws::Utils::DateTime GetServerTimeFromError(const ClientError& error)
            {
                const HeaderValueCollection& headers = error.GetResponseHeaders();
                auto awsDateHeaderIter = headers.find(Aws::Utils::StringUtils::ToLower(Aws::Http::AWS_DATE_HEADER));
                auto dateHeaderIter = headers.find(Aws::Utils::StringUtils::ToLower(Aws::Http::DATE_HEADER));
                if (awsDateHeaderIter != headers.end())
                {
                    return DateTime(awsDateHeaderIter->second.c_str(), DateFormat::AutoDetect);
                }
                else if (dateHeaderIter != headers.end())
                {
                    return DateTime(dateHeaderIter->second.c_str(), DateFormat::AutoDetect);
                }
                return DateTime();
            }
        };
    }
}
