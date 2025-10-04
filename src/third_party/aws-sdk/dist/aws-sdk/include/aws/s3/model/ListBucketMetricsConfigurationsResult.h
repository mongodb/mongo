/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/MetricsConfiguration.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{
  class ListBucketMetricsConfigurationsResult
  {
  public:
    AWS_S3_API ListBucketMetricsConfigurationsResult();
    AWS_S3_API ListBucketMetricsConfigurationsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListBucketMetricsConfigurationsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Indicates whether the returned list of metrics configurations is complete. A
     * value of true indicates that the list is not complete and the
     * NextContinuationToken will be provided for a subsequent request.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListBucketMetricsConfigurationsResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The marker that is used as a starting point for this metrics configuration
     * list response. This value is present if it was sent in the request.</p>
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationToken.assign(value); }
    inline ListBucketMetricsConfigurationsResult& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The marker used to continue a metrics configuration listing that has been
     * truncated. Use the <code>NextContinuationToken</code> from a previously
     * truncated list response to continue the listing. The continuation token is an
     * opaque value that Amazon S3 understands.</p>
     */
    inline const Aws::String& GetNextContinuationToken() const{ return m_nextContinuationToken; }
    inline void SetNextContinuationToken(const Aws::String& value) { m_nextContinuationToken = value; }
    inline void SetNextContinuationToken(Aws::String&& value) { m_nextContinuationToken = std::move(value); }
    inline void SetNextContinuationToken(const char* value) { m_nextContinuationToken.assign(value); }
    inline ListBucketMetricsConfigurationsResult& WithNextContinuationToken(const Aws::String& value) { SetNextContinuationToken(value); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithNextContinuationToken(Aws::String&& value) { SetNextContinuationToken(std::move(value)); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithNextContinuationToken(const char* value) { SetNextContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The list of metrics configurations for a bucket.</p>
     */
    inline const Aws::Vector<MetricsConfiguration>& GetMetricsConfigurationList() const{ return m_metricsConfigurationList; }
    inline void SetMetricsConfigurationList(const Aws::Vector<MetricsConfiguration>& value) { m_metricsConfigurationList = value; }
    inline void SetMetricsConfigurationList(Aws::Vector<MetricsConfiguration>&& value) { m_metricsConfigurationList = std::move(value); }
    inline ListBucketMetricsConfigurationsResult& WithMetricsConfigurationList(const Aws::Vector<MetricsConfiguration>& value) { SetMetricsConfigurationList(value); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithMetricsConfigurationList(Aws::Vector<MetricsConfiguration>&& value) { SetMetricsConfigurationList(std::move(value)); return *this;}
    inline ListBucketMetricsConfigurationsResult& AddMetricsConfigurationList(const MetricsConfiguration& value) { m_metricsConfigurationList.push_back(value); return *this; }
    inline ListBucketMetricsConfigurationsResult& AddMetricsConfigurationList(MetricsConfiguration&& value) { m_metricsConfigurationList.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListBucketMetricsConfigurationsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListBucketMetricsConfigurationsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_isTruncated;

    Aws::String m_continuationToken;

    Aws::String m_nextContinuationToken;

    Aws::Vector<MetricsConfiguration> m_metricsConfigurationList;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
