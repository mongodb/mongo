/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/Group.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <aws/iam/model/User.h>
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
   * <p>Contains the response to a successful <a>GetGroup</a> request. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GetGroupResponse">AWS
   * API Reference</a></p>
   */
  class GetGroupResult
  {
  public:
    AWS_IAM_API GetGroupResult();
    AWS_IAM_API GetGroupResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetGroupResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A structure that contains details about the group.</p>
     */
    inline const Group& GetGroup() const{ return m_group; }
    inline void SetGroup(const Group& value) { m_group = value; }
    inline void SetGroup(Group&& value) { m_group = std::move(value); }
    inline GetGroupResult& WithGroup(const Group& value) { SetGroup(value); return *this;}
    inline GetGroupResult& WithGroup(Group&& value) { SetGroup(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of users in the group.</p>
     */
    inline const Aws::Vector<User>& GetUsers() const{ return m_users; }
    inline void SetUsers(const Aws::Vector<User>& value) { m_users = value; }
    inline void SetUsers(Aws::Vector<User>&& value) { m_users = std::move(value); }
    inline GetGroupResult& WithUsers(const Aws::Vector<User>& value) { SetUsers(value); return *this;}
    inline GetGroupResult& WithUsers(Aws::Vector<User>&& value) { SetUsers(std::move(value)); return *this;}
    inline GetGroupResult& AddUsers(const User& value) { m_users.push_back(value); return *this; }
    inline GetGroupResult& AddUsers(User&& value) { m_users.push_back(std::move(value)); return *this; }
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
    inline GetGroupResult& WithIsTruncated(bool value) { SetIsTruncated(value); return *this;}
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
    inline GetGroupResult& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline GetGroupResult& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline GetGroupResult& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetGroupResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetGroupResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Group m_group;

    Aws::Vector<User> m_users;

    bool m_isTruncated;

    Aws::String m_marker;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
