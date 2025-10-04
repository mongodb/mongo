/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpRequest.h>

using namespace Aws;
using namespace Aws::Http;

HttpClient::HttpClient() :
    m_bad(false),
    m_disableRequestProcessing( false ),
    m_requestProcessingSignalLock(),
    m_requestProcessingSignal()
{
}

void HttpClient::DisableRequestProcessing() 
{ 
    m_disableRequestProcessing = true;
    m_requestProcessingSignal.notify_all();
}

void HttpClient::EnableRequestProcessing() 
{ 
    m_disableRequestProcessing = false; 
}

bool HttpClient::IsRequestProcessingEnabled() const 
{ 
    return m_disableRequestProcessing.load() == false; 
}

void HttpClient::RetryRequestSleep(std::chrono::milliseconds sleepTime) 
{
    std::unique_lock< std::mutex > signalLocker(m_requestProcessingSignalLock);
    m_requestProcessingSignal.wait_for(signalLocker, sleepTime, [this](){ return m_disableRequestProcessing.load() == true; });
}

bool HttpClient::ContinueRequest(const Aws::Http::HttpRequest& request) const
{
    if (request.GetContinueRequestHandler())
    {
        return request.GetContinueRequestHandler()(&request);
    }

    return true;
}
