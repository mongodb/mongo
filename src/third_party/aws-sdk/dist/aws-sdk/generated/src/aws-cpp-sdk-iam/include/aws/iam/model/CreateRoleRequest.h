/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class CreateRoleRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreateRoleRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateRole"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p> The path to the role. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * Identifiers</a> in the <i>IAM User Guide</i>.</p> <p>This parameter is optional.
     * If it is not included, it defaults to a slash (/).</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of either a forward slash (/) by itself or a
     * string that must begin and end with forward slashes. In addition, it can contain
     * any ASCII character from the ! (<code>\u0021</code>) through the DEL character
     * (<code>\u007F</code>), including most punctuation characters, digits, and upper
     * and lowercased letters.</p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline CreateRoleRequest& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline CreateRoleRequest& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline CreateRoleRequest& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the role to create.</p> <p>IAM user, group, role, and policy
     * names must be unique within the account. Names are not distinguished by case.
     * For example, you cannot create resources named both "MyResource" and
     * "myresource".</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline CreateRoleRequest& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline CreateRoleRequest& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline CreateRoleRequest& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The trust relationship policy document that grants an entity permission to
     * assume the role.</p> <p>In IAM, you must provide a JSON policy that has been
     * converted to a string. However, for CloudFormation templates formatted in YAML,
     * you can provide the policy in JSON or YAML format. CloudFormation always
     * converts a YAML policy to JSON format before submitting it to IAM.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul> <p> Upon success, the
     * response includes the same trust policy in JSON format.</p>
     */
    inline const Aws::String& GetAssumeRolePolicyDocument() const{ return m_assumeRolePolicyDocument; }
    inline bool AssumeRolePolicyDocumentHasBeenSet() const { return m_assumeRolePolicyDocumentHasBeenSet; }
    inline void SetAssumeRolePolicyDocument(const Aws::String& value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument = value; }
    inline void SetAssumeRolePolicyDocument(Aws::String&& value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument = std::move(value); }
    inline void SetAssumeRolePolicyDocument(const char* value) { m_assumeRolePolicyDocumentHasBeenSet = true; m_assumeRolePolicyDocument.assign(value); }
    inline CreateRoleRequest& WithAssumeRolePolicyDocument(const Aws::String& value) { SetAssumeRolePolicyDocument(value); return *this;}
    inline CreateRoleRequest& WithAssumeRolePolicyDocument(Aws::String&& value) { SetAssumeRolePolicyDocument(std::move(value)); return *this;}
    inline CreateRoleRequest& WithAssumeRolePolicyDocument(const char* value) { SetAssumeRolePolicyDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A description of the role.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline CreateRoleRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CreateRoleRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CreateRoleRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum session duration (in seconds) that you want to set for the
     * specified role. If you do not specify a value for this setting, the default
     * value of one hour is applied. This setting can have a value from 1 hour to 12
     * hours.</p> <p>Anyone who assumes the role from the CLI or API can use the
     * <code>DurationSeconds</code> API parameter or the <code>duration-seconds</code>
     * CLI parameter to request a longer session. The <code>MaxSessionDuration</code>
     * setting determines the maximum duration that can be requested using the
     * <code>DurationSeconds</code> parameter. If users don't specify a value for the
     * <code>DurationSeconds</code> parameter, their security credentials are valid for
     * one hour by default. This applies when you use the <code>AssumeRole*</code> API
     * operations or the <code>assume-role*</code> CLI operations but does not apply
     * when you use those operations to create a console URL. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_roles_use.html">Using
     * IAM roles</a> in the <i>IAM User Guide</i>.</p>
     */
    inline int GetMaxSessionDuration() const{ return m_maxSessionDuration; }
    inline bool MaxSessionDurationHasBeenSet() const { return m_maxSessionDurationHasBeenSet; }
    inline void SetMaxSessionDuration(int value) { m_maxSessionDurationHasBeenSet = true; m_maxSessionDuration = value; }
    inline CreateRoleRequest& WithMaxSessionDuration(int value) { SetMaxSessionDuration(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the managed policy that is used to set the permissions boundary
     * for the role.</p> <p>A permissions boundary policy defines the maximum
     * permissions that identity-based policies can grant to an entity, but does not
     * grant permissions. Permissions boundaries do not define the maximum permissions
     * that a resource-based policy can grant to an entity. To learn more, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_boundaries.html">Permissions
     * boundaries for IAM entities</a> in the <i>IAM User Guide</i>.</p> <p>For more
     * information about policy types, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies.html#access_policy-types">Policy
     * types </a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetPermissionsBoundary() const{ return m_permissionsBoundary; }
    inline bool PermissionsBoundaryHasBeenSet() const { return m_permissionsBoundaryHasBeenSet; }
    inline void SetPermissionsBoundary(const Aws::String& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = value; }
    inline void SetPermissionsBoundary(Aws::String&& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = std::move(value); }
    inline void SetPermissionsBoundary(const char* value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary.assign(value); }
    inline CreateRoleRequest& WithPermissionsBoundary(const Aws::String& value) { SetPermissionsBoundary(value); return *this;}
    inline CreateRoleRequest& WithPermissionsBoundary(Aws::String&& value) { SetPermissionsBoundary(std::move(value)); return *this;}
    inline CreateRoleRequest& WithPermissionsBoundary(const char* value) { SetPermissionsBoundary(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that you want to attach to the new role. Each tag consists of
     * a key name and an associated value. For more information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>  <p>If any one of the tags
     * is invalid or if you exceed the allowed maximum number of tags, then the entire
     * request fails and the resource is not created.</p> 
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline CreateRoleRequest& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline CreateRoleRequest& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateRoleRequest& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline CreateRoleRequest& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;

    Aws::String m_assumeRolePolicyDocument;
    bool m_assumeRolePolicyDocumentHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    int m_maxSessionDuration;
    bool m_maxSessionDurationHasBeenSet = false;

    Aws::String m_permissionsBoundary;
    bool m_permissionsBoundaryHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
