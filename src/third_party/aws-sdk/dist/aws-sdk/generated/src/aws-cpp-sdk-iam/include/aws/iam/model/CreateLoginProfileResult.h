/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/LoginProfile.h>
#include <aws/iam/model/ResponseMetadata.h>
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
   * <p>Contains the response to a successful <a>CreateLoginProfile</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/CreateLoginProfileResponse">AWS
   * API Reference</a></p>
   */
  class CreateLoginProfileResult
  {
  public:
    AWS_IAM_API CreateLoginProfileResult();
    AWS_IAM_API CreateLoginProfileResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API CreateLoginProfileResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A structure containing the user name and password create date.</p>
     */
    inline const LoginProfile& GetLoginProfile() const{ return m_loginProfile; }
    inline void SetLoginProfile(const LoginProfile& value) { m_loginProfile = value; }
    inline void SetLoginProfile(LoginProfile&& value) { m_loginProfile = std::move(value); }
    inline CreateLoginProfileResult& WithLoginProfile(const LoginProfile& value) { SetLoginProfile(value); return *this;}
    inline CreateLoginProfileResult& WithLoginProfile(LoginProfile&& value) { SetLoginProfile(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline CreateLoginProfileResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline CreateLoginProfileResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    LoginProfile m_loginProfile;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
