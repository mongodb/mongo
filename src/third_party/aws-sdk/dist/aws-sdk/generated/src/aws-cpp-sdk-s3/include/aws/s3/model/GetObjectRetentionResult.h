/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ObjectLockRetention.h>
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
  class GetObjectRetentionResult
  {
  public:
    AWS_S3_API GetObjectRetentionResult();
    AWS_S3_API GetObjectRetentionResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetObjectRetentionResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The container element for an object's retention settings.</p>
     */
    inline const ObjectLockRetention& GetRetention() const{ return m_retention; }
    inline void SetRetention(const ObjectLockRetention& value) { m_retention = value; }
    inline void SetRetention(ObjectLockRetention&& value) { m_retention = std::move(value); }
    inline GetObjectRetentionResult& WithRetention(const ObjectLockRetention& value) { SetRetention(value); return *this;}
    inline GetObjectRetentionResult& WithRetention(ObjectLockRetention&& value) { SetRetention(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetObjectRetentionResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetObjectRetentionResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetObjectRetentionResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    ObjectLockRetention m_retention;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
