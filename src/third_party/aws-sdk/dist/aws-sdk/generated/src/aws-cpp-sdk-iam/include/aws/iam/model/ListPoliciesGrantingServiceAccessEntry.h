/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/PolicyGrantingServiceAccess.h>
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
   * <p>Contains details about the permissions policies that are attached to the
   * specified identity (user, group, or role).</p> <p>This data type is used as a
   * response element in the <a>ListPoliciesGrantingServiceAccess</a>
   * operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ListPoliciesGrantingServiceAccessEntry">AWS
   * API Reference</a></p>
   */
  class ListPoliciesGrantingServiceAccessEntry
  {
  public:
    AWS_IAM_API ListPoliciesGrantingServiceAccessEntry();
    AWS_IAM_API ListPoliciesGrantingServiceAccessEntry(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ListPoliciesGrantingServiceAccessEntry& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The namespace of the service that was accessed.</p> <p>To learn the service
     * namespace of a service, see <a
     * href="https://docs.aws.amazon.com/service-authorization/latest/reference/reference_policies_actions-resources-contextkeys.html">Actions,
     * resources, and condition keys for Amazon Web Services services</a> in the
     * <i>Service Authorization Reference</i>. Choose the name of the service to view
     * details for that service. In the first paragraph, find the service prefix. For
     * example, <code>(service prefix: a4b)</code>. For more information about service
     * namespaces, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#genref-aws-service-namespaces">Amazon
     * Web Services service namespaces</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetServiceNamespace() const{ return m_serviceNamespace; }
    inline bool ServiceNamespaceHasBeenSet() const { return m_serviceNamespaceHasBeenSet; }
    inline void SetServiceNamespace(const Aws::String& value) { m_serviceNamespaceHasBeenSet = true; m_serviceNamespace = value; }
    inline void SetServiceNamespace(Aws::String&& value) { m_serviceNamespaceHasBeenSet = true; m_serviceNamespace = std::move(value); }
    inline void SetServiceNamespace(const char* value) { m_serviceNamespaceHasBeenSet = true; m_serviceNamespace.assign(value); }
    inline ListPoliciesGrantingServiceAccessEntry& WithServiceNamespace(const Aws::String& value) { SetServiceNamespace(value); return *this;}
    inline ListPoliciesGrantingServiceAccessEntry& WithServiceNamespace(Aws::String&& value) { SetServiceNamespace(std::move(value)); return *this;}
    inline ListPoliciesGrantingServiceAccessEntry& WithServiceNamespace(const char* value) { SetServiceNamespace(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <code>PoliciesGrantingServiceAccess</code> object that contains details
     * about the policy.</p>
     */
    inline const Aws::Vector<PolicyGrantingServiceAccess>& GetPolicies() const{ return m_policies; }
    inline bool PoliciesHasBeenSet() const { return m_policiesHasBeenSet; }
    inline void SetPolicies(const Aws::Vector<PolicyGrantingServiceAccess>& value) { m_policiesHasBeenSet = true; m_policies = value; }
    inline void SetPolicies(Aws::Vector<PolicyGrantingServiceAccess>&& value) { m_policiesHasBeenSet = true; m_policies = std::move(value); }
    inline ListPoliciesGrantingServiceAccessEntry& WithPolicies(const Aws::Vector<PolicyGrantingServiceAccess>& value) { SetPolicies(value); return *this;}
    inline ListPoliciesGrantingServiceAccessEntry& WithPolicies(Aws::Vector<PolicyGrantingServiceAccess>&& value) { SetPolicies(std::move(value)); return *this;}
    inline ListPoliciesGrantingServiceAccessEntry& AddPolicies(const PolicyGrantingServiceAccess& value) { m_policiesHasBeenSet = true; m_policies.push_back(value); return *this; }
    inline ListPoliciesGrantingServiceAccessEntry& AddPolicies(PolicyGrantingServiceAccess&& value) { m_policiesHasBeenSet = true; m_policies.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_serviceNamespace;
    bool m_serviceNamespaceHasBeenSet = false;

    Aws::Vector<PolicyGrantingServiceAccess> m_policies;
    bool m_policiesHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
