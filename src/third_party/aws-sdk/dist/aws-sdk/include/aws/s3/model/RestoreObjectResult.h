/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
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
  class RestoreObjectResult
  {
  public:
    AWS_S3_API RestoreObjectResult();
    AWS_S3_API RestoreObjectResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API RestoreObjectResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline RestoreObjectResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline RestoreObjectResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates the path in the provided S3 output location where Select results
     * will be restored to.</p>
     */
    inline const Aws::String& GetRestoreOutputPath() const{ return m_restoreOutputPath; }
    inline void SetRestoreOutputPath(const Aws::String& value) { m_restoreOutputPath = value; }
    inline void SetRestoreOutputPath(Aws::String&& value) { m_restoreOutputPath = std::move(value); }
    inline void SetRestoreOutputPath(const char* value) { m_restoreOutputPath.assign(value); }
    inline RestoreObjectResult& WithRestoreOutputPath(const Aws::String& value) { SetRestoreOutputPath(value); return *this;}
    inline RestoreObjectResult& WithRestoreOutputPath(Aws::String&& value) { SetRestoreOutputPath(std::move(value)); return *this;}
    inline RestoreObjectResult& WithRestoreOutputPath(const char* value) { SetRestoreOutputPath(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline RestoreObjectResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline RestoreObjectResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline RestoreObjectResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    RequestCharged m_requestCharged;

    Aws::String m_restoreOutputPath;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
