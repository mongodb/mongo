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
  class DeletePolicyVersionRequest : public IAMRequest
  {
  public:
    AWS_IAM_API DeletePolicyVersionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeletePolicyVersion"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the IAM policy from which you want to
     * delete a version.</p> <p>For more information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetPolicyArn() const{ return m_policyArn; }
    inline bool PolicyArnHasBeenSet() const { return m_policyArnHasBeenSet; }
    inline void SetPolicyArn(const Aws::String& value) { m_policyArnHasBeenSet = true; m_policyArn = value; }
    inline void SetPolicyArn(Aws::String&& value) { m_policyArnHasBeenSet = true; m_policyArn = std::move(value); }
    inline void SetPolicyArn(const char* value) { m_policyArnHasBeenSet = true; m_policyArn.assign(value); }
    inline DeletePolicyVersionRequest& WithPolicyArn(const Aws::String& value) { SetPolicyArn(value); return *this;}
    inline DeletePolicyVersionRequest& WithPolicyArn(Aws::String&& value) { SetPolicyArn(std::move(value)); return *this;}
    inline DeletePolicyVersionRequest& WithPolicyArn(const char* value) { SetPolicyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The policy version to delete.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * that consists of the lowercase letter 'v' followed by one or two digits, and
     * optionally followed by a period '.' and a string of letters and digits.</p>
     * <p>For more information about managed policy versions, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/policies-managed-versions.html">Versioning
     * for managed policies</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline DeletePolicyVersionRequest& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline DeletePolicyVersionRequest& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline DeletePolicyVersionRequest& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}
  private:

    Aws::String m_policyArn;
    bool m_policyArnHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
