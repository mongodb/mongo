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
   * <p>Contains information about an IAM user entity.</p> <p>This data type is used
   * as a response element in the following operations:</p> <ul> <li> <p>
   * <a>CreateUser</a> </p> </li> <li> <p> <a>GetUser</a> </p> </li> <li> <p>
   * <a>ListUsers</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/User">AWS API
   * Reference</a></p>
   */
  class User
  {
  public:
    AWS_IAM_API User();
    AWS_IAM_API User(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API User& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The path to the user. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p> <p>The ARN of the policy used
     * to set the permissions boundary for the user.</p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline User& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline User& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline User& WithPath(const char* value) { SetPath(value); return *this;}
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
    inline User& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline User& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline User& WithUserName(const char* value) { SetUserName(value); return *this;}
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
    inline User& WithUserId(const Aws::String& value) { SetUserId(value); return *this;}
    inline User& WithUserId(Aws::String&& value) { SetUserId(std::move(value)); return *this;}
    inline User& WithUserId(const char* value) { SetUserId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) that identifies the user. For more information
     * about ARNs and how to use ARNs in policies, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * Identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline User& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline User& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline User& WithArn(const char* value) { SetArn(value); return *this;}
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
    inline User& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline User& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when the user's password was last used to sign in to an
     * Amazon Web Services website. For a list of Amazon Web Services websites that
     * capture a user's last sign-in time, see the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/credential-reports.html">Credential
     * reports</a> topic in the <i>IAM User Guide</i>. If a password is used more than
     * once in a five-minute span, only the first use is returned in this field. If the
     * field is null (no value), then it indicates that they never signed in with a
     * password. This can be because:</p> <ul> <li> <p>The user never had a
     * password.</p> </li> <li> <p>A password exists but has not been used since IAM
     * started tracking this information on October 20, 2014.</p> </li> </ul> <p>A null
     * value does not mean that the user <i>never</i> had a password. Also, if the user
     * does not currently have a password but had one in the past, then this field
     * contains the date and time the most recent password was used.</p> <p>This value
     * is returned only in the <a>GetUser</a> and <a>ListUsers</a> operations. </p>
     */
    inline const Aws::Utils::DateTime& GetPasswordLastUsed() const{ return m_passwordLastUsed; }
    inline bool PasswordLastUsedHasBeenSet() const { return m_passwordLastUsedHasBeenSet; }
    inline void SetPasswordLastUsed(const Aws::Utils::DateTime& value) { m_passwordLastUsedHasBeenSet = true; m_passwordLastUsed = value; }
    inline void SetPasswordLastUsed(Aws::Utils::DateTime&& value) { m_passwordLastUsedHasBeenSet = true; m_passwordLastUsed = std::move(value); }
    inline User& WithPasswordLastUsed(const Aws::Utils::DateTime& value) { SetPasswordLastUsed(value); return *this;}
    inline User& WithPasswordLastUsed(Aws::Utils::DateTime&& value) { SetPasswordLastUsed(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>For more information about permissions boundaries, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_boundaries.html">Permissions
     * boundaries for IAM identities </a> in the <i>IAM User Guide</i>.</p>
     */
    inline const AttachedPermissionsBoundary& GetPermissionsBoundary() const{ return m_permissionsBoundary; }
    inline bool PermissionsBoundaryHasBeenSet() const { return m_permissionsBoundaryHasBeenSet; }
    inline void SetPermissionsBoundary(const AttachedPermissionsBoundary& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = value; }
    inline void SetPermissionsBoundary(AttachedPermissionsBoundary&& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = std::move(value); }
    inline User& WithPermissionsBoundary(const AttachedPermissionsBoundary& value) { SetPermissionsBoundary(value); return *this;}
    inline User& WithPermissionsBoundary(AttachedPermissionsBoundary&& value) { SetPermissionsBoundary(std::move(value)); return *this;}
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
    inline User& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline User& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline User& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline User& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
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

    Aws::Utils::DateTime m_passwordLastUsed;
    bool m_passwordLastUsedHasBeenSet = false;

    AttachedPermissionsBoundary m_permissionsBoundary;
    bool m_permissionsBoundaryHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
