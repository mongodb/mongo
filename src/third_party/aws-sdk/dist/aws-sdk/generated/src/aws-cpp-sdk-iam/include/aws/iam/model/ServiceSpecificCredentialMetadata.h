/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/StatusType.h>
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
   * <p>Contains additional details about a service-specific
   * credential.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ServiceSpecificCredentialMetadata">AWS
   * API Reference</a></p>
   */
  class ServiceSpecificCredentialMetadata
  {
  public:
    AWS_IAM_API ServiceSpecificCredentialMetadata();
    AWS_IAM_API ServiceSpecificCredentialMetadata(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ServiceSpecificCredentialMetadata& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name of the IAM user associated with the service-specific credential.</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline ServiceSpecificCredentialMetadata& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline ServiceSpecificCredentialMetadata& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline ServiceSpecificCredentialMetadata& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The status of the service-specific credential. <code>Active</code> means that
     * the key is valid for API calls, while <code>Inactive</code> means it is not.</p>
     */
    inline const StatusType& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const StatusType& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(StatusType&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline ServiceSpecificCredentialMetadata& WithStatus(const StatusType& value) { SetStatus(value); return *this;}
    inline ServiceSpecificCredentialMetadata& WithStatus(StatusType&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The generated user name for the service-specific credential.</p>
     */
    inline const Aws::String& GetServiceUserName() const{ return m_serviceUserName; }
    inline bool ServiceUserNameHasBeenSet() const { return m_serviceUserNameHasBeenSet; }
    inline void SetServiceUserName(const Aws::String& value) { m_serviceUserNameHasBeenSet = true; m_serviceUserName = value; }
    inline void SetServiceUserName(Aws::String&& value) { m_serviceUserNameHasBeenSet = true; m_serviceUserName = std::move(value); }
    inline void SetServiceUserName(const char* value) { m_serviceUserNameHasBeenSet = true; m_serviceUserName.assign(value); }
    inline ServiceSpecificCredentialMetadata& WithServiceUserName(const Aws::String& value) { SetServiceUserName(value); return *this;}
    inline ServiceSpecificCredentialMetadata& WithServiceUserName(Aws::String&& value) { SetServiceUserName(std::move(value)); return *this;}
    inline ServiceSpecificCredentialMetadata& WithServiceUserName(const char* value) { SetServiceUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when the service-specific credential were created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline bool CreateDateHasBeenSet() const { return m_createDateHasBeenSet; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDateHasBeenSet = true; m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDateHasBeenSet = true; m_createDate = std::move(value); }
    inline ServiceSpecificCredentialMetadata& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline ServiceSpecificCredentialMetadata& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The unique identifier for the service-specific credential.</p>
     */
    inline const Aws::String& GetServiceSpecificCredentialId() const{ return m_serviceSpecificCredentialId; }
    inline bool ServiceSpecificCredentialIdHasBeenSet() const { return m_serviceSpecificCredentialIdHasBeenSet; }
    inline void SetServiceSpecificCredentialId(const Aws::String& value) { m_serviceSpecificCredentialIdHasBeenSet = true; m_serviceSpecificCredentialId = value; }
    inline void SetServiceSpecificCredentialId(Aws::String&& value) { m_serviceSpecificCredentialIdHasBeenSet = true; m_serviceSpecificCredentialId = std::move(value); }
    inline void SetServiceSpecificCredentialId(const char* value) { m_serviceSpecificCredentialIdHasBeenSet = true; m_serviceSpecificCredentialId.assign(value); }
    inline ServiceSpecificCredentialMetadata& WithServiceSpecificCredentialId(const Aws::String& value) { SetServiceSpecificCredentialId(value); return *this;}
    inline ServiceSpecificCredentialMetadata& WithServiceSpecificCredentialId(Aws::String&& value) { SetServiceSpecificCredentialId(std::move(value)); return *this;}
    inline ServiceSpecificCredentialMetadata& WithServiceSpecificCredentialId(const char* value) { SetServiceSpecificCredentialId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the service associated with the service-specific credential.</p>
     */
    inline const Aws::String& GetServiceName() const{ return m_serviceName; }
    inline bool ServiceNameHasBeenSet() const { return m_serviceNameHasBeenSet; }
    inline void SetServiceName(const Aws::String& value) { m_serviceNameHasBeenSet = true; m_serviceName = value; }
    inline void SetServiceName(Aws::String&& value) { m_serviceNameHasBeenSet = true; m_serviceName = std::move(value); }
    inline void SetServiceName(const char* value) { m_serviceNameHasBeenSet = true; m_serviceName.assign(value); }
    inline ServiceSpecificCredentialMetadata& WithServiceName(const Aws::String& value) { SetServiceName(value); return *this;}
    inline ServiceSpecificCredentialMetadata& WithServiceName(Aws::String&& value) { SetServiceName(std::move(value)); return *this;}
    inline ServiceSpecificCredentialMetadata& WithServiceName(const char* value) { SetServiceName(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    StatusType m_status;
    bool m_statusHasBeenSet = false;

    Aws::String m_serviceUserName;
    bool m_serviceUserNameHasBeenSet = false;

    Aws::Utils::DateTime m_createDate;
    bool m_createDateHasBeenSet = false;

    Aws::String m_serviceSpecificCredentialId;
    bool m_serviceSpecificCredentialIdHasBeenSet = false;

    Aws::String m_serviceName;
    bool m_serviceNameHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
