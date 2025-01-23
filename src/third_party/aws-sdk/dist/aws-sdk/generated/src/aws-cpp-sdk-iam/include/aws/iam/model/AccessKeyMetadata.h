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
   * <p>Contains information about an Amazon Web Services access key, without its
   * secret key.</p> <p>This data type is used as a response element in the
   * <a>ListAccessKeys</a> operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/AccessKeyMetadata">AWS
   * API Reference</a></p>
   */
  class AccessKeyMetadata
  {
  public:
    AWS_IAM_API AccessKeyMetadata();
    AWS_IAM_API AccessKeyMetadata(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API AccessKeyMetadata& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name of the IAM user that the key is associated with.</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline AccessKeyMetadata& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline AccessKeyMetadata& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline AccessKeyMetadata& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ID for this access key.</p>
     */
    inline const Aws::String& GetAccessKeyId() const{ return m_accessKeyId; }
    inline bool AccessKeyIdHasBeenSet() const { return m_accessKeyIdHasBeenSet; }
    inline void SetAccessKeyId(const Aws::String& value) { m_accessKeyIdHasBeenSet = true; m_accessKeyId = value; }
    inline void SetAccessKeyId(Aws::String&& value) { m_accessKeyIdHasBeenSet = true; m_accessKeyId = std::move(value); }
    inline void SetAccessKeyId(const char* value) { m_accessKeyIdHasBeenSet = true; m_accessKeyId.assign(value); }
    inline AccessKeyMetadata& WithAccessKeyId(const Aws::String& value) { SetAccessKeyId(value); return *this;}
    inline AccessKeyMetadata& WithAccessKeyId(Aws::String&& value) { SetAccessKeyId(std::move(value)); return *this;}
    inline AccessKeyMetadata& WithAccessKeyId(const char* value) { SetAccessKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The status of the access key. <code>Active</code> means that the key is valid
     * for API calls; <code>Inactive</code> means it is not.</p>
     */
    inline const StatusType& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const StatusType& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(StatusType&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline AccessKeyMetadata& WithStatus(const StatusType& value) { SetStatus(value); return *this;}
    inline AccessKeyMetadata& WithStatus(StatusType&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date when the access key was created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline bool CreateDateHasBeenSet() const { return m_createDateHasBeenSet; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDateHasBeenSet = true; m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDateHasBeenSet = true; m_createDate = std::move(value); }
    inline AccessKeyMetadata& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline AccessKeyMetadata& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_accessKeyId;
    bool m_accessKeyIdHasBeenSet = false;

    StatusType m_status;
    bool m_statusHasBeenSet = false;

    Aws::Utils::DateTime m_createDate;
    bool m_createDateHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
