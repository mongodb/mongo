/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/PasswordPolicy.h>
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
   * <p>Contains the response to a successful <a>GetAccountPasswordPolicy</a>
   * request. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GetAccountPasswordPolicyResponse">AWS
   * API Reference</a></p>
   */
  class GetAccountPasswordPolicyResult
  {
  public:
    AWS_IAM_API GetAccountPasswordPolicyResult();
    AWS_IAM_API GetAccountPasswordPolicyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetAccountPasswordPolicyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A structure that contains details about the account's password policy.</p>
     */
    inline const PasswordPolicy& GetPasswordPolicy() const{ return m_passwordPolicy; }
    inline void SetPasswordPolicy(const PasswordPolicy& value) { m_passwordPolicy = value; }
    inline void SetPasswordPolicy(PasswordPolicy&& value) { m_passwordPolicy = std::move(value); }
    inline GetAccountPasswordPolicyResult& WithPasswordPolicy(const PasswordPolicy& value) { SetPasswordPolicy(value); return *this;}
    inline GetAccountPasswordPolicyResult& WithPasswordPolicy(PasswordPolicy&& value) { SetPasswordPolicy(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetAccountPasswordPolicyResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetAccountPasswordPolicyResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    PasswordPolicy m_passwordPolicy;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
