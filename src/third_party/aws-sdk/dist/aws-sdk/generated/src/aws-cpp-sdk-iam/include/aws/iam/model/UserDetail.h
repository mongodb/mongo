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
   * <p>Contains information about an IAM user, including all the user's policies and
   * all the IAM groups the user is in.</p> <p>This data type is used as a response
   * element in the <a>GetAccountAuthorizationDetails</a> operation.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/UserDetail">AWS API
   * Reference</a></p>
   */
  class UserDetail
  {
  public:
    AWS_IAM_API UserDetail();
    AWS_IAM_API UserDetail(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API UserDetail& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The path to the user. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline UserDetail& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline UserDetail& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline UserDetail& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The friendly name identifying the user.</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline UserDetail& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline UserDetail& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline UserDetail& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The stable and unique string identifying the user. For more information about
     * IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetUserId() const{ return m_userId; }
    inline bool UserIdHasBeenSet() const { return m_userIdHasBeenSet; }
    inline void SetUserId(const Aws::String& value) { m_userIdHasBeenSet = true; m_userId = value; }
    inline void SetUserId(Aws::String&& value) { m_userIdHasBeenSet = true; m_userId = std::move(value); }
    inline void SetUserId(const char* value) { m_userIdHasBeenSet = true; m_userId.assign(value); }
    inline UserDetail& WithUserId(const Aws::String& value) { SetUserId(value); return *this;}
    inline UserDetail& WithUserId(Aws::String&& value) { SetUserId(std::move(value)); return *this;}
    inline UserDetail& WithUserId(const char* value) { SetUserId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline UserDetail& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline UserDetail& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline UserDetail& WithArn(const char* value) { SetArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when the user was created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline bool CreateDateHasBeenSet() const { return m_createDateHasBeenSet; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDateHasBeenSet = true; m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDateHasBeenSet = true; m_createDate = std::move(value); }
    inline UserDetail& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline UserDetail& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of the inline policies embedded in the user.</p>
     */
    inline const Aws::Vector<PolicyDetail>& GetUserPolicyList() const{ return m_userPolicyList; }
    inline bool UserPolicyListHasBeenSet() const { return m_userPolicyListHasBeenSet; }
    inline void SetUserPolicyList(const Aws::Vector<PolicyDetail>& value) { m_userPolicyListHasBeenSet = true; m_userPolicyList = value; }
    inline void SetUserPolicyList(Aws::Vector<PolicyDetail>&& value) { m_userPolicyListHasBeenSet = true; m_userPolicyList = std::move(value); }
    inline UserDetail& WithUserPolicyList(const Aws::Vector<PolicyDetail>& value) { SetUserPolicyList(value); return *this;}
    inline UserDetail& WithUserPolicyList(Aws::Vector<PolicyDetail>&& value) { SetUserPolicyList(std::move(value)); return *this;}
    inline UserDetail& AddUserPolicyList(const PolicyDetail& value) { m_userPolicyListHasBeenSet = true; m_userPolicyList.push_back(value); return *this; }
    inline UserDetail& AddUserPolicyList(PolicyDetail&& value) { m_userPolicyListHasBeenSet = true; m_userPolicyList.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of IAM groups that the user is in.</p>
     */
    inline const Aws::Vector<Aws::String>& GetGroupList() const{ return m_groupList; }
    inline bool GroupListHasBeenSet() const { return m_groupListHasBeenSet; }
    inline void SetGroupList(const Aws::Vector<Aws::String>& value) { m_groupListHasBeenSet = true; m_groupList = value; }
    inline void SetGroupList(Aws::Vector<Aws::String>&& value) { m_groupListHasBeenSet = true; m_groupList = std::move(value); }
    inline UserDetail& WithGroupList(const Aws::Vector<Aws::String>& value) { SetGroupList(value); return *this;}
    inline UserDetail& WithGroupList(Aws::Vector<Aws::String>&& value) { SetGroupList(std::move(value)); return *this;}
    inline UserDetail& AddGroupList(const Aws::String& value) { m_groupListHasBeenSet = true; m_groupList.push_back(value); return *this; }
    inline UserDetail& AddGroupList(Aws::String&& value) { m_groupListHasBeenSet = true; m_groupList.push_back(std::move(value)); return *this; }
    inline UserDetail& AddGroupList(const char* value) { m_groupListHasBeenSet = true; m_groupList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of the managed policies attached to the user.</p>
     */
    inline const Aws::Vector<AttachedPolicy>& GetAttachedManagedPolicies() const{ return m_attachedManagedPolicies; }
    inline bool AttachedManagedPoliciesHasBeenSet() const { return m_attachedManagedPoliciesHasBeenSet; }
    inline void SetAttachedManagedPolicies(const Aws::Vector<AttachedPolicy>& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies = value; }
    inline void SetAttachedManagedPolicies(Aws::Vector<AttachedPolicy>&& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies = std::move(value); }
    inline UserDetail& WithAttachedManagedPolicies(const Aws::Vector<AttachedPolicy>& value) { SetAttachedManagedPolicies(value); return *this;}
    inline UserDetail& WithAttachedManagedPolicies(Aws::Vector<AttachedPolicy>&& value) { SetAttachedManagedPolicies(std::move(value)); return *this;}
    inline UserDetail& AddAttachedManagedPolicies(const AttachedPolicy& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies.push_back(value); return *this; }
    inline UserDetail& AddAttachedManagedPolicies(AttachedPolicy&& value) { m_attachedManagedPoliciesHasBeenSet = true; m_attachedManagedPolicies.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The ARN of the policy used to set the permissions boundary for the user.</p>
     * <p>For more information about permissions boundaries, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_boundaries.html">Permissions
     * boundaries for IAM identities </a> in the <i>IAM User Guide</i>.</p>
     */
    inline const AttachedPermissionsBoundary& GetPermissionsBoundary() const{ return m_permissionsBoundary; }
    inline bool PermissionsBoundaryHasBeenSet() const { return m_permissionsBoundaryHasBeenSet; }
    inline void SetPermissionsBoundary(const AttachedPermissionsBoundary& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = value; }
    inline void SetPermissionsBoundary(AttachedPermissionsBoundary&& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = std::move(value); }
    inline UserDetail& WithPermissionsBoundary(const AttachedPermissionsBoundary& value) { SetPermissionsBoundary(value); return *this;}
    inline UserDetail& WithPermissionsBoundary(AttachedPermissionsBoundary&& value) { SetPermissionsBoundary(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are associated with the user. For more information about
     * tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline UserDetail& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline UserDetail& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline UserDetail& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline UserDetail& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_userId;
    bool m_userIdHasBeenSet = false;

    Aws::String m_arn;
    bool m_arnHasBeenSet = false;

    Aws::Utils::DateTime m_createDate;
    bool m_createDateHasBeenSet = false;

    Aws::Vector<PolicyDetail> m_userPolicyList;
    bool m_userPolicyListHasBeenSet = false;

    Aws::Vector<Aws::String> m_groupList;
    bool m_groupListHasBeenSet = false;

    Aws::Vector<AttachedPolicy> m_attachedManagedPolicies;
    bool m_attachedManagedPoliciesHasBeenSet = false;

    AttachedPermissionsBoundary m_permissionsBoundary;
    bool m_permissionsBoundaryHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
