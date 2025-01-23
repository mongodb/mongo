/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class ListSAMLProviderTagsResult
  {
  public:
    AWS_IAM_API ListSAMLProviderTagsResult();
    AWS_IAM_API ListSAMLProviderTagsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API ListSAMLProviderTagsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The list of tags that are currently attached to the Security Assertion Markup
     * Language (SAML) identity provider. Each tag consists of a key name and an
     * associated value. If no tags are attached to the specified resource, the
     * response contains an empty list.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline ListSAMLProviderTagsResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline ListSAMLProviderTagsResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline ListSAMLProviderTagsResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline ListSAMLProviderTagsResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A flag that indicates whether there are more items to return. If your results
     * were truncated, you can make a subsequent pagination request using the
     * <code>Marker</code> request parameter to retrieve more items. Note that IAM
     * might return fewer than the <code>MaxItems</code> number of results even when
     * there are more results available. We recommend that you check
     * <code>IsTruncated</code> after every call to ensure that you receive all your
     * results.</p>
     */
    inline bool GetIsTruncated() const{ return m_isTruncated; }
    inline void SetIsTruncated(bool value) { m_isTruncated = value; }
    inline ListSAMLProviderTagsResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When <code>IsTruncated</code> is <code>true</code>, this element is present
     * and contains the value to use for the <code>Marker</code> parameter in a
     * subsequent pagination request.</p>
     */
    inline const Aws::String& GetMarker() const{ return m_marker; }
    inline void SetMarker(const Aws::String& value) { m_marker = value; }
    inline void SetMarker(Aws::String&& value) { m_marker = std::move(value); }
    inline void SetMarker(const char* value) { m_marker.assign(value); }
    inline ListSAMLProviderTagsResult& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListSAMLProviderTagsResult& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListSAMLProviderTagsResult& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline ListSAMLProviderTagsResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline ListSAMLProviderTagsResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::Vector<Tag> m_tags;

    bool m_isTruncated;

    Aws::String m_marker;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
