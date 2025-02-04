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
namespace Http
{
    class URI;
} //namespace Http
namespace Lambda
{
namespace Model
{

  /**
   */
  class RemovePermissionRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API RemovePermissionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "RemovePermission"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;

    AWS_LAMBDA_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function, version, or alias.</p> <p
     * class="title"> <b>Name formats</b> </p> <ul> <li> <p> <b>Function name</b> –
     * <code>my-function</code> (name-only), <code>my-function:v1</code> (with
     * alias).</p> </li> <li> <p> <b>Function ARN</b> –
     * <code>arn:aws:lambda:us-west-2:123456789012:function:my-function</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> –
     * <code>123456789012:function:my-function</code>.</p> </li> </ul> <p>You can
     * append a version number or alias to any of the formats. The length constraint
     * applies only to the full ARN. If you specify only the function name, it is
     * limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline RemovePermissionRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline RemovePermissionRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline RemovePermissionRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Statement ID of the permission to remove.</p>
     */
    inline const Aws::String& GetStatementId() const{ return m_statementId; }
    inline bool StatementIdHasBeenSet() const { return m_statementIdHasBeenSet; }
    inline void SetStatementId(const Aws::String& value) { m_statementIdHasBeenSet = true; m_statementId = value; }
    inline void SetStatementId(Aws::String&& value) { m_statementIdHasBeenSet = true; m_statementId = std::move(value); }
    inline void SetStatementId(const char* value) { m_statementIdHasBeenSet = true; m_statementId.assign(value); }
    inline RemovePermissionRequest& WithStatementId(const Aws::String& value) { SetStatementId(value); return *this;}
    inline RemovePermissionRequest& WithStatementId(Aws::String&& value) { SetStatementId(std::move(value)); return *this;}
    inline RemovePermissionRequest& WithStatementId(const char* value) { SetStatementId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify a version or alias to remove permissions from a published version of
     * the function.</p>
     */
    inline const Aws::String& GetQualifier() const{ return m_qualifier; }
    inline bool QualifierHasBeenSet() const { return m_qualifierHasBeenSet; }
    inline void SetQualifier(const Aws::String& value) { m_qualifierHasBeenSet = true; m_qualifier = value; }
    inline void SetQualifier(Aws::String&& value) { m_qualifierHasBeenSet = true; m_qualifier = std::move(value); }
    inline void SetQualifier(const char* value) { m_qualifierHasBeenSet = true; m_qualifier.assign(value); }
    inline RemovePermissionRequest& WithQualifier(const Aws::String& value) { SetQualifier(value); return *this;}
    inline RemovePermissionRequest& WithQualifier(Aws::String&& value) { SetQualifier(std::move(value)); return *this;}
    inline RemovePermissionRequest& WithQualifier(const char* value) { SetQualifier(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Update the policy only if the revision ID matches the ID that's specified.
     * Use this option to avoid modifying a policy that has changed since you last read
     * it.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline bool RevisionIdHasBeenSet() const { return m_revisionIdHasBeenSet; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionIdHasBeenSet = true; m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionIdHasBeenSet = true; m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionIdHasBeenSet = true; m_revisionId.assign(value); }
    inline RemovePermissionRequest& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline RemovePermissionRequest& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline RemovePermissionRequest& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_statementId;
    bool m_statementIdHasBeenSet = false;

    Aws::String m_qualifier;
    bool m_qualifierHasBeenSet = false;

    Aws::String m_revisionId;
    bool m_revisionIdHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
