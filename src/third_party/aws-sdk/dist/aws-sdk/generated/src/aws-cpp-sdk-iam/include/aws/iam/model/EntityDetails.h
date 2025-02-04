/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/iam/model/EntityInfo.h>
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
   * <p>An object that contains details about when the IAM entities (users or roles)
   * were last used in an attempt to access the specified Amazon Web Services
   * service.</p> <p>This data type is a response element in the
   * <a>GetServiceLastAccessedDetailsWithEntities</a> operation.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/EntityDetails">AWS
   * API Reference</a></p>
   */
  class EntityDetails
  {
  public:
    AWS_IAM_API EntityDetails();
    AWS_IAM_API EntityDetails(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API EntityDetails& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The <code>EntityInfo</code> object that contains details about the entity
     * (user or role).</p>
     */
    inline const EntityInfo& GetEntityInfo() const{ return m_entityInfo; }
    inline bool EntityInfoHasBeenSet() const { return m_entityInfoHasBeenSet; }
    inline void SetEntityInfo(const EntityInfo& value) { m_entityInfoHasBeenSet = true; m_entityInfo = value; }
    inline void SetEntityInfo(EntityInfo&& value) { m_entityInfoHasBeenSet = true; m_entityInfo = std::move(value); }
    inline EntityDetails& WithEntityInfo(const EntityInfo& value) { SetEntityInfo(value); return *this;}
    inline EntityDetails& WithEntityInfo(EntityInfo&& value) { SetEntityInfo(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when the authenticated entity last attempted to access
     * Amazon Web Services. Amazon Web Services does not report unauthenticated
     * requests.</p> <p>This field is null if no IAM entities attempted to access the
     * service within the <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_access-advisor.html#service-last-accessed-reporting-period">tracking
     * period</a>.</p>
     */
    inline const Aws::Utils::DateTime& GetLastAuthenticated() const{ return m_lastAuthenticated; }
    inline bool LastAuthenticatedHasBeenSet() const { return m_lastAuthenticatedHasBeenSet; }
    inline void SetLastAuthenticated(const Aws::Utils::DateTime& value) { m_lastAuthenticatedHasBeenSet = true; m_lastAuthenticated = value; }
    inline void SetLastAuthenticated(Aws::Utils::DateTime&& value) { m_lastAuthenticatedHasBeenSet = true; m_lastAuthenticated = std::move(value); }
    inline EntityDetails& WithLastAuthenticated(const Aws::Utils::DateTime& value) { SetLastAuthenticated(value); return *this;}
    inline EntityDetails& WithLastAuthenticated(Aws::Utils::DateTime&& value) { SetLastAuthenticated(std::move(value)); return *this;}
    ///@}
  private:

    EntityInfo m_entityInfo;
    bool m_entityInfoHasBeenSet = false;

    Aws::Utils::DateTime m_lastAuthenticated;
    bool m_lastAuthenticatedHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
