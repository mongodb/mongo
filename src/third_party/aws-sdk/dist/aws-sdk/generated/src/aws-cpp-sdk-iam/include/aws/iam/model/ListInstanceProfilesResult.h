/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <aws/iam/model/InstanceProfile.h>
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
   * <p>Contains the response to a successful <a>ListInstanceProfiles</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ListInstanceProfilesResponse">AWS
   * API Reference</a></p>
   */
  class ListInstanceProfilesResult
  {
  public:
    AWS_IAM_API ListInstanceProfilesResult();
    AWS_IAM_API ListInstanceProfilesResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API ListInstanceProfilesResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A list of instance profiles.</p>
     */
    inline const Aws::Vector<InstanceProfile>& GetInstanceProfiles() const{ return m_instanceProfiles; }
    inline void SetInstanceProfiles(const Aws::Vector<InstanceProfile>& value) { m_instanceProfiles = value; }
    inline void SetInstanceProfiles(Aws::Vector<InstanceProfile>&& value) { m_instanceProfiles = std::move(value); }
    inline ListInstanceProfilesResult& WithInstanceProfiles(const Aws::Vector<InstanceProfile>& value) { SetInstanceProfiles(value); return *this;}
    inline ListInstanceProfilesResult& WithInstanceProfiles(Aws::Vector<InstanceProfile>&& value) { SetInstanceProfiles(std::move(value)); return *this;}
    inline ListInstanceProfilesResult& AddInstanceProfiles(const InstanceProfile& value) { m_instanceProfiles.push_back(value); return *this; }
    inline ListInstanceProfilesResult& AddInstanceProfiles(InstanceProfile&& value) { m_instanceProfiles.push_back(std::move(value)); return *this; }
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
    inline ListInstanceProfilesResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
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
    inline ListInstanceProfilesResult& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListInstanceProfilesResult& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListInstanceProfilesResult& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline ListInstanceProfilesResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline ListInstanceProfilesResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::Vector<InstanceProfile> m_instanceProfiles;

    bool m_isTruncated;

    Aws::String m_marker;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
