/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/TrackedActionLastAccessed.h>
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
   * <p>Contains details about the most recent attempt to access the service.</p>
   * <p>This data type is used as a response element in the
   * <a>GetServiceLastAccessedDetails</a> operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ServiceLastAccessed">AWS
   * API Reference</a></p>
   */
  class ServiceLastAccessed
  {
  public:
    AWS_IAM_API ServiceLastAccessed();
    AWS_IAM_API ServiceLastAccessed(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ServiceLastAccessed& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

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
    inline ServiceLastAccessed& WithServiceName(const Aws::String& value) { SetServiceName(value); return *this;}
    inline ServiceLastAccessed& WithServiceName(Aws::String&& value) { SetServiceName(std::move(value)); return *this;}
    inline ServiceLastAccessed& WithServiceName(const char* value) { SetServiceName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when an authenticated entity most recently attempted to
     * access the service. Amazon Web Services does not report unauthenticated
     * requests.</p> <p>This field is null if no IAM entities attempted to access the
     * service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::Utils::DateTime& GetLastAuthenticated() const{ return m_lastAuthenticated; }
    inline bool LastAuthenticatedHasBeenSet() const { return m_lastAuthenticatedHasBeenSet; }
    inline void SetLastAuthenticated(const Aws::Utils::DateTime& value) { m_lastAuthenticatedHasBeenSet = true; m_lastAuthenticated = value; }
    inline void SetLastAuthenticated(Aws::Utils::DateTime&& value) { m_lastAuthenticatedHasBeenSet = true; m_lastAuthenticated = std::move(value); }
    inline ServiceLastAccessed& WithLastAuthenticated(const Aws::Utils::DateTime& value) { SetLastAuthenticated(value); return *this;}
    inline ServiceLastAccessed& WithLastAuthenticated(Aws::Utils::DateTime&& value) { SetLastAuthenticated(std::move(value)); return *this;}
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
     * Web Services Service Namespaces</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetServiceNamespace() const{ return m_serviceNamespace; }
    inline bool ServiceNamespaceHasBeenSet() const { return m_serviceNamespaceHasBeenSet; }
    inline void SetServiceNamespace(const Aws::String& value) { m_serviceNamespaceHasBeenSet = true; m_serviceNamespace = value; }
    inline void SetServiceNamespace(Aws::String&& value) { m_serviceNamespaceHasBeenSet = true; m_serviceNamespace = std::move(value); }
    inline void SetServiceNamespace(const char* value) { m_serviceNamespaceHasBeenSet = true; m_serviceNamespace.assign(value); }
    inline ServiceLastAccessed& WithServiceNamespace(const Aws::String& value) { SetServiceNamespace(value); return *this;}
    inline ServiceLastAccessed& WithServiceNamespace(Aws::String&& value) { SetServiceNamespace(std::move(value)); return *this;}
    inline ServiceLastAccessed& WithServiceNamespace(const char* value) { SetServiceNamespace(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the authenticated entity (user or role) that last attempted to
     * access the service. Amazon Web Services does not report unauthenticated
     * requests.</p> <p>This field is null if no IAM entities attempted to access the
     * service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::String& GetLastAuthenticatedEntity() const{ return m_lastAuthenticatedEntity; }
    inline bool LastAuthenticatedEntityHasBeenSet() const { return m_lastAuthenticatedEntityHasBeenSet; }
    inline void SetLastAuthenticatedEntity(const Aws::String& value) { m_lastAuthenticatedEntityHasBeenSet = true; m_lastAuthenticatedEntity = value; }
    inline void SetLastAuthenticatedEntity(Aws::String&& value) { m_lastAuthenticatedEntityHasBeenSet = true; m_lastAuthenticatedEntity = std::move(value); }
    inline void SetLastAuthenticatedEntity(const char* value) { m_lastAuthenticatedEntityHasBeenSet = true; m_lastAuthenticatedEntity.assign(value); }
    inline ServiceLastAccessed& WithLastAuthenticatedEntity(const Aws::String& value) { SetLastAuthenticatedEntity(value); return *this;}
    inline ServiceLastAccessed& WithLastAuthenticatedEntity(Aws::String&& value) { SetLastAuthenticatedEntity(std::move(value)); return *this;}
    inline ServiceLastAccessed& WithLastAuthenticatedEntity(const char* value) { SetLastAuthenticatedEntity(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Region from which the authenticated entity (user or role) last attempted
     * to access the service. Amazon Web Services does not report unauthenticated
     * requests.</p> <p>This field is null if no IAM entities attempted to access the
     * service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::String& GetLastAuthenticatedRegion() const{ return m_lastAuthenticatedRegion; }
    inline bool LastAuthenticatedRegionHasBeenSet() const { return m_lastAuthenticatedRegionHasBeenSet; }
    inline void SetLastAuthenticatedRegion(const Aws::String& value) { m_lastAuthenticatedRegionHasBeenSet = true; m_lastAuthenticatedRegion = value; }
    inline void SetLastAuthenticatedRegion(Aws::String&& value) { m_lastAuthenticatedRegionHasBeenSet = true; m_lastAuthenticatedRegion = std::move(value); }
    inline void SetLastAuthenticatedRegion(const char* value) { m_lastAuthenticatedRegionHasBeenSet = true; m_lastAuthenticatedRegion.assign(value); }
    inline ServiceLastAccessed& WithLastAuthenticatedRegion(const Aws::String& value) { SetLastAuthenticatedRegion(value); return *this;}
    inline ServiceLastAccessed& WithLastAuthenticatedRegion(Aws::String&& value) { SetLastAuthenticatedRegion(std::move(value)); return *this;}
    inline ServiceLastAccessed& WithLastAuthenticatedRegion(const char* value) { SetLastAuthenticatedRegion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The total number of authenticated principals (root user, IAM users, or IAM
     * roles) that have attempted to access the service.</p> <p>This field is null if
     * no principals attempted to access the service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline int GetTotalAuthenticatedEntities() const{ return m_totalAuthenticatedEntities; }
    inline bool TotalAuthenticatedEntitiesHasBeenSet() const { return m_totalAuthenticatedEntitiesHasBeenSet; }
    inline void SetTotalAuthenticatedEntities(int value) { m_totalAuthenticatedEntitiesHasBeenSet = true; m_totalAuthenticatedEntities = value; }
    inline ServiceLastAccessed& WithTotalAuthenticatedEntities(int value) { SetTotalAuthenticatedEntities(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An object that contains details about the most recent attempt to access a
     * tracked action within the service.</p> <p>This field is null if there no tracked
     * actions or if the principal did not use the tracked actions within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>. This field is also null if the report was generated at the service
     * level and not the action level. For more information, see the
     * <code>Granularity</code> field in <a>GenerateServiceLastAccessedDetails</a>.</p>
     */
    inline const Aws::Vector<TrackedActionLastAccessed>& GetTrackedActionsLastAccessed() const{ return m_trackedActionsLastAccessed; }
    inline bool TrackedActionsLastAccessedHasBeenSet() const { return m_trackedActionsLastAccessedHasBeenSet; }
    inline void SetTrackedActionsLastAccessed(const Aws::Vector<TrackedActionLastAccessed>& value) { m_trackedActionsLastAccessedHasBeenSet = true; m_trackedActionsLastAccessed = value; }
    inline void SetTrackedActionsLastAccessed(Aws::Vector<TrackedActionLastAccessed>&& value) { m_trackedActionsLastAccessedHasBeenSet = true; m_trackedActionsLastAccessed = std::move(value); }
    inline ServiceLastAccessed& WithTrackedActionsLastAccessed(const Aws::Vector<TrackedActionLastAccessed>& value) { SetTrackedActionsLastAccessed(value); return *this;}
    inline ServiceLastAccessed& WithTrackedActionsLastAccessed(Aws::Vector<TrackedActionLastAccessed>&& value) { SetTrackedActionsLastAccessed(std::move(value)); return *this;}
    inline ServiceLastAccessed& AddTrackedActionsLastAccessed(const TrackedActionLastAccessed& value) { m_trackedActionsLastAccessedHasBeenSet = true; m_trackedActionsLastAccessed.push_back(value); return *this; }
    inline ServiceLastAccessed& AddTrackedActionsLastAccessed(TrackedActionLastAccessed&& value) { m_trackedActionsLastAccessedHasBeenSet = true; m_trackedActionsLastAccessed.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_serviceName;
    bool m_serviceNameHasBeenSet = false;

    Aws::Utils::DateTime m_lastAuthenticated;
    bool m_lastAuthenticatedHasBeenSet = false;

    Aws::String m_serviceNamespace;
    bool m_serviceNamespaceHasBeenSet = false;

    Aws::String m_lastAuthenticatedEntity;
    bool m_lastAuthenticatedEntityHasBeenSet = false;

    Aws::String m_lastAuthenticatedRegion;
    bool m_lastAuthenticatedRegionHasBeenSet = false;

    int m_totalAuthenticatedEntities;
    bool m_totalAuthenticatedEntitiesHasBeenSet = false;

    Aws::Vector<TrackedActionLastAccessed> m_trackedActionsLastAccessed;
    bool m_trackedActionsLastAccessedHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
