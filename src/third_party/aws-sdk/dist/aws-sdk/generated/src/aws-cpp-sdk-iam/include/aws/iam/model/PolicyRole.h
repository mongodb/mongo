/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Contains information about a role that a managed policy is attached to.</p>
   * <p>This data type is used as a response element in the
   * <a>ListEntitiesForPolicy</a> operation. </p> <p>For more information about
   * managed policies, refer to <a
   * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/policies-managed-vs-inline.html">Managed
   * policies and inline policies</a> in the <i>IAM User Guide</i>. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PolicyRole">AWS API
   * Reference</a></p>
   */
  class PolicyRole
  {
  public:
    AWS_IAM_API PolicyRole();
    AWS_IAM_API PolicyRole(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PolicyRole& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name (friendly name, not ARN) identifying the role.</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline PolicyRole& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline PolicyRole& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline PolicyRole& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The stable and unique string identifying the role. For more information about
     * IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetRoleId() const{ return m_roleId; }
    inline bool RoleIdHasBeenSet() const { return m_roleIdHasBeenSet; }
    inline void SetRoleId(const Aws::String& value) { m_roleIdHasBeenSet = true; m_roleId = value; }
    inline void SetRoleId(Aws::String&& value) { m_roleIdHasBeenSet = true; m_roleId = std::move(value); }
    inline void SetRoleId(const char* value) { m_roleIdHasBeenSet = true; m_roleId.assign(value); }
    inline PolicyRole& WithRoleId(const Aws::String& value) { SetRoleId(value); return *this;}
    inline PolicyRole& WithRoleId(Aws::String&& value) { SetRoleId(std::move(value)); return *this;}
    inline PolicyRole& WithRoleId(const char* value) { SetRoleId(value); return *this;}
    ///@}
  private:

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;

    Aws::String m_roleId;
    bool m_roleIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
