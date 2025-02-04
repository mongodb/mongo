/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Payer.h>
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
  class GetBucketRequestPaymentResult
  {
  public:
    AWS_S3_API GetBucketRequestPaymentResult();
    AWS_S3_API GetBucketRequestPaymentResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketRequestPaymentResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Specifies who pays for the download and request fees.</p>
     */
    inline const Payer& GetPayer() const{ return m_payer; }
    inline void SetPayer(const Payer& value) { m_payer = value; }
    inline void SetPayer(Payer&& value) { m_payer = std::move(value); }
    inline GetBucketRequestPaymentResult& WithPayer(const Payer& value) { SetPayer(value); return *this;}
    inline GetBucketRequestPaymentResult& WithPayer(Payer&& value) { SetPayer(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketRequestPaymentResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketRequestPaymentResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketRequestPaymentResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Payer m_payer;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
