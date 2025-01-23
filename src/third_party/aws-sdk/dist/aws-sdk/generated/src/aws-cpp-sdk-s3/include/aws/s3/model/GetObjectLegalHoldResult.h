/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ObjectLockLegalHold.h>
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
  class GetObjectLegalHoldResult
  {
  public:
    AWS_S3_API GetObjectLegalHoldResult();
    AWS_S3_API GetObjectLegalHoldResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetObjectLegalHoldResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The current legal hold status for the specified object.</p>
     */
    inline const ObjectLockLegalHold& GetLegalHold() const{ return m_legalHold; }
    inline void SetLegalHold(const ObjectLockLegalHold& value) { m_legalHold = value; }
    inline void SetLegalHold(ObjectLockLegalHold&& value) { m_legalHold = std::move(value); }
    inline GetObjectLegalHoldResult& WithLegalHold(const ObjectLockLegalHold& value) { SetLegalHold(value); return *this;}
    inline GetObjectLegalHoldResult& WithLegalHold(ObjectLockLegalHold&& value) { SetLegalHold(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetObjectLegalHoldResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetObjectLegalHoldResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetObjectLegalHoldResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    ObjectLockLegalHold m_legalHold;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
