/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/ReportStateType.h>
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
  /**
   * <p>Contains the response to a successful <a>GenerateCredentialReport</a>
   * request. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GenerateCredentialReportResponse">AWS
   * API Reference</a></p>
   */
  class GenerateCredentialReportResult
  {
  public:
    AWS_IAM_API GenerateCredentialReportResult();
    AWS_IAM_API GenerateCredentialReportResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GenerateCredentialReportResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Information about the state of the credential report.</p>
     */
    inline const ReportStateType& GetState() const{ return m_state; }
    inline void SetState(const ReportStateType& value) { m_state = value; }
    inline void SetState(ReportStateType&& value) { m_state = std::move(value); }
    inline GenerateCredentialReportResult& WithState(const ReportStateType& value) { SetState(value); return *this;}
    inline GenerateCredentialReportResult& WithState(ReportStateType&& value) { SetState(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Information about the credential report.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline void SetDescription(const Aws::String& value) { m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_description.assign(value); }
    inline GenerateCredentialReportResult& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline GenerateCredentialReportResult& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline GenerateCredentialReportResult& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GenerateCredentialReportResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GenerateCredentialReportResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    ReportStateType m_state;

    Aws::String m_description;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
