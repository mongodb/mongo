/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/iam/model/AttachedPermissionsBoundary.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/RoleLastUsed.h>
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
   * <p>Contains information about an IAM role. This structure is returned as a
   * response element in several API operations that interact with
   * roles.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/Role">AWS API
   * Reference</a></p>
   */
  class Role
  {
  public:
    AWS_IAM_API Role();
    AWS_IAM_API Role(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API Role& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p> The path to the role. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline Role& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline Role& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline Role& WithPath(const char* value) { SetPath(value); return *this;}
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
    inline Role& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline Role& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline Role& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The stable and unique string identifying the role. For more information
     * about IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetRoleId() const{ return m_roleId; }
    inline bool RoleIdHasBeenSet() const { return m_roleIdHasBeenSet; }
    inline void SetRoleId(const Aws::String& value) { m_roleIdHasBeenSet = true; m_roleId = value; }
    inline void SetRoleId(Aws::String&& value) { m_roleIdHasBeenSet = true; m_roleId = std::move(value); }
    inline void SetRoleId(const char* value) { m_roleIdHasBeenSet = true; m_roleId.assign(value); }
    inline Role& WithRoleId(const Aws::String& value) { SetRoleId(value); return *this;}
    inline Role& WithRoleId(Aws::String&& value) { SetRoleId(std::move(value)); return *this;}
    inline Role& WithRoleId(const char* value) { SetRoleId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The Amazon Resource Name (ARN) specifying the role. For more information
     * about ARNs and how to use them in policies, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i> guide. </p>
     */
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline Role& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline Role& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline Role& WithArn(const char* value) { SetArn(value); return *this;}
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
    inline Role& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline Role& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The policy that grants an entity permission to assume the role.</p>
     */
    inline const Aws::String& GetAssumeRolePolicyDocument() const{ return m_assumeRolePolicyDocument; }
    inline bool AssumeRolePolicyDocumentHasBeenSet() const { return m_assumeRolePolicyDocumentHasBeenSet; }
    inline void SetAssumeRolePolicyDocument(const Aws::String& value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument = value; }
    inline void SetAssumeRolePolicyDocument(Aws::String&& value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument = std::move(value); }
    inline void SetAssumeRolePolicyDocument(const char* value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument.assign(value); }
    inline Role& WithAssumeRolePolicyDocument(const Aws::String& value) { SetAssumeRolePolicyDocument(value); return *this;}
    inline Role& WithAssumeRolePolicyDocument(Aws::String&& value) { SetAssumeRolePolicyDocument(std::move(value)); return *this;}
    inline Role& WithAssumeRolePolicyDocument(const char* value) { SetAssumeRolePolicyDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A description of the role that you provide.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline Role& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline Role& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline Role& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum session duration (in seconds) for the specified role. Anyone who
     * uses the CLI, or API to assume the role can specify the duration using the
     * optional <code>DurationSeconds</code> API parameter or
     * <code>duration-seconds</code> CLI parameter.</p>
     */
    inline int GetMaxSessionDuration() const{ return m_maxSessionDuration; }
    inline bool MaxSessionDurationHasBeenSet() const { return m_maxSessionDurationHasBeenSet; }
    inline void SetMaxSessionDuration(int value) { m_maxSessionDurationHasBeenSet = true; m_maxSessionDuration = value; }
    inline Role& WithMaxSessionDuration(int value) { SetMaxSessionDuration(value); return *this;}
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
    inline Role& WithPermissionsBoundary(const AttachedPermissionsBoundary& value) { SetPermissionsBoundary(value); return *this;}
    inline Role& WithPermissionsBoundary(AttachedPermissionsBoundary&& value) { SetPermissionsBoundary(std::move(value)); return *this;}
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
    inline Role& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline Role& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline Role& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline Role& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Contains information about the last time that an IAM role was used. This
     * includes the date and time and the Region in which the role was last used.
     * Activity is only reported for the trailing 400 days. This period can be shorter
     * if your Region began supporting these features within the last year. The role
     * might have been used more than 400 days ago. For more information, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#access-advisor_tracking-period">Regions
     * where data is tracked</a> in the <i>IAM user Guide</i>.</p>
     */
    inline const RoleLastUsed& GetRoleLastUsed() const{ return m_roleLastUsed; }
    inline bool RoleLastUsedHasBeenSet() const { return m_roleLastUsedHasBeenSet; }
    inline void SetRoleLastUsed(const RoleLastUsed& value) { m_roleLastUsedHasBeenSet = true; m_roleLastUsed = value; }
    inline void SetRoleLastUsed(RoleLastUsed&& value) { m_roleLastUsedHasBeenSet = true; m_roleLastUsed = std::move(value); }
    inline Role& WithRoleLastUsed(const RoleLastUsed& value) { SetRoleLastUsed(value); return *this;}
    inline Role& WithRoleLastUsed(RoleLastUsed&& value) { SetRoleLastUsed(std::move(value)); return *this;}
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

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    int m_maxSessionDuration;
    bool m_maxSessionDurationHasBeenSet = false;

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
