/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Contains the response to a successful <a>GetGroupPolicy</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GetGroupPolicyResponse">AWS
   * API Reference</a></p>
   */
  class GetGroupPolicyResult
  {
  public:
    AWS_IAM_API GetGroupPolicyResult();
    AWS_IAM_API GetGroupPolicyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetGroupPolicyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The group the policy is associated with.</p>
     */
    inline const Aws::String& GetGroupName() const{ return m_groupName; }
    inline void SetGroupName(const Aws::String& value) { m_groupName = value; }
    inline void SetGroupName(Aws::String&& value) { m_groupName = std::move(value); }
    inline void SetGroupName(const char* value) { m_groupName.assign(value); }
    inline GetGroupPolicyResult& WithGroupName(const Aws::String& value) { SetGroupName(value); return *this;}
    inline GetGroupPolicyResult& WithGroupName(Aws::String&& value) { SetGroupName(std::move(value)); return *this;}
    inline GetGroupPolicyResult& WithGroupName(const char* value) { SetGroupName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the policy.</p>
     */
    inline const Aws::String& GetPolicyName() const{ return m_policyName; }
    inline void SetPolicyName(const Aws::String& value) { m_policyName = value; }
    inline void SetPolicyName(Aws::String&& value) { m_policyName = std::move(value); }
    inline void SetPolicyName(const char* value) { m_policyName.assign(value); }
    inline GetGroupPolicyResult& WithPolicyName(const Aws::String& value) { SetPolicyName(value); return *this;}
    inline GetGroupPolicyResult& WithPolicyName(Aws::String&& value) { SetPolicyName(std::move(value)); return *this;}
    inline GetGroupPolicyResult& WithPolicyName(const char* value) { SetPolicyName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The policy document.</p> <p>IAM stores policies in JSON format. However,
     * resources that were created using CloudFormation templates can be formatted in
     * YAML. CloudFormation always converts a YAML policy to JSON format before
     * submitting it to IAM.</p>
     */
    inline const Aws::String& GetPolicyDocument() const{ return m_policyDocument; }
    inline void SetPolicyDocument(const Aws::String& value) { m_policyDocument = value; }
    inline void SetPolicyDocument(Aws::String&& value) { m_policyDocument = std::move(value); }
    inline void SetPolicyDocument(const char* value) { m_policyDocument.assign(value); }
    inline GetGroupPolicyResult& WithPolicyDocument(const Aws::String& value) { SetPolicyDocument(value); return *this;}
    inline GetGroupPolicyResult& WithPolicyDocument(Aws::String&& value) { SetPolicyDocument(std::move(value)); return *this;}
    inline GetGroupPolicyResult& WithPolicyDocument(const char* value) { SetPolicyDocument(value); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetGroupPolicyResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetGroupPolicyResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_groupName;

    Aws::String m_policyName;

    Aws::String m_policyDocument;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
