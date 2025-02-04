/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/IntelligentTieringConfiguration.h>
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
  class ListBucketIntelligentTieringConfigurationsResult
  {
  public:
    AWS_S3_API ListBucketIntelligentTieringConfigurationsResult();
    AWS_S3_API ListBucketIntelligentTieringConfigurationsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListBucketIntelligentTieringConfigurationsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Indicates whether the returned list of analytics configurations is complete.
     * A value of <code>true</code> indicates that the list is not complete and the
     * <code>NextContinuationToken</code> will be provided for a subsequent
     * request.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListBucketIntelligentTieringConfigurationsResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <code>ContinuationToken</code> that represents a placeholder from where
     * this request should begin.</p>
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationToken.assign(value); }
    inline ListBucketIntelligentTieringConfigurationsResult& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The marker used to continue this inventory configuration listing. Use the
     * <code>NextContinuationToken</code> from this response to continue the listing in
     * a subsequent request. The continuation token is an opaque value that Amazon S3
     * understands.</p>
     */
    inline const Aws::String& GetNextContinuationToken() const{ return m_nextContinuationToken; }
    inline void SetNextContinuationToken(const Aws::String& value) { m_nextContinuationToken = value; }
    inline void SetNextContinuationToken(Aws::String&& value) { m_nextContinuationToken = std::move(value); }
    inline void SetNextContinuationToken(const char* value) { m_nextContinuationToken.assign(value); }
    inline ListBucketIntelligentTieringConfigurationsResult& WithNextContinuationToken(const Aws::String& value) { SetNextContinuationToken(value); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithNextContinuationToken(Aws::String&& value) { SetNextContinuationToken(std::move(value)); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithNextContinuationToken(const char* value) { SetNextContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The list of S3 Intelligent-Tiering configurations for a bucket.</p>
     */
    inline const Aws::Vector<IntelligentTieringConfiguration>& GetIntelligentTieringConfigurationList() const{ return m_intelligentTieringConfigurationList; }
    inline void SetIntelligentTieringConfigurationList(const Aws::Vector<IntelligentTieringConfiguration>& value) { m_intelligentTieringConfigurationList = value; }
    inline void SetIntelligentTieringConfigurationList(Aws::Vector<IntelligentTieringConfiguration>&& value) { m_intelligentTieringConfigurationList = std::move(value); }
    inline ListBucketIntelligentTieringConfigurationsResult& WithIntelligentTieringConfigurationList(const Aws::Vector<IntelligentTieringConfiguration>& value) { SetIntelligentTieringConfigurationList(value); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithIntelligentTieringConfigurationList(Aws::Vector<IntelligentTieringConfiguration>&& value) { SetIntelligentTieringConfigurationList(std::move(value)); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& AddIntelligentTieringConfigurationList(const IntelligentTieringConfiguration& value) { m_intelligentTieringConfigurationList.push_back(value); return *this; }
    inline ListBucketIntelligentTieringConfigurationsResult& AddIntelligentTieringConfigurationList(IntelligentTieringConfiguration&& value) { m_intelligentTieringConfigurationList.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListBucketIntelligentTieringConfigurationsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListBucketIntelligentTieringConfigurationsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_isTruncated;

    Aws::String m_continuationToken;

    Aws::String m_nextContinuationToken;

    Aws::Vector<IntelligentTieringConfiguration> m_intelligentTieringConfigurationList;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
