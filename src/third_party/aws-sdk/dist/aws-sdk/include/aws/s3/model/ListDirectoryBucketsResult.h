/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Bucket.h>
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
  class ListDirectoryBucketsResult
  {
  public:
    AWS_S3_API ListDirectoryBucketsResult();
    AWS_S3_API ListDirectoryBucketsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListDirectoryBucketsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The list of buckets owned by the requester. </p>
     */
    inline const Aws::Vector<Bucket>& GetBuckets() const{ return m_buckets; }
    inline void SetBuckets(const Aws::Vector<Bucket>& value) { m_buckets = value; }
    inline void SetBuckets(Aws::Vector<Bucket>&& value) { m_buckets = std::move(value); }
    inline ListDirectoryBucketsResult& WithBuckets(const Aws::Vector<Bucket>& value) { SetBuckets(value); return *this;}
    inline ListDirectoryBucketsResult& WithBuckets(Aws::Vector<Bucket>&& value) { SetBuckets(std::move(value)); return *this;}
    inline ListDirectoryBucketsResult& AddBuckets(const Bucket& value) { m_buckets.push_back(value); return *this; }
    inline ListDirectoryBucketsResult& AddBuckets(Bucket&& value) { m_buckets.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>If <code>ContinuationToken</code> was sent with the request, it is included
     * in the response. You can use the returned <code>ContinuationToken</code> for
     * pagination of the list response.</p>
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationToken.assign(value); }
    inline ListDirectoryBucketsResult& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListDirectoryBucketsResult& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListDirectoryBucketsResult& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListDirectoryBucketsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListDirectoryBucketsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListDirectoryBucketsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<Bucket> m_buckets;

    Aws::String m_continuationToken;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
