/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/BucketLocationConstraint.h>
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
  class GetBucketLocationResult
  {
  public:
    AWS_S3_API GetBucketLocationResult();
    AWS_S3_API GetBucketLocationResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketLocationResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Specifies the Region where the bucket resides. For a list of all the Amazon
     * S3 supported location constraints by Region, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
     * and Endpoints</a>. Buckets in Region <code>us-east-1</code> have a
     * LocationConstraint of <code>null</code>.</p>
     */
    inline const BucketLocationConstraint& GetLocationConstraint() const{ return m_locationConstraint; }
    inline void SetLocationConstraint(const BucketLocationConstraint& value) { m_locationConstraint = value; }
    inline void SetLocationConstraint(BucketLocationConstraint&& value) { m_locationConstraint = std::move(value); }
    inline GetBucketLocationResult& WithLocationConstraint(const BucketLocationConstraint& value) { SetLocationConstraint(value); return *this;}
    inline GetBucketLocationResult& WithLocationConstraint(BucketLocationConstraint&& value) { SetLocationConstraint(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketLocationResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketLocationResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketLocationResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    BucketLocationConstraint m_locationConstraint;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
