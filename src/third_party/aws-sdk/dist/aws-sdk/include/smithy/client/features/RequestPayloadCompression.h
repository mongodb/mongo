/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/client/RequestCompression.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpRequest.h>
#include <smithy/client/common/AwsSmithyClientUtils.h>
#include <iomanip>
#include <cassert>

namespace smithy
{
    namespace client
    {
        static const char AWS_CLIENT_REQUEST_COMPRESSION_LOG_TAG[] = "RequestPayloadCompression";

        class RequestPayloadCompression
        {
        public:
            static void AddCompressedContentBodyToRequest(const Aws::AmazonWebServiceRequest* pRequest,
                                                          const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
                                                          const Aws::Client::CompressionAlgorithm& compressionAlgorithm,
                                                          const std::shared_ptr<Aws::Http::HttpClient>& httpClient)
            {
                if (Aws::Client::CompressionAlgorithm::NONE != compressionAlgorithm)
                {
                    Aws::Client::RequestCompression rc;
                    auto compressOutcome = rc.compress(pRequest->GetBody(), compressionAlgorithm);

                    if (compressOutcome.IsSuccess())
                    {
                        const Aws::String compressionAlgorithmId = Aws::Client::GetCompressionAlgorithmId(
                            compressionAlgorithm);
                        Utils::AppendHeaderValueToRequest(httpRequest, Aws::Http::CONTENT_ENCODING_HEADER,
                                                          compressionAlgorithmId);
                        Utils::AddContentBodyToRequest(httpRequest,
                                                       compressOutcome.GetResult(),
                                                       httpClient,
                                                       pRequest->ShouldComputeContentMd5(),
                                                       pRequest->IsStreaming() && pRequest->IsChunked() && httpClient->
                                                       SupportsChunkedTransferEncoding());
                    }
                    else
                    {
                        AWS_LOGSTREAM_ERROR(AWS_CLIENT_REQUEST_COMPRESSION_LOG_TAG,
                                            "Failed to compress request, submitting uncompressed");
                        Utils::AddContentBodyToRequest(httpRequest,
                                                       pRequest->GetBody(),
                                                       httpClient,
                                                       pRequest->ShouldComputeContentMd5(),
                                                       pRequest->IsStreaming() && pRequest->IsChunked() && httpClient->
                                                       SupportsChunkedTransferEncoding());
                    }
                }
            }
        };
    }
}
