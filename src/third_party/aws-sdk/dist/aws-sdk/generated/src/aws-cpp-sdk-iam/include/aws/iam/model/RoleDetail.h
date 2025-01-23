/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/AttachedPermissionsBoundary.h>
#include <aws/iam/model/RoleLastUsed.h>
#include <aws/iam/model/InstanceProfile.h>
#include <aws/iam/model/PolicyDetail.h>
#include <aws/iam/model/AttachedPolicy.h>
#include <aws/iam/model/Tag.h>
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
   * <p>Contains information about an IAM role, including all of the role's
   * policies.</p> <p>This data type is used as a response element in the
   * <a>GetAccountAuthorizationDetails</a> operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/RoleDetail">AWS API
   * Reference</a></p>
   */
  class RoleDetail
  {
  public:
    AWS_IAM_API RoleDetail();
    AWS_IAM_API RoleDetail(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API RoleDetail& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The path to the role. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline RoleDetail& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline RoleDetail& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline RoleDetail& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The friendly name that identifies the role.</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline RoleDetail& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline RoleDetail& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline RoleDetail& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The stable and unique string identifying the role. For more information about
     * IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetRoleId() const{ return m_roleId; }
    inline bool RoleIdHasBeenSet() const { return m_roleIdHasBeenSet; }
    inline void SetRoleId(const Aws::String& value) { m_roleIdHasBeenSet = true; m_roleId = value; }
    inline void SetRoleId(Aws::String&& value) { m_roleIdHasBeenSet = true; m_roleId = std::move(value); }
    inline void SetRoleId(const char* value) { m_roleIdHasBeenSet = true; m_roleId.assign(value); }
    inline RoleDetail& WithRoleId(const Aws::String& value) { SetRoleId(value); return *this;}
    inline RoleDetail& WithRoleId(Aws::String&& value) { SetRoleId(std::move(value)); return *this;}
    inline RoleDetail& WithRoleId(const char* value) { SetRoleId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline RoleDetail& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline RoleDetail& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline RoleDetail& WithArn(const char* value) { SetArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when the role was created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline bool CreateDateHasBeenSet() const { return m_createDateHasBeenSet; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDateHasBeenSet = true; m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDateHasBeenSet = true; m_createDate = std::move(value); }
    inline RoleDetail& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline RoleDetail& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The trust policy that grants permission to assume the role.</p>
     */
    inline const Aws::String& GetAssumeRolePolicyDocument() const{ return m_assumeRolePolicyDocument; }
    inline bool AssumeRolePolicyDocumentHasBeenSet() const { return m_assumeRolePolicyDocumentHasBeenSet; }
    inline void SetAssumeRolePolicyDocument(const Aws::String& value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument = value; }
    inline void SetAssumeRolePolicyDocument(Aws::String&& value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument = std::move(value); }
    inline void SetAssumeRolePolicyDocument(const char* value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument.assign(value); }
    inline RoleDetail& WithAssumeRolePolicyDocument(const Aws::String& value) { SetAssumeRolePolicyDocument(value); return *this;}
    inline RoleDetail& WithAssumeRolePolicyDocument(Aws::String&& value) { SetAssumeRolePolicyDocument(std::move(value)); return *this;}
    inline RoleDetail& WithAssumeRolePolicyDocument(const char* value) { SetAssumeRolePolicyDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of instance profiles that contain this role.</p>
     */
    inline const Aws::Vector<InstanceProfile>& GetInstanceProfileList() const{ return m_instanceProfileList; }
    inline bool InstanceProfileListHasBeenSet() const { return m_instanceProfileListHasBeenSet; }
    inline void SetInstanceProfileList(const Aws::Vector<InstanceProfile>& value) { m_instanceProfileListHasBeenSet = true; m_instanceProfileList = value; }
    inline void SetInstanceProfileList(Aws::Vector<InstanceProfile>&& value) { m_instanceProfileListHasBeenSet = true; m_instanceProfileList = std::move(value); }
    inline RoleDetail& WithInstanceProfileList(const Aws::Vector<InstanceProfile>& value) { SetInstanceProfileList(value); return *this;}
    inline RoleDetail& WithInstanceProfileList(Aws::Vector<InstanceProfile>&& value) { SetInstanceProfileList(std::move(value)); return *this;}
    inline RoleDetail& AddInstanceProfileList(const InstanceProfile& value) { m_instanceProfileListHasBeenSet = true; m_instanceProfileList.push_back(value); return *this; }
    inline RoleDetail& AddInstanceProfileList(InstanceProfile&& value) { m_instanceProfileListHasBeenSet = true; m_instanceProfileList.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of inline policies embedded in the role. These policies are the role's
     * access (permissions) policies.</p>
     */
    inline const Aws::Vector<PolicyDetail>& GetRolePolicyList() const{ return m_rolePolicyList; }
    inline bool RolePolicyListHasBeenSet() const { return m_rolePolicyListHasBeenSet; }
    inline void SetRolePolicyList(const Aws::Vector<PolicyDetail>& value) { m_rolePolicyListHasBeenSet = true; m_rolePolicyList = value; }
    inline void SetRolePolicyList(Aws::Vector<PolicyDetail>&& value) { m_rolePolicyListHasBeenSet = true; m_rolePolicyList = std::move(value); }
    inline RoleDetail& WithRolePolicyList(const Aws::Vector<PolicyDetail>& value) { SetRolePolicyList(value); return *this;}
    inline RoleDetail& WithRolePolicyList(Aws::Vector<PolicyDetail>&& value) { SetRolePolicyList(std::move(value)); return *this;}
    inline RoleDetail& AddRolePolicyList(const PolicyDetail& value) { m_rolePolicyListHasBeenSet = true; m_rolePolicyList.push_back(value); return *this; }
    inline RoleDetail& AddRolePolicyList(PolicyDetail&& value) { m_rolePolicyListHasBeenSet = true; m_rolePolicyList.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of managed policies attached to the role. These policies are the
     * role's access (permissions) policies.</p>
     */
    inline const Aws::Vector<AttachedPolicy>& GetAttachedManagedPolicies() const{ return m_attachedManagedPolicies; }
    inline bool AttachedManagedPoliciesHasBeenSet() const { return m_attachedManagedPoliciesHasBeenSet; }
    inline void SetAttachedManagedPolicies(const Aws::Vector<AttachedPolicy>& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies = value; }
    inline void SetAttachedManagedPolicies(Aws::Vector<AttachedPolicy>&& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies = std::move(value); }
    inline RoleDetail& WithAttachedManagedPolicies(const Aws::Vector<AttachedPolicy>& value) { SetAttachedManagedPolicies(value); return *this;}
    inline RoleDetail& WithAttachedManagedPolicies(Aws::Vector<AttachedPolicy>&& value) { SetAttachedManagedPolicies(std::move(value)); return *this;}
    inline RoleDetail& AddAttachedManagedPolicies(const AttachedPolicy& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies.push_back(value); return *this; }
    inline RoleDetail& AddAttachedManagedPolicies(AttachedPolicy&& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The ARN of the policy used to set the permissions boundary for the role.</p>
     * <p>For more information about permissions boundaries, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_boundaries.html">Permissions
     * boundaries for IAM identities </a> in the <i>IAM User Guide</i>.</p>
     */
    inline const AttachedPermissionsBoundary& GetPermissionsBoundary() const{ return m_permissionsBoundary; }
    inline bool PermissionsBoundaryHasBeenSet() const { return m_permissionsBoundaryHasBeenSet; }
    inline void SetPermissionsBoundary(const AttachedPermissionsBoundary& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = value; }
    inline void SetPermissionsBoundary(AttachedPermissionsBoundary&& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = std::move(value); }
    inline RoleDetail& WithPermissionsBoundary(const AttachedPermissionsBoundary& value) { SetPermissionsBoundary(value); return *this;}
    inline RoleDetail& WithPermissionsBoundary(AttachedPermissionsBoundary&& value) { SetPermissionsBoundary(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the role. For more information about
     * tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline RoleDetail& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline RoleDetail& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline RoleDetail& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline RoleDetail& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Contains information about the last time that an IAM role was used. This
     * includes the date and time and the Region in which the role was last used.
     * Activity is only reported for the trailing 400 days. This period can be shorter
     * if your Region began supporting these features within the last year. The role
     * might have been used more than 400 days ago. For more information, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#access-advisor_tracking-period">Regions
     * where data is tracked</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const RoleLastUsed& GetRoleLastUsed() const{ return m_roleLastUsed; }
    inline bool RoleLastUsedHasBeenSet() const { return m_roleLastUsedHasBeenSet; }
    inline void SetRoleLastUsed(const RoleLastUsed& value) { m_roleLastUsedHasBeenSet = true; m_roleLastUsed = value; }
    inline void SetRoleLastUsed(RoleLastUsed&& value) { m_roleLastUsedHasBeenSet = true; m_roleLastUsed = std::move(value); }
    inline RoleDetail& WithRoleLastUsed(const RoleLastUsed& value) { SetRoleLastUsed(value); return *this;}
    inline RoleDetail& WithRoleLastUsed(RoleLastUsed&& value) { SetRoleLastUsed(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;

    Aws::String m_roleId;
    bool m_roleIdHasBeenSet = false;

    Aws::String m_arn;
    bool m_arnHasBeenSet = false;

    Aws::Utils::DateTime m_createDate;
    bool m_createDateHasBeenSet = false;

    Aws::String m_assumeRolePolicyDocument;
    bool m_assumeRolePolicyDocumentHasBeenSet = false;

    Aws::Vector<InstanceProfile> m_instanceProfileList;
    bool m_instanceProfileListHasBeenSet = false;

    Aws::Vector<PolicyDetail> m_rolePolicyList;
    bool m_rolePolicyListHasBeenSet = false;

    Aws::Vector<AttachedPolicy> m_attachedManagedPolicies;
    bool m_attachedManagedPoliciesHasBeenSet = false;

    AttachedPermissionsBoundary m_permissionsBoundary;
    bool m_permissionsBoundaryHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;

    RoleLastUsed m_roleLastUsed;
    bool m_roleLastUsedHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
