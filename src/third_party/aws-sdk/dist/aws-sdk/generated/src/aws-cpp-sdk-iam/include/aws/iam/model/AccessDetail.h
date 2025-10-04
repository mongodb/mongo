/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
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
   * <p>An object that contains details about when a principal in the reported
   * Organizations entity last attempted to access an Amazon Web Services service. A
   * principal can be an IAM user, an IAM role, or the Amazon Web Services account
   * root user within the reported Organizations entity.</p> <p>This data type is a
   * response element in the <a>GetOrganizationsAccessReport</a>
   * operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/AccessDetail">AWS
   * API Reference</a></p>
   */
  class AccessDetail
  {
  public:
    AWS_IAM_API AccessDetail();
    AWS_IAM_API AccessDetail(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API AccessDetail& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name of the service in which access was attempted.</p>
     */
    inline const Aws::String& GetServiceName() const{ return m_serviceName; }
    inline bool ServiceNameHasBeenSet() const { return m_serviceNameHasBeenSet; }
    inline void SetServiceName(const Aws::String& value) { m_serviceNameHasBeenSet = true; m_serviceName = value; }
    inline void SetServiceName(Aws::String&& value) { m_serviceNameHasBeenSet = true; m_serviceName = std::move(value); }
    inline void SetServiceName(const char* value) { m_serviceNameHasBeenSet = true; m_serviceName.assign(value); }
    inline AccessDetail& WithServiceName(const Aws::String& value) { SetServiceName(value); return *this;}
    inline AccessDetail& WithServiceName(Aws::String&& value) { SetServiceName(std::move(value)); return *this;}
    inline AccessDetail& WithServiceName(const char* value) { SetServiceName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The namespace of the service in which access was attempted.</p> <p>To learn
     * the service namespace of a service, see <a
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
    inline AccessDetail& WithServiceNamespace(const Aws::String& value) { SetServiceNamespace(value); return *this;}
    inline AccessDetail& WithServiceNamespace(Aws::String&& value) { SetServiceNamespace(std::move(value)); return *this;}
    inline AccessDetail& WithServiceNamespace(const char* value) { SetServiceNamespace(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Region where the last service access attempt occurred.</p> <p>This field
     * is null if no principals in the reported Organizations entity attempted to
     * access the service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::String& GetRegion() const{ return m_region; }
    inline bool RegionHasBeenSet() const { return m_regionHasBeenSet; }
    inline void SetRegion(const Aws::String& value) { m_regionHasBeenSet = true; m_region = value; }
    inline void SetRegion(Aws::String&& value) { m_regionHasBeenSet = true; m_region = std::move(value); }
    inline void SetRegion(const char* value) { m_regionHasBeenSet = true; m_region.assign(value); }
    inline AccessDetail& WithRegion(const Aws::String& value) { SetRegion(value); return *this;}
    inline AccessDetail& WithRegion(Aws::String&& value) { SetRegion(std::move(value)); return *this;}
    inline AccessDetail& WithRegion(const char* value) { SetRegion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The path of the Organizations entity (root, organizational unit, or account)
     * from which an authenticated principal last attempted to access the service.
     * Amazon Web Services does not report unauthenticated requests.</p> <p>This field
     * is null if no principals (IAM users, IAM roles, or root user) in the reported
     * Organizations entity attempted to access the service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::String& GetEntityPath() const{ return m_entityPath; }
    inline bool EntityPathHasBeenSet() const { return m_entityPathHasBeenSet; }
    inline void SetEntityPath(const Aws::String& value) { m_entityPathHasBeenSet = true; m_entityPath = value; }
    inline void SetEntityPath(Aws::String&& value) { m_entityPathHasBeenSet = true; m_entityPath = std::move(value); }
    inline void SetEntityPath(const char* value) { m_entityPathHasBeenSet = true; m_entityPath.assign(value); }
    inline AccessDetail& WithEntityPath(const Aws::String& value) { SetEntityPath(value); return *this;}
    inline AccessDetail& WithEntityPath(Aws::String&& value) { SetEntityPath(std::move(value)); return *this;}
    inline AccessDetail& WithEntityPath(const char* value) { SetEntityPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when an authenticated principal most recently attempted to
     * access the service. Amazon Web Services does not report unauthenticated
     * requests.</p> <p>This field is null if no principals in the reported
     * Organizations entity attempted to access the service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::Utils::DateTime& GetLastAuthenticatedTime() const{ return m_lastAuthenticatedTime; }
    inline bool LastAuthenticatedTimeHasBeenSet() const { return m_lastAuthenticatedTimeHasBeenSet; }
    inline void SetLastAuthenticatedTime(const Aws::Utils::DateTime& value) { m_lastAuthenticatedTimeHasBeenSet = true; m_lastAuthenticatedTime = value; }
    inline void SetLastAuthenticatedTime(Aws::Utils::DateTime&& value) { m_lastAuthenticatedTimeHasBeenSet = true; m_lastAuthenticatedTime = std::move(value); }
    inline AccessDetail& WithLastAuthenticatedTime(const Aws::Utils::DateTime& value) { SetLastAuthenticatedTime(value); return *this;}
    inline AccessDetail& WithLastAuthenticatedTime(Aws::Utils::DateTime&& value) { SetLastAuthenticatedTime(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of accounts with authenticated principals (root user, IAM users,
     * and IAM roles) that attempted to access the service in the tracking period.</p>
     */
    inline int GetTotalAuthenticatedEntities() const{ return m_totalAuthenticatedEntities; }
    inline bool TotalAuthenticatedEntitiesHasBeenSet() const { return m_totalAuthenticatedEntitiesHasBeenSet; }
    inline void SetTotalAuthenticatedEntities(int value) { m_totalAuthenticatedEntitiesHasBeenSet = true; m_totalAuthenticatedEntities = value; }
    inline AccessDetail& WithTotalAuthenticatedEntities(int value) { SetTotalAuthenticatedEntities(value); return *this;}
    ///@}
  private:

    Aws::String m_serviceName;
    bool m_serviceNameHasBeenSet = false;

    Aws::String m_serviceNamespace;
    bool m_serviceNamespaceHasBeenSet = false;

    Aws::String m_region;
    bool m_regionHasBeenSet = false;

    Aws::String m_entityPath;
    bool m_entityPathHasBeenSet = false;

    Aws::Utils::DateTime m_lastAuthenticatedTime;
    bool m_lastAuthenticatedTimeHasBeenSet = false;

    int m_totalAuthenticatedEntities;
    bool m_totalAuthenticatedEntitiesHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
