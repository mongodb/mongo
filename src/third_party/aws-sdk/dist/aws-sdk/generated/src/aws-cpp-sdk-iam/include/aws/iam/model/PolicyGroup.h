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
   * <p>Contains information about a group that a managed policy is attached to.</p>
   * <p>This data type is used as a response element in the
   * <a>ListEntitiesForPolicy</a> operation. </p> <p>For more information about
   * managed policies, refer to <a
   * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/policies-managed-vs-inline.html">Managed
   * policies and inline policies</a> in the <i>IAM User Guide</i>. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PolicyGroup">AWS API
   * Reference</a></p>
   */
  class PolicyGroup
  {
  public:
    AWS_IAM_API PolicyGroup();
    AWS_IAM_API PolicyGroup(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PolicyGroup& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name (friendly name, not ARN) identifying the group.</p>
     */
    inline const Aws::String& GetGroupName() const{ return m_groupName; }
    inline bool GroupNameHasBeenSet() const { return m_groupNameHasBeenSet; }
    inline void SetGroupName(const Aws::String& value) { m_groupNameHasBeenSet = true; m_groupName = value; }
    inline void SetGroupName(Aws::String&& value) { m_groupNameHasBeenSet = true; m_groupName = std::move(value); }
    inline void SetGroupName(const char* value) { m_groupNameHasBeenSet = true; m_groupName.assign(value); }
    inline PolicyGroup& WithGroupName(const Aws::String& value) { SetGroupName(value); return *this;}
    inline PolicyGroup& WithGroupName(Aws::String&& value) { SetGroupName(std::move(value)); return *this;}
    inline PolicyGroup& WithGroupName(const char* value) { SetGroupName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The stable and unique string identifying the group. For more information
     * about IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetGroupId() const{ return m_groupId; }
    inline bool GroupIdHasBeenSet() const { return m_groupIdHasBeenSet; }
    inline void SetGroupId(const Aws::String& value) { m_groupIdHasBeenSet = true; m_groupId = value; }
    inline void SetGroupId(Aws::String&& value) { m_groupIdHasBeenSet = true; m_groupId = std::move(value); }
    inline void SetGroupId(const char* value) { m_groupIdHasBeenSet = true; m_groupId.assign(value); }
    inline PolicyGroup& WithGroupId(const Aws::String& value) { SetGroupId(value); return *this;}
    inline PolicyGroup& WithGroupId(Aws::String&& value) { SetGroupId(std::move(value)); return *this;}
    inline PolicyGroup& WithGroupId(const char* value) { SetGroupId(value); return *this;}
    ///@}
  private:

    Aws::String m_groupName;
    bool m_groupNameHasBeenSet = false;

    Aws::String m_groupId;
    bool m_groupIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
