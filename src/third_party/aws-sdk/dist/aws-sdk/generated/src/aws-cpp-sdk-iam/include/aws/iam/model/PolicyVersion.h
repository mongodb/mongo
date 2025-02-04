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
   * <p>Contains information about a version of a managed policy.</p> <p>This data
   * type is used as a response element in the <a>CreatePolicyVersion</a>,
   * <a>GetPolicyVersion</a>, <a>ListPolicyVersions</a>, and
   * <a>GetAccountAuthorizationDetails</a> operations. </p> <p>For more information
   * about managed policies, refer to <a
   * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/policies-managed-vs-inline.html">Managed
   * policies and inline policies</a> in the <i>IAM User Guide</i>. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PolicyVersion">AWS
   * API Reference</a></p>
   */
  class PolicyVersion
  {
  public:
    AWS_IAM_API PolicyVersion();
    AWS_IAM_API PolicyVersion(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PolicyVersion& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The policy document.</p> <p>The policy document is returned in the response
     * to the <a>GetPolicyVersion</a> and <a>GetAccountAuthorizationDetails</a>
     * operations. It is not returned in the response to the <a>CreatePolicyVersion</a>
     * or <a>ListPolicyVersions</a> operations. </p> <p>The policy document returned in
     * this structure is URL-encoded compliant with <a
     * href="https://tools.ietf.org/html/rfc3986">RFC 3986</a>. You can use a URL
     * decoding method to convert the policy back to plain JSON text. For example, if
     * you use Java, you can use the <code>decode</code> method of the
     * <code>java.net.URLDecoder</code> utility class in the Java SDK. Other languages
     * and SDKs provide similar functionality.</p>
     */
    inline const Aws::String& GetDocument() const{ return m_document; }
    inline bool DocumentHasBeenSet() const { return m_documentHasBeenSet; }
    inline void SetDocument(const Aws::String& value) { m_documentHasBeenSet = true; m_document = value; }
    inline void SetDocument(Aws::String&& value) { m_documentHasBeenSet = true; m_document = std::move(value); }
    inline void SetDocument(const char* value) { m_documentHasBeenSet = true; m_document.assign(value); }
    inline PolicyVersion& WithDocument(const Aws::String& value) { SetDocument(value); return *this;}
    inline PolicyVersion& WithDocument(Aws::String&& value) { SetDocument(std::move(value)); return *this;}
    inline PolicyVersion& WithDocument(const char* value) { SetDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The identifier for the policy version.</p> <p>Policy version identifiers
     * always begin with <code>v</code> (always lowercase). When a policy is created,
     * the first policy version is <code>v1</code>. </p>
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline PolicyVersion& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline PolicyVersion& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline PolicyVersion& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether the policy version is set as the policy's default
     * version.</p>
     */
    inline bool GetIsDefaultVersion() const{ return m_isDefaultVersion; }
    inline bool IsDefaultVersionHasBeenSet() const { return m_isDefaultVersionHasBeenSet; }
    inline void SetIsDefaultVersion(bool value) { m_isDefaultVersionHasBeenSet = true; m_isDefaultVersion = value; }
    inline PolicyVersion& WithIsDefaultVersion(bool value) { SetIsDefaultVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time, in <a href="http://www.iso.org/iso/iso8601">ISO 8601
     * date-time format</a>, when the policy version was created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline bool CreateDateHasBeenSet() const { return m_createDateHasBeenSet; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDateHasBeenSet = true; m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDateHasBeenSet = true; m_createDate = std::move(value); }
    inline PolicyVersion& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline PolicyVersion& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_document;
    bool m_documentHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    bool m_isDefaultVersion;
    bool m_isDefaultVersionHasBeenSet = false;

    Aws::Utils::DateTime m_createDate;
    bool m_createDateHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
