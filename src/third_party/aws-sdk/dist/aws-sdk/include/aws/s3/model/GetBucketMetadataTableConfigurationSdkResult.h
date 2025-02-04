/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/GetBucketMetadataTableConfigurationResult.h>
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
  class GetBucketMetadataTableConfigurationSdkResult
  {
  public:
    AWS_S3_API GetBucketMetadataTableConfigurationSdkResult();
    AWS_S3_API GetBucketMetadataTableConfigurationSdkResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketMetadataTableConfigurationSdkResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p> The metadata table configuration for the general purpose bucket. </p>
     */
    inline const GetBucketMetadataTableConfigurationResult& GetGetBucketMetadataTableConfigurationResult() const{ return m_getBucketMetadataTableConfigurationResult; }
    inline void SetGetBucketMetadataTableConfigurationResult(const GetBucketMetadataTableConfigurationResult& value) { m_getBucketMetadataTableConfigurationResult = value; }
    inline void SetGetBucketMetadataTableConfigurationResult(GetBucketMetadataTableConfigurationResult&& value) { m_getBucketMetadataTableConfigurationResult = std::move(value); }
    inline GetBucketMetadataTableConfigurationSdkResult& WithGetBucketMetadataTableConfigurationResult(const GetBucketMetadataTableConfigurationResult& value) { SetGetBucketMetadataTableConfigurationResult(value); return *this;}
    inline GetBucketMetadataTableConfigurationSdkResult& WithGetBucketMetadataTableConfigurationResult(GetBucketMetadataTableConfigurationResult&& value) { SetGetBucketMetadataTableConfigurationResult(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketMetadataTableConfigurationSdkResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketMetadataTableConfigurationSdkResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketMetadataTableConfigurationSdkResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    GetBucketMetadataTableConfigurationResult m_getBucketMetadataTableConfigurationResult;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
