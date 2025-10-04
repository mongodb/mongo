/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/LocationType.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class HeadBucketResult
  {
  public:
    AWS_S3_API HeadBucketResult();
    AWS_S3_API HeadBucketResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API HeadBucketResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The type of location where the bucket is created.</p>  <p>This
     * functionality is only supported by directory buckets.</p> 
     */
    inline const LocationType& GetBucketLocationType() const{ return m_bucketLocationType; }
    inline void SetBucketLocationType(const LocationType& value) { m_bucketLocationType = value; }
    inline void SetBucketLocationType(LocationType&& value) { m_bucketLocationType = std::move(value); }
    inline HeadBucketResult& WithBucketLocationType(const LocationType& value) { SetBucketLocationType(value); return *this;}
    inline HeadBucketResult& WithBucketLocationType(LocationType&& value) { SetBucketLocationType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the location where the bucket will be created.</p> <p>For
     * directory buckets, the Zone ID of the Availability Zone or the Local Zone where
     * the bucket is created. An example Zone ID value for an Availability Zone is
     * <code>usw2-az1</code>.</p>  <p>This functionality is only supported by
     * directory buckets.</p> 
     */
    inline const Aws::String& GetBucketLocationName() const{ return m_bucketLocationName; }
    inline void SetBucketLocationName(const Aws::String& value) { m_bucketLocationName = value; }
    inline void SetBucketLocationName(Aws::String&& value) { m_bucketLocationName = std::move(value); }
    inline void SetBucketLocationName(const char* value) { m_bucketLocationName.assign(value); }
    inline HeadBucketResult& WithBucketLocationName(const Aws::String& value) { SetBucketLocationName(value); return *this;}
    inline HeadBucketResult& WithBucketLocationName(Aws::String&& value) { SetBucketLocationName(std::move(value)); return *this;}
    inline HeadBucketResult& WithBucketLocationName(const char* value) { SetBucketLocationName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Region that the bucket is located.</p>
     */
    inline const Aws::String& GetBucketRegion() const{ return m_bucketRegion; }
    inline void SetBucketRegion(const Aws::String& value) { m_bucketRegion = value; }
    inline void SetBucketRegion(Aws::String&& value) { m_bucketRegion = std::move(value); }
    inline void SetBucketRegion(const char* value) { m_bucketRegion.assign(value); }
    inline HeadBucketResult& WithBucketRegion(const Aws::String& value) { SetBucketRegion(value); return *this;}
    inline HeadBucketResult& WithBucketRegion(Aws::String&& value) { SetBucketRegion(std::move(value)); return *this;}
    inline HeadBucketResult& WithBucketRegion(const char* value) { SetBucketRegion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the bucket name used in the request is an access point
     * alias.</p>  <p>For directory buckets, the value of this field is
     * <code>false</code>.</p> 
     */
    inline bool GetAccessPointAlias() const{ return m_accessPointAlias; }
    inline void SetAccessPointAlias(bool value) { m_accessPointAlias = value; }
    inline HeadBucketResult& WithAccessPointAlias(bool value) { SetAccessPointAlias(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline HeadBucketResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline HeadBucketResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline HeadBucketResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    LocationType m_bucketLocationType;

    Aws::String m_bucketLocationName;

    Aws::String m_bucketRegion;

    bool m_accessPointAlias;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
