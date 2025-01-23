/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Contains the response to a successful <a>CreateSAMLProvider</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/CreateSAMLProviderResponse">AWS
   * API Reference</a></p>
   */
  class CreateSAMLProviderResult
  {
  public:
    AWS_IAM_API CreateSAMLProviderResult();
    AWS_IAM_API CreateSAMLProviderResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API CreateSAMLProviderResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the new SAML provider resource in IAM.</p>
     */
    inline const Aws::String& GetSAMLProviderArn() const{ return m_sAMLProviderArn; }
    inline void SetSAMLProviderArn(const Aws::String& value) { m_sAMLProviderArn = value; }
    inline void SetSAMLProviderArn(Aws::String&& value) { m_sAMLProviderArn = std::move(value); }
    inline void SetSAMLProviderArn(const char* value) { m_sAMLProviderArn.assign(value); }
    inline CreateSAMLProviderResult& WithSAMLProviderArn(const Aws::String& value) { SetSAMLProviderArn(value); return *this;}
    inline CreateSAMLProviderResult& WithSAMLProviderArn(Aws::String&& value) { SetSAMLProviderArn(std::move(value)); return *this;}
    inline CreateSAMLProviderResult& WithSAMLProviderArn(const char* value) { SetSAMLProviderArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the new IAM SAML provider. The returned
     * list of tags is sorted by tag key. For more information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline CreateSAMLProviderResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline CreateSAMLProviderResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateSAMLProviderResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline CreateSAMLProviderResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline CreateSAMLProviderResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline CreateSAMLProviderResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_sAMLProviderArn;

    Aws::Vector<Tag> m_tags;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
