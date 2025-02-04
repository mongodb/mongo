/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/BucketAccelerateStatus.h>
#include <aws/s3/model/RequestCharged.h>
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
  class GetBucketAccelerateConfigurationResult
  {
  public:
    AWS_S3_API GetBucketAccelerateConfigurationResult();
    AWS_S3_API GetBucketAccelerateConfigurationResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketAccelerateConfigurationResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The accelerate configuration of the bucket.</p>
     */
    inline const BucketAccelerateStatus& GetStatus() const{ return m_status; }
    inline void SetStatus(const BucketAccelerateStatus& value) { m_status = value; }
    inline void SetStatus(BucketAccelerateStatus&& value) { m_status = std::move(value); }
    inline GetBucketAccelerateConfigurationResult& WithStatus(const BucketAccelerateStatus& value) { SetStatus(value); return *this;}
    inline GetBucketAccelerateConfigurationResult& WithStatus(BucketAccelerateStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline GetBucketAccelerateConfigurationResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline GetBucketAccelerateConfigurationResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketAccelerateConfigurationResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketAccelerateConfigurationResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketAccelerateConfigurationResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    BucketAccelerateStatus m_status;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
