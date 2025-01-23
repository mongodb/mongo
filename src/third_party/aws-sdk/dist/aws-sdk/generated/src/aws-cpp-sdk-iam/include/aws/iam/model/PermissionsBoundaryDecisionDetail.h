/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

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
   * <p>Contains information about the effect that a permissions boundary has on a
   * policy simulation when the boundary is applied to an IAM entity.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PermissionsBoundaryDecisionDetail">AWS
   * API Reference</a></p>
   */
  class PermissionsBoundaryDecisionDetail
  {
  public:
    AWS_IAM_API PermissionsBoundaryDecisionDetail();
    AWS_IAM_API PermissionsBoundaryDecisionDetail(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PermissionsBoundaryDecisionDetail& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>Specifies whether an action is allowed by a permissions boundary that is
     * applied to an IAM entity (user or role). A value of <code>true</code> means that
     * the permissions boundary does not deny the action. This means that the policy
     * includes an <code>Allow</code> statement that matches the request. In this case,
     * if an identity-based policy also allows the action, the request is allowed. A
     * value of <code>false</code> means that either the requested action is not
     * allowed (implicitly denied) or that the action is explicitly denied by the
     * permissions boundary. In both of these cases, the action is not allowed,
     * regardless of the identity-based policy.</p>
     */
    inline bool GetAllowedByPermissionsBoundary() const{ return m_allowedByPermissionsBoundary; }
    inline bool AllowedByPermissionsBoundaryHasBeenSet() const { return m_allowedByPermissionsBoundaryHasBeenSet; }
    inline void SetAllowedByPermissionsBoundary(bool value) { m_allowedByPermissionsBoundaryHasBeenSet = true; m_allowedByPermissionsBoundary = value; }
    inline PermissionsBoundaryDecisionDetail& WithAllowedByPermissionsBoundary(bool value) { SetAllowedByPermissionsBoundary(value); return *this;}
    ///@}
  private:

    bool m_allowedByPermissionsBoundary;
    bool m_allowedByPermissionsBoundaryHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
