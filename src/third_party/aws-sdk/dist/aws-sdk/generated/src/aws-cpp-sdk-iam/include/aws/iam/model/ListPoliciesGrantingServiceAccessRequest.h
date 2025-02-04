/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class ListPoliciesGrantingServiceAccessRequest : public IAMRequest
  {
  public:
    AWS_IAM_API ListPoliciesGrantingServiceAccessRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListPoliciesGrantingServiceAccess"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>Use this parameter only when paginating results and only after you receive a
     * response indicating that the results are truncated. Set it to the value of the
     * <code>Marker</code> element in the response that you received to indicate where
     * the next call should start.</p>
     */
    inline const Aws::String& GetMarker() const{ return m_marker; }
    inline bool MarkerHasBeenSet() const { return m_markerHasBeenSet; }
    inline void SetMarker(const Aws::String& value) { m_markerHasBeenSet = true; m_marker = value; }
    inline void SetMarker(Aws::String&& value) { m_markerHasBeenSet = true; m_marker = std::move(value); }
    inline void SetMarker(const char* value) { m_markerHasBeenSet = true; m_marker.assign(value); }
    inline ListPoliciesGrantingServiceAccessRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListPoliciesGrantingServiceAccessRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListPoliciesGrantingServiceAccessRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the IAM identity (user, group, or role) whose policies you want to
     * list.</p>
     */
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline ListPoliciesGrantingServiceAccessRequest& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline ListPoliciesGrantingServiceAccessRequest& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline ListPoliciesGrantingServiceAccessRequest& WithArn(const char* value) { SetArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The service namespace for the Amazon Web Services services whose policies you
     * want to list.</p> <p>To learn the service namespace for a service, see <a
     * href="https://docs.aws.amazon.com/service-authorization/latest/reference/reference_policies_actions-resources-contextkeys.html">Actions,
     * resources, and condition keys for Amazon Web Services services</a> in the <i>IAM
     * User Guide</i>. Choose the name of the service to view details for that service.
     * In the first paragraph, find the service prefix. For example, <code>(service
     * prefix: a4b)</code>. For more information about service namespaces, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#genref-aws-service-namespaces">Amazon
     * Web Services service namespaces</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetServiceNamespaces() const{ return m_serviceNamespaces; }
    inline bool ServiceNamespacesHasBeenSet() const { return m_serviceNamespacesHasBeenSet; }
    inline void SetServiceNamespaces(const Aws::Vector<Aws::String>& value) { m_serviceNamespacesHasBeenSet = true; m_serviceNamespaces = value; }
    inline void SetServiceNamespaces(Aws::Vector<Aws::String>&& value) { m_serviceNamespacesHasBeenSet = true; m_serviceNamespaces = std::move(value); }
    inline ListPoliciesGrantingServiceAccessRequest& WithServiceNamespaces(const Aws::Vector<Aws::String>& value) { SetServiceNamespaces(value); return *this;}
    inline ListPoliciesGrantingServiceAccessRequest& WithServiceNamespaces(Aws::Vector<Aws::String>&& value) { SetServiceNamespaces(std::move(value)); return *this;}
    inline ListPoliciesGrantingServiceAccessRequest& AddServiceNamespaces(const Aws::String& value) { m_serviceNamespacesHasBeenSet = true; m_serviceNamespaces.push_back(value); return *this; }
    inline ListPoliciesGrantingServiceAccessRequest& AddServiceNamespaces(Aws::String&& value) { m_serviceNamespacesHasBeenSet = true; m_serviceNamespaces.push_back(std::move(value)); return *this; }
    inline ListPoliciesGrantingServiceAccessRequest& AddServiceNamespaces(const char* value) { m_serviceNamespacesHasBeenSet = true; m_serviceNamespaces.push_back(value); return *this; }
    ///@}
  private:

    Aws::String m_marker;
    bool m_markerHasBeenSet = false;

    Aws::String m_arn;
    bool m_arnHasBeenSet = false;

    Aws::Vector<Aws::String> m_serviceNamespaces;
    bool m_serviceNamespacesHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
