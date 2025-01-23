/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/DateTime.h>
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
   * <p>Contains the response to a successful <a>GetOpenIDConnectProvider</a>
   * request. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GetOpenIDConnectProviderResponse">AWS
   * API Reference</a></p>
   */
  class GetOpenIDConnectProviderResult
  {
  public:
    AWS_IAM_API GetOpenIDConnectProviderResult();
    AWS_IAM_API GetOpenIDConnectProviderResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetOpenIDConnectProviderResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The URL that the IAM OIDC provider resource object is associated with. For
     * more information, see <a>CreateOpenIDConnectProvider</a>.</p>
     */
    inline const Aws::String& GetUrl() const{ return m_url; }
    inline void SetUrl(const Aws::String& value) { m_url = value; }
    inline void SetUrl(Aws::String&& value) { m_url = std::move(value); }
    inline void SetUrl(const char* value) { m_url.assign(value); }
    inline GetOpenIDConnectProviderResult& WithUrl(const Aws::String& value) { SetUrl(value); return *this;}
    inline GetOpenIDConnectProviderResult& WithUrl(Aws::String&& value) { SetUrl(std::move(value)); return *this;}
    inline GetOpenIDConnectProviderResult& WithUrl(const char* value) { SetUrl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of client IDs (also known as audiences) that are associated with the
     * specified IAM OIDC provider resource object. For more information, see
     * <a>CreateOpenIDConnectProvider</a>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetClientIDList() const{ return m_clientIDList; }
    inline void SetClientIDList(const Aws::Vector<Aws::String>& value) { m_clientIDList = value; }
    inline void SetClientIDList(Aws::Vector<Aws::String>&& value) { m_clientIDList = std::move(value); }
    inline GetOpenIDConnectProviderResult& WithClientIDList(const Aws::Vector<Aws::String>& value) { SetClientIDList(value); return *this;}
    inline GetOpenIDConnectProviderResult& WithClientIDList(Aws::Vector<Aws::String>&& value) { SetClientIDList(std::move(value)); return *this;}
    inline GetOpenIDConnectProviderResult& AddClientIDList(const Aws::String& value) { m_clientIDList.push_back(value); return *this; }
    inline GetOpenIDConnectProviderResult& AddClientIDList(Aws::String&& value) { m_clientIDList.push_back(std::move(value)); return *this; }
    inline GetOpenIDConnectProviderResult& AddClientIDList(const char* value) { m_clientIDList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of certificate thumbprints that are associated with the specified IAM
     * OIDC provider resource object. For more information, see
     * <a>CreateOpenIDConnectProvider</a>. </p>
     */
    inline const Aws::Vector<Aws::String>& GetThumbprintList() const{ return m_thumbprintList; }
    inline void SetThumbprintList(const Aws::Vector<Aws::String>& value) { m_thumbprintList = value; }
    inline void SetThumbprintList(Aws::Vector<Aws::String>&& value) { m_thumbprintList = std::move(value); }
    inline GetOpenIDConnectProviderResult& WithThumbprintList(const Aws::Vector<Aws::String>& value) { SetThumbprintList(value); return *this;}
    inline GetOpenIDConnectProviderResult& WithThumbprintList(Aws::Vector<Aws::String>&& value) { SetThumbprintList(std::move(value)); return *this;}
    inline GetOpenIDConnectProviderResult& AddThumbprintList(const Aws::String& value) { m_thumbprintList.push_back(value); return *this; }
    inline GetOpenIDConnectProviderResult& AddThumbprintList(Aws::String&& value) { m_thumbprintList.push_back(std::move(value)); return *this; }
    inline GetOpenIDConnectProviderResult& AddThumbprintList(const char* value) { m_thumbprintList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The date and time when the IAM OIDC provider resource object was created in
     * the Amazon Web Services account.</p>
     */
    inline const Aws::Utils::DateTime& GetCreateDate() const{ return m_createDate; }
    inline void SetCreateDate(const Aws::Utils::DateTime& value) { m_createDate = value; }
    inline void SetCreateDate(Aws::Utils::DateTime&& value) { m_createDate = std::move(value); }
    inline GetOpenIDConnectProviderResult& WithCreateDate(const Aws::Utils::DateTime& value) { SetCreateDate(value); return *this;}
    inline GetOpenIDConnectProviderResult& WithCreateDate(Aws::Utils::DateTime&& value) { SetCreateDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the specified IAM OIDC provider. The
     * returned list of tags is sorted by tag key. For more information about tagging,
     * see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline GetOpenIDConnectProviderResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline GetOpenIDConnectProviderResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline GetOpenIDConnectProviderResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline GetOpenIDConnectProviderResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetOpenIDConnectProviderResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetOpenIDConnectProviderResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_url;

    Aws::Vector<Aws::String> m_clientIDList;

    Aws::Vector<Aws::String> m_thumbprintList;

    Aws::Utils::DateTime m_createDate;

    Aws::Vector<Tag> m_tags;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
