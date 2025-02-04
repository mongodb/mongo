/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/RequestCharged.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/DeletedObject.h>
#include <aws/s3/model/Error.h>
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
  class DeleteObjectsResult
  {
  public:
    AWS_S3_API DeleteObjectsResult();
    AWS_S3_API DeleteObjectsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API DeleteObjectsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Container element for a successful delete. It identifies the object that was
     * successfully deleted.</p>
     */
    inline const Aws::Vector<DeletedObject>& GetDeleted() const{ return m_deleted; }
    inline void SetDeleted(const Aws::Vector<DeletedObject>& value) { m_deleted = value; }
    inline void SetDeleted(Aws::Vector<DeletedObject>&& value) { m_deleted = std::move(value); }
    inline DeleteObjectsResult& WithDeleted(const Aws::Vector<DeletedObject>& value) { SetDeleted(value); return *this;}
    inline DeleteObjectsResult& WithDeleted(Aws::Vector<DeletedObject>&& value) { SetDeleted(std::move(value)); return *this;}
    inline DeleteObjectsResult& AddDeleted(const DeletedObject& value) { m_deleted.push_back(value); return *this; }
    inline DeleteObjectsResult& AddDeleted(DeletedObject&& value) { m_deleted.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline DeleteObjectsResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline DeleteObjectsResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for a failed delete action that describes the object that Amazon S3
     * attempted to delete and the error it encountered.</p>
     */
    inline const Aws::Vector<Error>& GetErrors() const{ return m_errors; }
    inline void SetErrors(const Aws::Vector<Error>& value) { m_errors = value; }
    inline void SetErrors(Aws::Vector<Error>&& value) { m_errors = std::move(value); }
    inline DeleteObjectsResult& WithErrors(const Aws::Vector<Error>& value) { SetErrors(value); return *this;}
    inline DeleteObjectsResult& WithErrors(Aws::Vector<Error>&& value) { SetErrors(std::move(value)); return *this;}
    inline DeleteObjectsResult& AddErrors(const Error& value) { m_errors.push_back(value); return *this; }
    inline DeleteObjectsResult& AddErrors(Error&& value) { m_errors.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DeleteObjectsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DeleteObjectsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DeleteObjectsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<DeletedObject> m_deleted;

    RequestCharged m_requestCharged;

    Aws::Vector<Error> m_errors;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
