/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/Owner.h>
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
  class ListBucketsResult
  {
  public:
    AWS_S3_API ListBucketsResult();
    AWS_S3_API ListBucketsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API ListBucketsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The list of buckets owned by the requester.</p>
     */
    inline const Aws::Vector<Bucket>& GetBuckets() const{ return m_buckets; }
    inline void SetBuckets(const Aws::Vector<Bucket>& value) { m_buckets = value; }
    inline void SetBuckets(Aws::Vector<Bucket>&& value) { m_buckets = std::move(value); }
    inline ListBucketsResult& WithBuckets(const Aws::Vector<Bucket>& value) { SetBuckets(value); return *this;}
    inline ListBucketsResult& WithBuckets(Aws::Vector<Bucket>&& value) { SetBuckets(std::move(value)); return *this;}
    inline ListBucketsResult& AddBuckets(const Bucket& value) { m_buckets.push_back(value); return *this; }
    inline ListBucketsResult& AddBuckets(Bucket&& value) { m_buckets.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The owner of the buckets listed.</p>
     */
    inline const Owner& GetOwner() const{ return m_owner; }
    inline void SetOwner(const Owner& value) { m_owner = value; }
    inline void SetOwner(Owner&& value) { m_owner = std::move(value); }
    inline ListBucketsResult& WithOwner(const Owner& value) { SetOwner(value); return *this;}
    inline ListBucketsResult& WithOwner(Owner&& value) { SetOwner(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p> <code>ContinuationToken</code> is included in the response when there are
     * more buckets that can be listed with pagination. The next
     * <code>ListBuckets</code> request to Amazon S3 can be continued with this
     * <code>ContinuationToken</code>. <code>ContinuationToken</code> is obfuscated and
     * is not a real bucket.</p>
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationToken.assign(value); }
    inline ListBucketsResult& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListBucketsResult& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListBucketsResult& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If <code>Prefix</code> was sent with the request, it is included in the
     * response.</p> <p>All bucket names in the response begin with the specified
     * bucket name prefix.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline void SetPrefix(const Aws::String& value) { m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefix.assign(value); }
    inline ListBucketsResult& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListBucketsResult& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListBucketsResult& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListBucketsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListBucketsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListBucketsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<Bucket> m_buckets;

    Owner m_owner;

    Aws::String m_continuationToken;

    Aws::String m_prefix;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
