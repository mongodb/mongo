/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/ProvisionedConcurrencyConfigListItem.h>
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
  class ListProvisionedConcurrencyConfigsResult
  {
  public:
    AWS_LAMBDA_API ListProvisionedConcurrencyConfigsResult();
    AWS_LAMBDA_API ListProvisionedConcurrencyConfigsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListProvisionedConcurrencyConfigsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A list of provisioned concurrency configurations.</p>
     */
    inline const Aws::Vector<ProvisionedConcurrencyConfigListItem>& GetProvisionedConcurrencyConfigs() const{ return m_provisionedConcurrencyConfigs; }
    inline void SetProvisionedConcurrencyConfigs(const Aws::Vector<ProvisionedConcurrencyConfigListItem>& value) { m_provisionedConcurrencyConfigs = value; }
    inline void SetProvisionedConcurrencyConfigs(Aws::Vector<ProvisionedConcurrencyConfigListItem>&& value) { m_provisionedConcurrencyConfigs = std::move(value); }
    inline ListProvisionedConcurrencyConfigsResult& WithProvisionedConcurrencyConfigs(const Aws::Vector<ProvisionedConcurrencyConfigListItem>& value) { SetProvisionedConcurrencyConfigs(value); return *this;}
    inline ListProvisionedConcurrencyConfigsResult& WithProvisionedConcurrencyConfigs(Aws::Vector<ProvisionedConcurrencyConfigListItem>&& value) { SetProvisionedConcurrencyConfigs(std::move(value)); return *this;}
    inline ListProvisionedConcurrencyConfigsResult& AddProvisionedConcurrencyConfigs(const ProvisionedConcurrencyConfigListItem& value) { m_provisionedConcurrencyConfigs.push_back(value); return *this; }
    inline ListProvisionedConcurrencyConfigsResult& AddProvisionedConcurrencyConfigs(ProvisionedConcurrencyConfigListItem&& value) { m_provisionedConcurrencyConfigs.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The pagination token that's included if more results are available.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListProvisionedConcurrencyConfigsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListProvisionedConcurrencyConfigsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListProvisionedConcurrencyConfigsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListProvisionedConcurrencyConfigsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListProvisionedConcurrencyConfigsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListProvisionedConcurrencyConfigsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<ProvisionedConcurrencyConfigListItem> m_provisionedConcurrencyConfigs;

    Aws::String m_nextMarker;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
