/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/DeletionTaskStatusType.h>
#include <aws/iam/model/DeletionTaskFailureReasonType.h>
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
  class GetServiceLinkedRoleDeletionStatusResult
  {
  public:
    AWS_IAM_API GetServiceLinkedRoleDeletionStatusResult();
    AWS_IAM_API GetServiceLinkedRoleDeletionStatusResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetServiceLinkedRoleDeletionStatusResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The status of the deletion.</p>
     */
    inline const DeletionTaskStatusType& GetStatus() const{ return m_status; }
    inline void SetStatus(const DeletionTaskStatusType& value) { m_status = value; }
    inline void SetStatus(DeletionTaskStatusType&& value) { m_status = std::move(value); }
    inline GetServiceLinkedRoleDeletionStatusResult& WithStatus(const DeletionTaskStatusType& value) { SetStatus(value); return *this;}
    inline GetServiceLinkedRoleDeletionStatusResult& WithStatus(DeletionTaskStatusType&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>An object that contains details about the reason the deletion failed.</p>
     */
    inline const DeletionTaskFailureReasonType& GetReason() const{ return m_reason; }
    inline void SetReason(const DeletionTaskFailureReasonType& value) { m_reason = value; }
    inline void SetReason(DeletionTaskFailureReasonType&& value) { m_reason = std::move(value); }
    inline GetServiceLinkedRoleDeletionStatusResult& WithReason(const DeletionTaskFailureReasonType& value) { SetReason(value); return *this;}
    inline GetServiceLinkedRoleDeletionStatusResult& WithReason(DeletionTaskFailureReasonType&& value) { SetReason(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetServiceLinkedRoleDeletionStatusResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetServiceLinkedRoleDeletionStatusResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    DeletionTaskStatusType m_status;

    DeletionTaskFailureReasonType m_reason;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
