/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/OwnershipControls.h>
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
  class GetBucketOwnershipControlsResult
  {
  public:
    AWS_S3_API GetBucketOwnershipControlsResult();
    AWS_S3_API GetBucketOwnershipControlsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketOwnershipControlsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The <code>OwnershipControls</code> (BucketOwnerEnforced,
     * BucketOwnerPreferred, or ObjectWriter) currently in effect for this Amazon S3
     * bucket.</p>
     */
    inline const OwnershipControls& GetOwnershipControls() const{ return m_ownershipControls; }
    inline void SetOwnershipControls(const OwnershipControls& value) { m_ownershipControls = value; }
    inline void SetOwnershipControls(OwnershipControls&& value) { m_ownershipControls = std::move(value); }
    inline GetBucketOwnershipControlsResult& WithOwnershipControls(const OwnershipControls& value) { SetOwnershipControls(value); return *this;}
    inline GetBucketOwnershipControlsResult& WithOwnershipControls(OwnershipControls&& value) { SetOwnershipControls(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketOwnershipControlsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketOwnershipControlsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketOwnershipControlsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    OwnershipControls m_ownershipControls;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
