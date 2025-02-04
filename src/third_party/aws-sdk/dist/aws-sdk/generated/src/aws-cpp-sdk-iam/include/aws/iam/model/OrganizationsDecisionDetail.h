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
   * <p>Contains information about the effect that Organizations has on a policy
   * simulation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/OrganizationsDecisionDetail">AWS
   * API Reference</a></p>
   */
  class OrganizationsDecisionDetail
  {
  public:
    AWS_IAM_API OrganizationsDecisionDetail();
    AWS_IAM_API OrganizationsDecisionDetail(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API OrganizationsDecisionDetail& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>Specifies whether the simulated operation is allowed by the Organizations
     * service control policies that impact the simulated user's account.</p>
     */
    inline bool GetAllowedByOrganizations() const{ return m_allowedByOrganizations; }
    inline bool AllowedByOrganizationsHasBeenSet() const { return m_allowedByOrganizationsHasBeenSet; }
    inline void SetAllowedByOrganizations(bool value) { m_allowedByOrganizationsHasBeenSet = true; m_allowedByOrganizations = value; }
    inline OrganizationsDecisionDetail& WithAllowedByOrganizations(bool value) { SetAllowedByOrganizations(value); return *this;}
    ///@}
  private:

    bool m_allowedByOrganizations;
    bool m_allowedByOrganizationsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
