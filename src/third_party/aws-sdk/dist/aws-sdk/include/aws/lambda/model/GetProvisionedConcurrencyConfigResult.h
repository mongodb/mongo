/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/ProvisionedConcurrencyStatusEnum.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{
  class GetProvisionedConcurrencyConfigResult
  {
  public:
    AWS_LAMBDA_API GetProvisionedConcurrencyConfigResult();
    AWS_LAMBDA_API GetProvisionedConcurrencyConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetProvisionedConcurrencyConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The amount of provisioned concurrency requested.</p>
     */
    inline int GetRequestedProvisionedConcurrentExecutions() const{ return m_requestedProvisionedConcurrentExecutions; }
    inline void SetRequestedProvisionedConcurrentExecutions(int value) { m_requestedProvisionedConcurrentExecutions = value; }
    inline GetProvisionedConcurrencyConfigResult& WithRequestedProvisionedConcurrentExecutions(int value) { SetRequestedProvisionedConcurrentExecutions(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The amount of provisioned concurrency available.</p>
     */
    inline int GetAvailableProvisionedConcurrentExecutions() const{ return m_availableProvisionedConcurrentExecutions; }
    inline void SetAvailableProvisionedConcurrentExecutions(int value) { m_availableProvisionedConcurrentExecutions = value; }
    inline GetProvisionedConcurrencyConfigResult& WithAvailableProvisionedConcurrentExecutions(int value) { SetAvailableProvisionedConcurrentExecutions(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The amount of provisioned concurrency allocated. When a weighted alias is
     * used during linear and canary deployments, this value fluctuates depending on
     * the amount of concurrency that is provisioned for the function versions.</p>
     */
    inline int GetAllocatedProvisionedConcurrentExecutions() const{ return m_allocatedProvisionedConcurrentExecutions; }
    inline void SetAllocatedProvisionedConcurrentExecutions(int value) { m_allocatedProvisionedConcurrentExecutions = value; }
    inline GetProvisionedConcurrencyConfigResult& WithAllocatedProvisionedConcurrentExecutions(int value) { SetAllocatedProvisionedConcurrentExecutions(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The status of the allocation process.</p>
     */
    inline const ProvisionedConcurrencyStatusEnum& GetStatus() const{ return m_status; }
    inline void SetStatus(const ProvisionedConcurrencyStatusEnum& value) { m_status = value; }
    inline void SetStatus(ProvisionedConcurrencyStatusEnum&& value) { m_status = std::move(value); }
    inline GetProvisionedConcurrencyConfigResult& WithStatus(const ProvisionedConcurrencyStatusEnum& value) { SetStatus(value); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithStatus(ProvisionedConcurrencyStatusEnum&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>For failed allocations, the reason that provisioned concurrency could not be
     * allocated.</p>
     */
    inline const Aws::String& GetStatusReason() const{ return m_statusReason; }
    inline void SetStatusReason(const Aws::String& value) { m_statusReason = value; }
    inline void SetStatusReason(Aws::String&& value) { m_statusReason = std::move(value); }
    inline void SetStatusReason(const char* value) { m_statusReason.assign(value); }
    inline GetProvisionedConcurrencyConfigResult& WithStatusReason(const Aws::String& value) { SetStatusReason(value); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithStatusReason(Aws::String&& value) { SetStatusReason(std::move(value)); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithStatusReason(const char* value) { SetStatusReason(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time that a user last updated the configuration, in <a
     * href="https://www.iso.org/iso-8601-date-and-time-format.html">ISO 8601
     * format</a>.</p>
     */
    inline const Aws::String& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::String& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::String&& value) { m_lastModified = std::move(value); }
    inline void SetLastModified(const char* value) { m_lastModified.assign(value); }
    inline GetProvisionedConcurrencyConfigResult& WithLastModified(const Aws::String& value) { SetLastModified(value); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithLastModified(Aws::String&& value) { SetLastModified(std::move(value)); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithLastModified(const char* value) { SetLastModified(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetProvisionedConcurrencyConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetProvisionedConcurrencyConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_requestedProvisionedConcurrentExecutions;

    int m_availableProvisionedConcurrentExecutions;

    int m_allocatedProvisionedConcurrentExecutions;

    ProvisionedConcurrencyStatusEnum m_status;

    Aws::String m_statusReason;

    Aws::String m_lastModified;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
