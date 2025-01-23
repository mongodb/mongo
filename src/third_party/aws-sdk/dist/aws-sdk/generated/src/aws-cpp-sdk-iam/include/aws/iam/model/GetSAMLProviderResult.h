/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <aws/iam/model/Tag.h>
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
  /**
   * <p>Contains the response to a successful <a>GetSAMLProvider</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GetSAMLProviderResponse">AWS
   * API Reference</a></p>
   */
  class GetSAMLProviderResult
  {
  public:
    AWS_IAM_API GetSAMLProviderResult();
    AWS_IAM_API GetSAMLProviderResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetSAMLProviderResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The XML metadata document that includes information about an identity
     * provider.</p>
     */
    inline const Aws::String& GetSAMLMetadataDocument() const{ return m_sAMLMetadataDocument; }
    inline void SetSAMLMetadataDocument(const Aws::String& value) { m_sAMLMetadataDocument = value; }
    inline void SetSAMLMetadataDocument(Aws::String&& value) { m_sAMLMetadataDocument = std::move(value); }
    inline void SetSAMLMetadataDocument(const char* value) { m_sAMLMetadataDocument.assign(value); }
    inline GetSAMLProviderResult& WithSAMLMetadataDocument(const Aws::String& value) { SetSAMLMetadataDocument(value); return *this;}
    inline GetSAMLProviderResult& WithSAMLMetadataDocument(Aws::String&& value) { SetSAMLMetadataDocument(std::move(value)); return *this;}
    inline GetSAMLProviderResult& WithSAMLMetadataDocument(const char* value) { SetSAMLMetadataDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time when the SAML provider was created.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDate = std::move(value); }
    inline GetSAMLProviderResult& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline GetSAMLProviderResult& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The expiration date and time for the SAML provider.</p>
     */
    inline const Aws::Utils::DateTime& GetValidUntil() const{ return m_validUntil; }
    inline void SetValidUntil(const Aws::Utils::DateTime& value) { m_validUntil = value; }
    inline void SetValidUntil(Aws::Utils::DateTime&& value) { m_validUntil = std::move(value); }
    inline GetSAMLProviderResult& WithValidUntil(const Aws::Utils::DateTime& value) { SetValidUntil(value); return *this;}
    inline GetSAMLProviderResult& WithValidUntil(Aws::Utils::DateTime&& value) { SetValidUntil(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the specified IAM SAML provider. The
     * returned list of tags is sorted by tag key. For more information about tagging,
     * see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline GetSAMLProviderResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline GetSAMLProviderResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline GetSAMLProviderResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline GetSAMLProviderResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetSAMLProviderResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetSAMLProviderResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_sAMLMetadataDocument;

    Aws::Utils::DateTime m_createDate;

    Aws::Utils::DateTime m_validUntil;

    Aws::Vector<Tag> m_tags;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
