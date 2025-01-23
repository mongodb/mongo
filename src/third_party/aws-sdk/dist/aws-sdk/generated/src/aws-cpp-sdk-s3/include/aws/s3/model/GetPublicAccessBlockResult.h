/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/PublicAccessBlockConfiguration.h>
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
  class GetPublicAccessBlockResult
  {
  public:
    AWS_S3_API GetPublicAccessBlockResult();
    AWS_S3_API GetPublicAccessBlockResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetPublicAccessBlockResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The <code>PublicAccessBlock</code> configuration currently in effect for this
     * Amazon S3 bucket.</p>
     */
    inline const PublicAccessBlockConfiguration& GetPublicAccessBlockConfiguration() const{ return m_publicAccessBlockConfiguration; }
    inline void SetPublicAccessBlockConfiguration(const PublicAccessBlockConfiguration& value) { m_publicAccessBlockConfiguration = value; }
    inline void SetPublicAccessBlockConfiguration(PublicAccessBlockConfiguration&& value) { m_publicAccessBlockConfiguration = std::move(value); }
    inline GetPublicAccessBlockResult& WithPublicAccessBlockConfiguration(const PublicAccessBlockConfiguration& value) { SetPublicAccessBlockConfiguration(value); return *this;}
    inline GetPublicAccessBlockResult& WithPublicAccessBlockConfiguration(PublicAccessBlockConfiguration&& value) { SetPublicAccessBlockConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetPublicAccessBlockResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetPublicAccessBlockResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetPublicAccessBlockResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    PublicAccessBlockConfiguration m_publicAccessBlockConfiguration;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
