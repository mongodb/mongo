/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Contains information about the reason that the operation failed.</p> <p>This
   * data type is used as a response element in the
   * <a>GetOrganizationsAccessReport</a>, <a>GetServiceLastAccessedDetails</a>, and
   * <a>GetServiceLastAccessedDetailsWithEntities</a> operations.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ErrorDetails">AWS
   * API Reference</a></p>
   */
  class ErrorDetails
  {
  public:
    AWS_IAM_API ErrorDetails();
    AWS_IAM_API ErrorDetails(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ErrorDetails& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>Detailed information about the reason that the operation failed.</p>
     */
    inline const Aws::String& GetMessage() const{ return m_message; }
    inline bool MessageHasBeenSet() const { return m_messageHasBeenSet; }
    inline void SetMessage(const Aws::String& value) { m_messageHasBeenSet = true; m_message = value; }
    inline void SetMessage(Aws::String&& value) { m_messageHasBeenSet = true; m_message = std::move(value); }
    inline void SetMessage(const char* value) { m_messageHasBeenSet = true; m_message.assign(value); }
    inline ErrorDetails& WithMessage(const Aws::String& value) { SetMessage(value); return *this;}
    inline ErrorDetails& WithMessage(Aws::String&& value) { SetMessage(std::move(value)); return *this;}
    inline ErrorDetails& WithMessage(const char* value) { SetMessage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error code associated with the operation failure.</p>
     */
    inline const Aws::String& GetCode() const{ return m_code; }
    inline bool CodeHasBeenSet() const { return m_codeHasBeenSet; }
    inline void SetCode(const Aws::String& value) { m_codeHasBeenSet = true; m_code = value; }
    inline void SetCode(Aws::String&& value) { m_codeHasBeenSet = true; m_code = std::move(value); }
    inline void SetCode(const char* value) { m_codeHasBeenSet = true; m_code.assign(value); }
    inline ErrorDetails& WithCode(const Aws::String& value) { SetCode(value); return *this;}
    inline ErrorDetails& WithCode(Aws::String&& value) { SetCode(std::move(value)); return *this;}
    inline ErrorDetails& WithCode(const char* value) { SetCode(value); return *this;}
    ///@}
  private:

    Aws::String m_message;
    bool m_messageHasBeenSet = false;

    Aws::String m_code;
    bool m_codeHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
