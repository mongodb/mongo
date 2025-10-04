/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{
  class GetMFADeviceResult
  {
  public:
    AWS_IAM_API GetMFADeviceResult();
    AWS_IAM_API GetMFADeviceResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetMFADeviceResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The friendly name identifying the user.</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline void SetUserName(const Aws::String& value) { m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userName.assign(value); }
    inline GetMFADeviceResult& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline GetMFADeviceResult& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline GetMFADeviceResult& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Serial number that uniquely identifies the MFA device. For this API, we only
     * accept FIDO security key <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference-arns.html">ARNs</a>.</p>
     */
    inline const Aws::String& GetSerialNumber() const{ return m_serialNumber; }
    inline void SetSerialNumber(const Aws::String& value) { m_serialNumber = value; }
    inline void SetSerialNumber(Aws::String&& value) { m_serialNumber = std::move(value); }
    inline void SetSerialNumber(const char* value) { m_serialNumber.assign(value); }
    inline GetMFADeviceResult& WithSerialNumber(const Aws::String& value) { SetSerialNumber(value); return *this;}
    inline GetMFADeviceResult& WithSerialNumber(Aws::String&& value) { SetSerialNumber(std::move(value)); return *this;}
    inline GetMFADeviceResult& WithSerialNumber(const char* value) { SetSerialNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date that a specified user's MFA device was first enabled.</p>
     */
    inline const Aws::Utils::DateTime& GetEnableDate() const{ return m_enableDate; }
    inline void SetEnableDate(const Aws::Utils::DateTime& value) { m_enableDate = value; }
    inline void SetEnableDate(Aws::Utils::DateTime&& value) { m_enableDate = std::move(value); }
    inline GetMFADeviceResult& WithEnableDate(const Aws::Utils::DateTime& value) { SetEnableDate(value); return *this;}
    inline GetMFADeviceResult& WithEnableDate(Aws::Utils::DateTime&& value) { SetEnableDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The certifications of a specified user's MFA device. We currently provide
     * FIPS-140-2, FIPS-140-3, and FIDO certification levels obtained from <a
     * href="https://fidoalliance.org/metadata/"> FIDO Alliance Metadata Service
     * (MDS)</a>.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetCertifications() const{ return m_certifications; }
    inline void SetCertifications(const Aws::Map<Aws::String, Aws::String>& value) { m_certifications = value; }
    inline void SetCertifications(Aws::Map<Aws::String, Aws::String>&& value) { m_certifications = std::move(value); }
    inline GetMFADeviceResult& WithCertifications(const Aws::Map<Aws::String, Aws::String>& value) { SetCertifications(value); return *this;}
    inline GetMFADeviceResult& WithCertifications(Aws::Map<Aws::String, Aws::String>&& value) { SetCertifications(std::move(value)); return *this;}
    inline GetMFADeviceResult& AddCertifications(const Aws::String& key, const Aws::String& value) { m_certifications.emplace(key, value); return *this; }
    inline GetMFADeviceResult& AddCertifications(Aws::String&& key, const Aws::String& value) { m_certifications.emplace(std::move(key), value); return *this; }
    inline GetMFADeviceResult& AddCertifications(const Aws::String& key, Aws::String&& value) { m_certifications.emplace(key, std::move(value)); return *this; }
    inline GetMFADeviceResult& AddCertifications(Aws::String&& key, Aws::String&& value) { m_certifications.emplace(std::move(key), std::move(value)); return *this; }
    inline GetMFADeviceResult& AddCertifications(const char* key, Aws::String&& value) { m_certifications.emplace(key, std::move(value)); return *this; }
    inline GetMFADeviceResult& AddCertifications(Aws::String&& key, const char* value) { m_certifications.emplace(std::move(key), value); return *this; }
    inline GetMFADeviceResult& AddCertifications(const char* key, const char* value) { m_certifications.emplace(key, value); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetMFADeviceResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetMFADeviceResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_userName;

    Aws::String m_serialNumber;

    Aws::Utils::DateTime m_enableDate;

    Aws::Map<Aws::String, Aws::String> m_certifications;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
