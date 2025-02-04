/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/RequestCharged.h>
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
  class DeleteObjectResult
  {
  public:
    AWS_S3_API DeleteObjectResult();
    AWS_S3_API DeleteObjectResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API DeleteObjectResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Indicates whether the specified object version that was permanently deleted
     * was (true) or was not (false) a delete marker before deletion. In a simple
     * DELETE, this header indicates whether (true) or not (false) the current version
     * of the object is a delete marker.</p>  <p>This functionality is not
     * supported for directory buckets.</p> 
     */
    inline bool GetDeleteMarker() const{ return m_deleteMarker; }
    inline void SetDeleteMarker(bool value) { m_deleteMarker = value; }
    inline DeleteObjectResult& WithDeleteMarker(bool value) { SetDeleteMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Returns the version ID of the delete marker created as a result of the DELETE
     * operation.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline void SetVersionId(const Aws::String& value) { m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionId.assign(value); }
    inline DeleteObjectResult& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline DeleteObjectResult& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline DeleteObjectResult& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline DeleteObjectResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline DeleteObjectResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DeleteObjectResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DeleteObjectResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DeleteObjectResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_deleteMarker;

    Aws::String m_versionId;

    RequestCharged m_requestCharged;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
