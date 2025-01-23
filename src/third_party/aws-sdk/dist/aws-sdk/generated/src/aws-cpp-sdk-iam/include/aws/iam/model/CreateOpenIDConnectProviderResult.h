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
   * <p>Contains the response to a successful <a>CreateOpenIDConnectProvider</a>
   * request. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/CreateOpenIDConnectProviderResponse">AWS
   * API Reference</a></p>
   */
  class CreateOpenIDConnectProviderResult
  {
  public:
    AWS_IAM_API CreateOpenIDConnectProviderResult();
    AWS_IAM_API CreateOpenIDConnectProviderResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API CreateOpenIDConnectProviderResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the new IAM OpenID Connect provider that is
     * created. For more information, see <a>OpenIDConnectProviderListEntry</a>. </p>
     */
    inline const Aws::String& GetOpenIDConnectProviderArn() const{ return m_openIDConnectProviderArn; }
    inline void SetOpenIDConnectProviderArn(const Aws::String& value) { m_openIDConnectProviderArn = value; }
    inline void SetOpenIDConnectProviderArn(Aws::String&& value) { m_openIDConnectProviderArn = std::move(value); }
    inline void SetOpenIDConnectProviderArn(const char* value) { m_openIDConnectProviderArn.assign(value); }
    inline CreateOpenIDConnectProviderResult& WithOpenIDConnectProviderArn(const Aws::String& value) { SetOpenIDConnectProviderArn(value); return *this;}
    inline CreateOpenIDConnectProviderResult& WithOpenIDConnectProviderArn(Aws::String&& value) { SetOpenIDConnectProviderArn(std::move(value)); return *this;}
    inline CreateOpenIDConnectProviderResult& WithOpenIDConnectProviderArn(const char* value) { SetOpenIDConnectProviderArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the new IAM OIDC provider. The returned
     * list of tags is sorted by tag key. For more information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline CreateOpenIDConnectProviderResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline CreateOpenIDConnectProviderResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateOpenIDConnectProviderResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline CreateOpenIDConnectProviderResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline CreateOpenIDConnectProviderResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline CreateOpenIDConnectProviderResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_openIDConnectProviderArn;

    Aws::Vector<Tag> m_tags;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
