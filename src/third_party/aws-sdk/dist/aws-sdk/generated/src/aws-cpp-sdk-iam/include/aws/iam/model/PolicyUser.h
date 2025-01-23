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
   * <p>Contains information about a user that a managed policy is attached to.</p>
   * <p>This data type is used as a response element in the
   * <a>ListEntitiesForPolicy</a> operation. </p> <p>For more information about
   * managed policies, refer to <a
   * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/policies-managed-vs-inline.html">Managed
   * policies and inline policies</a> in the <i>IAM User Guide</i>. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PolicyUser">AWS API
   * Reference</a></p>
   */
  class PolicyUser
  {
  public:
    AWS_IAM_API PolicyUser();
    AWS_IAM_API PolicyUser(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PolicyUser& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name (friendly name, not ARN) identifying the user.</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline PolicyUser& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline PolicyUser& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline PolicyUser& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The stable and unique string identifying the user. For more information about
     * IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetUserId() const{ return m_userId; }
    inline bool UserIdHasBeenSet() const { return m_userIdHasBeenSet; }
    inline void SetUserId(const Aws::String& value) { m_userIdHasBeenSet = true; m_userId = value; }
    inline void SetUserId(Aws::String&& value) { m_userIdHasBeenSet = true; m_userId = std::move(value); }
    inline void SetUserId(const char* value) { m_userIdHasBeenSet = true; m_userId.assign(value); }
    inline PolicyUser& WithUserId(const Aws::String& value) { SetUserId(value); return *this;}
    inline PolicyUser& WithUserId(Aws::String&& value) { SetUserId(std::move(value)); return *this;}
    inline PolicyUser& WithUserId(const char* value) { SetUserId(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_userId;
    bool m_userIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
