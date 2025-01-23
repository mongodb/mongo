/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class CreatePolicyVersionRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreatePolicyVersionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreatePolicyVersion"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the IAM policy to which you want to add a
     * new version.</p> <p>For more information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetPolicyArn() const{ return m_policyArn; }
    inline bool PolicyArnHasBeenSet() const { return m_policyArnHasBeenSet; }
    inline void SetPolicyArn(const Aws::String& value) { m_policyArnHasBeenSet = true; m_policyArn = value; }
    inline void SetPolicyArn(Aws::String&& value) { m_policyArnHasBeenSet = true; m_policyArn = std::move(value); }
    inline void SetPolicyArn(const char* value) { m_policyArnHasBeenSet = true; m_policyArn.assign(value); }
    inline CreatePolicyVersionRequest& WithPolicyArn(const Aws::String& value) { SetPolicyArn(value); return *this;}
    inline CreatePolicyVersionRequest& WithPolicyArn(Aws::String&& value) { SetPolicyArn(std::move(value)); return *this;}
    inline CreatePolicyVersionRequest& WithPolicyArn(const char* value) { SetPolicyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The JSON policy document that you want to use as the content for this new
     * version of the policy.</p> <p>You must provide policies in JSON format in IAM.
     * However, for CloudFormation templates formatted in YAML, you can provide the
     * policy in JSON or YAML format. CloudFormation always converts a YAML policy to
     * JSON format before submitting it to IAM.</p> <p>The maximum length of the policy
     * document that you can pass in this operation, including whitespace, is listed
     * below. To view the maximum character counts of a managed policy with no
     * whitespaces, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_iam-quotas.html#reference_iam-quotas-entity-length">IAM
     * and STS character quotas</a>.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::String& GetPolicyDocument() const{ return m_policyDocument; }
    inline bool PolicyDocumentHasBeenSet() const { return m_policyDocumentHasBeenSet; }
    inline void SetPolicyDocument(const Aws::String& value) { m_policyDocumentHasBeenSet = true; m_policyDocument = value; }
    inline void SetPolicyDocument(Aws::String&& value) { m_policyDocumentHasBeenSet = true; m_policyDocument = std::move(value); }
    inline void SetPolicyDocument(const char* value) { m_policyDocumentHasBeenSet = true; m_policyDocument.assign(value); }
    inline CreatePolicyVersionRequest& WithPolicyDocument(const Aws::String& value) { SetPolicyDocument(value); return *this;}
    inline CreatePolicyVersionRequest& WithPolicyDocument(Aws::String&& value) { SetPolicyDocument(std::move(value)); return *this;}
    inline CreatePolicyVersionRequest& WithPolicyDocument(const char* value) { SetPolicyDocument(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether to set this version as the policy's default version.</p>
     * <p>When this parameter is <code>true</code>, the new policy version becomes the
     * operative version. That is, it becomes the version that is in effect for the IAM
     * users, groups, and roles that the policy is attached to.</p> <p>For more
     * information about managed policy versions, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/policies-managed-versions.html">Versioning
     * for managed policies</a> in the <i>IAM User Guide</i>.</p>
     */
    inline bool GetSetAsDefault() const{ return m_setAsDefault; }
    inline bool SetAsDefaultHasBeenSet() const { return m_setAsDefaultHasBeenSet; }
    inline void SetSetAsDefault(bool value) { m_setAsDefaultHasBeenSet = true; m_setAsDefault = value; }
    inline CreatePolicyVersionRequest& WithSetAsDefault(bool value) { SetSetAsDefault(value); return *this;}
    ///@}
  private:

    Aws::String m_policyArn;
    bool m_policyArnHasBeenSet = false;

    Aws::String m_policyDocument;
    bool m_policyDocumentHasBeenSet = false;

    bool m_setAsDefault;
    bool m_setAsDefaultHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
