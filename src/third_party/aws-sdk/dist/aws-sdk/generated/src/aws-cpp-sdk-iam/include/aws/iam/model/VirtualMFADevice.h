/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/Array.h>
#include <aws/iam/model/User.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
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
   * <p>Contains information about a virtual MFA device.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/VirtualMFADevice">AWS
   * API Reference</a></p>
   */
  class VirtualMFADevice
  {
  public:
    AWS_IAM_API VirtualMFADevice();
    AWS_IAM_API VirtualMFADevice(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API VirtualMFADevice& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The serial number associated with <code>VirtualMFADevice</code>.</p>
     */
    inline const Aws::String& GetSerialNumber() const{ return m_serialNumber; }
    inline bool SerialNumberHasBeenSet() const { return m_serialNumberHasBeenSet; }
    inline void SetSerialNumber(const Aws::String& value) { m_serialNumberHasBeenSet = true; m_serialNumber = value; }
    inline void SetSerialNumber(Aws::String&& value) { m_serialNumberHasBeenSet = true; m_serialNumber = std::move(value); }
    inline void SetSerialNumber(const char* value) { m_serialNumberHasBeenSet = true; m_serialNumber.assign(value); }
    inline VirtualMFADevice& WithSerialNumber(const Aws::String& value) { SetSerialNumber(value); return *this;}
    inline VirtualMFADevice& WithSerialNumber(Aws::String&& value) { SetSerialNumber(std::move(value)); return *this;}
    inline VirtualMFADevice& WithSerialNumber(const char* value) { SetSerialNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The base32 seed defined as specified in <a
     * href="https://tools.ietf.org/html/rfc3548.txt">RFC3548</a>. The
     * <code>Base32StringSeed</code> is base32-encoded. </p>
     */
    inline const Aws::Utils::CryptoBuffer& GetBase32StringSeed() const{ return m_base32StringSeed; }
    inline bool Base32StringSeedHasBeenSet() const { return m_base32StringSeedHasBeenSet; }
    inline void SetBase32StringSeed(const Aws::Utils::CryptoBuffer& value) { m_base32StringSeedHasBeenSet = true; m_base32StringSeed = value; }
    inline void SetBase32StringSeed(Aws::Utils::CryptoBuffer&& value) { m_base32StringSeedHasBeenSet = true; m_base32StringSeed = std::move(value); }
    inline VirtualMFADevice& WithBase32StringSeed(const Aws::Utils::CryptoBuffer& value) { SetBase32StringSeed(value); return *this;}
    inline VirtualMFADevice& WithBase32StringSeed(Aws::Utils::CryptoBuffer&& value) { SetBase32StringSeed(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p> A QR code PNG image that encodes
     * <code>otpauth://totp/$virtualMFADeviceName@$AccountName?secret=$Base32String</code>
     * where <code>$virtualMFADeviceName</code> is one of the create call arguments.
     * <code>AccountName</code> is the user name if set (otherwise, the account ID
     * otherwise), and <code>Base32String</code> is the seed in base32 format. The
     * <code>Base32String</code> value is base64-encoded. </p>
     */
    inline const Aws::Utils::CryptoBuffer& GetQRCodePNG() const{ return m_qRCodePNG; }
    inline bool QRCodePNGHasBeenSet() const { return m_qRCodePNGHasBeenSet; }
    inline void SetQRCodePNG(const Aws::Utils::CryptoBuffer& value) { m_qRCodePNGHasBeenSet = true; m_qRCodePNG = value; }
    inline void SetQRCodePNG(Aws::Utils::CryptoBuffer&& value) { m_qRCodePNGHasBeenSet = true; m_qRCodePNG = std::move(value); }
    inline VirtualMFADevice& WithQRCodePNG(const Aws::Utils::CryptoBuffer& value) { SetQRCodePNG(value); return *this;}
    inline VirtualMFADevice& WithQRCodePNG(Aws::Utils::CryptoBuffer&& value) { SetQRCodePNG(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The IAM user associated with this virtual MFA device.</p>
     */
    inline const User& GetUser() const{ return m_user; }
    inline bool UserHasBeenSet() const { return m_userHasBeenSet; }
    inline void SetUser(const User& value) { m_userHasBeenSet = true; m_user = value; }
    inline void SetUser(User&& value) { m_userHasBeenSet = true; m_user = std::move(value); }
    inline VirtualMFADevice& WithUser(const User& value) { SetUser(value); return *this;}
    inline VirtualMFADevice& WithUser(User&& value) { SetUser(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time on which the virtual MFA device was enabled.</p>
     */
    inline const Aws::Utils::DateTime& GetEnableDate() const{ return m_enableDate; }
    inline bool EnableDateHasBeenSet() const { return m_enableDateHasBeenSet; }
    inline void SetEnableDate(const Aws::Utils::DateTime& value) { m_enableDateHasBeenSet = true; m_enableDate = value; }
    inline void SetEnableDate(Aws::Utils::DateTime&& value) { m_enableDateHasBeenSet = true; m_enableDate = std::move(value); }
    inline VirtualMFADevice& WithEnableDate(const Aws::Utils::DateTime& value) { SetEnableDate(value); return *this;}
    inline VirtualMFADevice& WithEnableDate(Aws::Utils::DateTime&& value) { SetEnableDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the virtual MFA device. For more
     * information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline VirtualMFADevice& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline VirtualMFADevice& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline VirtualMFADevice& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline VirtualMFADevice& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_serialNumber;
    bool m_serialNumberHasBeenSet = false;

    Aws::Utils::CryptoBuffer m_base32StringSeed;
    bool m_base32StringSeedHasBeenSet = false;

    Aws::Utils::CryptoBuffer m_qRCodePNG;
    bool m_qRCodePNGHasBeenSet = false;

    User m_user;
    bool m_userHasBeenSet = false;

    Aws::Utils::DateTime m_enableDate;
    bool m_enableDateHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
