/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class GenerateOrganizationsAccessReportRequest : public IAMRequest
  {
  public:
    AWS_IAM_API GenerateOrganizationsAccessReportRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GenerateOrganizationsAccessReport"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The path of the Organizations entity (root, OU, or account). You can build an
     * entity path using the known structure of your organization. For example, assume
     * that your account ID is <code>123456789012</code> and its parent OU ID is
     * <code>ou-rge0-awsabcde</code>. The organization root ID is
     * <code>r-f6g7h8i9j0example</code> and your organization ID is
     * <code>o-a1b2c3d4e5</code>. Your entity path is
     * <code>o-a1b2c3d4e5/r-f6g7h8i9j0example/ou-rge0-awsabcde/123456789012</code>.</p>
     */
    inline const Aws::String& GetEntityPath() const{ return m_entityPath; }
    inline bool EntityPathHasBeenSet() const { return m_entityPathHasBeenSet; }
    inline void SetEntityPath(const Aws::String& value) { m_entityPathHasBeenSet = true; m_entityPath = value; }
    inline void SetEntityPath(Aws::String&& value) { m_entityPathHasBeenSet = true; m_entityPath = std::move(value); }
    inline void SetEntityPath(const char* value) { m_entityPathHasBeenSet = true; m_entityPath.assign(value); }
    inline GenerateOrganizationsAccessReportRequest& WithEntityPath(const Aws::String& value) { SetEntityPath(value); return *this;}
    inline GenerateOrganizationsAccessReportRequest& WithEntityPath(Aws::String&& value) { SetEntityPath(std::move(value)); return *this;}
    inline GenerateOrganizationsAccessReportRequest& WithEntityPath(const char* value) { SetEntityPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The identifier of the Organizations service control policy (SCP). This
     * parameter is optional.</p> <p>This ID is used to generate information about when
     * an account principal that is limited by the SCP attempted to access an Amazon
     * Web Services service.</p>
     */
    inline const Aws::String& GetOrganizationsPolicyId() const{ return m_organizationsPolicyId; }
    inline bool OrganizationsPolicyIdHasBeenSet() const { return m_organizationsPolicyIdHasBeenSet; }
    inline void SetOrganizationsPolicyId(const Aws::String& value) { m_organizationsPolicyIdHasBeenSet = true; m_organizationsPolicyId = value; }
    inline void SetOrganizationsPolicyId(Aws::String&& value) { m_organizationsPolicyIdHasBeenSet = true; m_organizationsPolicyId = std::move(value); }
    inline void SetOrganizationsPolicyId(const char* value) { m_organizationsPolicyIdHasBeenSet = true; m_organizationsPolicyId.assign(value); }
    inline GenerateOrganizationsAccessReportRequest& WithOrganizationsPolicyId(const Aws::String& value) { SetOrganizationsPolicyId(value); return *this;}
    inline GenerateOrganizationsAccessReportRequest& WithOrganizationsPolicyId(Aws::String&& value) { SetOrganizationsPolicyId(std::move(value)); return *this;}
    inline GenerateOrganizationsAccessReportRequest& WithOrganizationsPolicyId(const char* value) { SetOrganizationsPolicyId(value); return *this;}
    ///@}
  private:

    Aws::String m_entityPath;
    bool m_entityPathHasBeenSet = false;

    Aws::String m_organizationsPolicyId;
    bool m_organizationsPolicyIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
