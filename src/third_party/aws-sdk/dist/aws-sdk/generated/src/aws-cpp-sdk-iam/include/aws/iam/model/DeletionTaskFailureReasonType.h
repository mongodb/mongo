/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/RoleUsageType.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{

  /**
   * <p>The reason that the service-linked role deletion failed.</p> <p>This data
   * type is used as a response element in the
   * <a>GetServiceLinkedRoleDeletionStatus</a> operation.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/DeletionTaskFailureReasonType">AWS
   * API Reference</a></p>
   */
  class DeletionTaskFailureReasonType
  {
  public:
    AWS_IAM_API DeletionTaskFailureReasonType();
    AWS_IAM_API DeletionTaskFailureReasonType(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API DeletionTaskFailureReasonType& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>A short description of the reason that the service-linked role deletion
     * failed.</p>
     */
    inline const Aws::String& GetReason() const{ return m_reason; }
    inline bool ReasonHasBeenSet() const { return m_reasonHasBeenSet; }
    inline void SetReason(const Aws::String& value) { m_reasonHasBeenSet = true; m_reason = value; }
    inline void SetReason(Aws::String&& value) { m_reasonHasBeenSet = true; m_reason = std::move(value); }
    inline void SetReason(const char* value) { m_reasonHasBeenSet = true; m_reason.assign(value); }
    inline DeletionTaskFailureReasonType& WithReason(const Aws::String& value) { SetReason(value); return *this;}
    inline DeletionTaskFailureReasonType& WithReason(Aws::String&& value) { SetReason(std::move(value)); return *this;}
    inline DeletionTaskFailureReasonType& WithReason(const char* value) { SetReason(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of objects that contains details about the service-linked role
     * deletion failure, if that information is returned by the service. If the
     * service-linked role has active sessions or if any resources that were used by
     * the role have not been deleted from the linked service, the role can't be
     * deleted. This parameter includes a list of the resources that are associated
     * with the role and the Region in which the resources are being used.</p>
     */
    inline const Aws::Vector<RoleUsageType>& GetRoleUsageList() const{ return m_roleUsageList; }
    inline bool RoleUsageListHasBeenSet() const { return m_roleUsageListHasBeenSet; }
    inline void SetRoleUsageList(const Aws::Vector<RoleUsageType>& value) { m_roleUsageListHasBeenSet = true; m_roleUsageList = value; }
    inline void SetRoleUsageList(Aws::Vector<RoleUsageType>&& value) { m_roleUsageListHasBeenSet = true; m_roleUsageList = std::move(value); }
    inline DeletionTaskFailureReasonType& WithRoleUsageList(const Aws::Vector<RoleUsageType>& value) { SetRoleUsageList(value); return *this;}
    inline DeletionTaskFailureReasonType& WithRoleUsageList(Aws::Vector<RoleUsageType>&& value) { SetRoleUsageList(std::move(value)); return *this;}
    inline DeletionTaskFailureReasonType& AddRoleUsageList(const RoleUsageType& value) { m_roleUsageListHasBeenSet = true; m_roleUsageList.push_back(value); return *this; }
    inline DeletionTaskFailureReasonType& AddRoleUsageList(RoleUsageType&& value) { m_roleUsageListHasBeenSet = true; m_roleUsageList.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_reason;
    bool m_reasonHasBeenSet = false;

    Aws::Vector<RoleUsageType> m_roleUsageList;
    bool m_roleUsageListHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
