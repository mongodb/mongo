/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class PublishVersionRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API PublishVersionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PublishVersion"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function.</p> <p class="title"> <b>Name
     * formats</b> </p> <ul> <li> <p> <b>Function name</b> -
     * <code>MyFunction</code>.</p> </li> <li> <p> <b>Function ARN</b> -
     * <code>arn:aws:lambda:us-west-2:123456789012:function:MyFunction</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> -
     * <code>123456789012:function:MyFunction</code>.</p> </li> </ul> <p>The length
     * constraint applies only to the full ARN. If you specify only the function name,
     * it is limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline PublishVersionRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline PublishVersionRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline PublishVersionRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Only publish a version if the hash value matches the value that's specified.
     * Use this option to avoid publishing a version if the function code has changed
     * since you last updated it. You can get the hash for the version that you
     * uploaded from the output of <a>UpdateFunctionCode</a>.</p>
     */
    inline const Aws::String& GetCodeSha256() const{ return m_codeSha256; }
    inline bool CodeSha256HasBeenSet() const { return m_codeSha256HasBeenSet; }
    inline void SetCodeSha256(const Aws::String& value) { m_codeSha256HasBeenSet = true; m_codeSha256 = value; }
    inline void SetCodeSha256(Aws::String&& value) { m_codeSha256HasBeenSet = true; m_codeSha256 = std::move(value); }
    inline void SetCodeSha256(const char* value) { m_codeSha256HasBeenSet = true; m_codeSha256.assign(value); }
    inline PublishVersionRequest& WithCodeSha256(const Aws::String& value) { SetCodeSha256(value); return *this;}
    inline PublishVersionRequest& WithCodeSha256(Aws::String&& value) { SetCodeSha256(std::move(value)); return *this;}
    inline PublishVersionRequest& WithCodeSha256(const char* value) { SetCodeSha256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A description for the version to override the description in the function
     * configuration.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline PublishVersionRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline PublishVersionRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline PublishVersionRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Only update the function if the revision ID matches the ID that's specified.
     * Use this option to avoid publishing a version if the function configuration has
     * changed since you last updated it.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline bool RevisionIdHasBeenSet() const { return m_revisionIdHasBeenSet; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionIdHasBeenSet = true; m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionIdHasBeenSet = true; m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionIdHasBeenSet = true; m_revisionId.assign(value); }
    inline PublishVersionRequest& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline PublishVersionRequest& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline PublishVersionRequest& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_codeSha256;
    bool m_codeSha256HasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    Aws::String m_revisionId;
    bool m_revisionIdHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
