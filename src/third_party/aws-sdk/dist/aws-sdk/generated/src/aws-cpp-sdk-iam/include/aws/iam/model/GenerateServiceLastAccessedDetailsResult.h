/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/ResponseMetadata.h>
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
namespace IAM
{
namespace Model
{
  class GenerateServiceLastAccessedDetailsResult
  {
  public:
    AWS_IAM_API GenerateServiceLastAccessedDetailsResult();
    AWS_IAM_API GenerateServiceLastAccessedDetailsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GenerateServiceLastAccessedDetailsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The <code>JobId</code> that you can use in the
     * <a>GetServiceLastAccessedDetails</a> or
     * <a>GetServiceLastAccessedDetailsWithEntities</a> operations. The
     * <code>JobId</code> returned by <code>GenerateServiceLastAccessedDetail</code>
     * must be used by the same role within a session, or by the same user when used to
     * call <code>GetServiceLastAccessedDetail</code>.</p>
     */
    inline const Aws::String& GetJobId() const{ return m_jobId; }
    inline void SetJobId(const Aws::String& value) { m_jobId = value; }
    inline void SetJobId(Aws::String&& value) { m_jobId = std::move(value); }
    inline void SetJobId(const char* value) { m_jobId.assign(value); }
    inline GenerateServiceLastAccessedDetailsResult& WithJobId(const Aws::String& value) { SetJobId(value); return *this;}
    inline GenerateServiceLastAccessedDetailsResult& WithJobId(Aws::String&& value) { SetJobId(std::move(value)); return *this;}
    inline GenerateServiceLastAccessedDetailsResult& WithJobId(const char* value) { SetJobId(value); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GenerateServiceLastAccessedDetailsResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GenerateServiceLastAccessedDetailsResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_jobId;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
