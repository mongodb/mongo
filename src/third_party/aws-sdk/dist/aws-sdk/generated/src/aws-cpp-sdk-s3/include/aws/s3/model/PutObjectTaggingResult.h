/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
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
  class PutObjectTaggingResult
  {
  public:
    AWS_S3_API PutObjectTaggingResult();
    AWS_S3_API PutObjectTaggingResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API PutObjectTaggingResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The versionId of the object the tag-set was added to.</p>
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline void SetVersionId(const Aws::String& value) { m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionId.assign(value); }
    inline PutObjectTaggingResult& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline PutObjectTaggingResult& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline PutObjectTaggingResult& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutObjectTaggingResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutObjectTaggingResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutObjectTaggingResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_versionId;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
