/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/FunctionUrlAuthType.h>
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
  class AddPermissionRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API AddPermissionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "AddPermission"; }

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
    inline AddPermissionRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline AddPermissionRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline AddPermissionRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A statement identifier that differentiates the statement from others in the
     * same policy.</p>
     */
    inline const Aws::String& GetStatementId() const{ return m_statementId; }
    inline bool StatementIdHasBeenSet() const { return m_statementIdHasBeenSet; }
    inline void SetStatementId(const Aws::String& value) { m_statementIdHasBeenSet = true; m_statementId = value; }
    inline void SetStatementId(Aws::String&& value) { m_statementIdHasBeenSet = true; m_statementId = std::move(value); }
    inline void SetStatementId(const char* value) { m_statementIdHasBeenSet = true; m_statementId.assign(value); }
    inline AddPermissionRequest& WithStatementId(const Aws::String& value) { SetStatementId(value); return *this;}
    inline AddPermissionRequest& WithStatementId(Aws::String&& value) { SetStatementId(std::move(value)); return *this;}
    inline AddPermissionRequest& WithStatementId(const char* value) { SetStatementId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The action that the principal can use on the function. For example,
     * <code>lambda:InvokeFunction</code> or <code>lambda:GetFunction</code>.</p>
     */
    inline const Aws::String& GetAction() const{ return m_action; }
    inline bool ActionHasBeenSet() const { return m_actionHasBeenSet; }
    inline void SetAction(const Aws::String& value) { m_actionHasBeenSet = true; m_action = value; }
    inline void SetAction(Aws::String&& value) { m_actionHasBeenSet = true; m_action = std::move(value); }
    inline void SetAction(const char* value) { m_actionHasBeenSet = true; m_action.assign(value); }
    inline AddPermissionRequest& WithAction(const Aws::String& value) { SetAction(value); return *this;}
    inline AddPermissionRequest& WithAction(Aws::String&& value) { SetAction(std::move(value)); return *this;}
    inline AddPermissionRequest& WithAction(const char* value) { SetAction(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Web Services service, Amazon Web Services account, IAM user, or
     * IAM role that invokes the function. If you specify a service, use
     * <code>SourceArn</code> or <code>SourceAccount</code> to limit who can invoke the
     * function through that service.</p>
     */
    inline const Aws::String& GetPrincipal() const{ return m_principal; }
    inline bool PrincipalHasBeenSet() const { return m_principalHasBeenSet; }
    inline void SetPrincipal(const Aws::String& value) { m_principalHasBeenSet = true; m_principal = value; }
    inline void SetPrincipal(Aws::String&& value) { m_principalHasBeenSet = true; m_principal = std::move(value); }
    inline void SetPrincipal(const char* value) { m_principalHasBeenSet = true; m_principal.assign(value); }
    inline AddPermissionRequest& WithPrincipal(const Aws::String& value) { SetPrincipal(value); return *this;}
    inline AddPermissionRequest& WithPrincipal(Aws::String&& value) { SetPrincipal(std::move(value)); return *this;}
    inline AddPermissionRequest& WithPrincipal(const char* value) { SetPrincipal(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>For Amazon Web Services services, the ARN of the Amazon Web Services resource
     * that invokes the function. For example, an Amazon S3 bucket or Amazon SNS
     * topic.</p> <p>Note that Lambda configures the comparison using the
     * <code>StringLike</code> operator.</p>
     */
    inline const Aws::String& GetSourceArn() const{ return m_sourceArn; }
    inline bool SourceArnHasBeenSet() const { return m_sourceArnHasBeenSet; }
    inline void SetSourceArn(const Aws::String& value) { m_sourceArnHasBeenSet = true; m_sourceArn = value; }
    inline void SetSourceArn(Aws::String&& value) { m_sourceArnHasBeenSet = true; m_sourceArn = std::move(value); }
    inline void SetSourceArn(const char* value) { m_sourceArnHasBeenSet = true; m_sourceArn.assign(value); }
    inline AddPermissionRequest& WithSourceArn(const Aws::String& value) { SetSourceArn(value); return *this;}
    inline AddPermissionRequest& WithSourceArn(Aws::String&& value) { SetSourceArn(std::move(value)); return *this;}
    inline AddPermissionRequest& WithSourceArn(const char* value) { SetSourceArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>For Amazon Web Services service, the ID of the Amazon Web Services account
     * that owns the resource. Use this together with <code>SourceArn</code> to ensure
     * that the specified account owns the resource. It is possible for an Amazon S3
     * bucket to be deleted by its owner and recreated by another account.</p>
     */
    inline const Aws::String& GetSourceAccount() const{ return m_sourceAccount; }
    inline bool SourceAccountHasBeenSet() const { return m_sourceAccountHasBeenSet; }
    inline void SetSourceAccount(const Aws::String& value) { m_sourceAccountHasBeenSet = true; m_sourceAccount = value; }
    inline void SetSourceAccount(Aws::String&& value) { m_sourceAccountHasBeenSet = true; m_sourceAccount = std::move(value); }
    inline void SetSourceAccount(const char* value) { m_sourceAccountHasBeenSet = true; m_sourceAccount.assign(value); }
    inline AddPermissionRequest& WithSourceAccount(const Aws::String& value) { SetSourceAccount(value); return *this;}
    inline AddPermissionRequest& WithSourceAccount(Aws::String&& value) { SetSourceAccount(std::move(value)); return *this;}
    inline AddPermissionRequest& WithSourceAccount(const char* value) { SetSourceAccount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>For Alexa Smart Home functions, a token that the invoker must supply.</p>
     */
    inline const Aws::String& GetEventSourceToken() const{ return m_eventSourceToken; }
    inline bool EventSourceTokenHasBeenSet() const { return m_eventSourceTokenHasBeenSet; }
    inline void SetEventSourceToken(const Aws::String& value) { m_eventSourceTokenHasBeenSet = true; m_eventSourceToken = value; }
    inline void SetEventSourceToken(Aws::String&& value) { m_eventSourceTokenHasBeenSet = true; m_eventSourceToken = std::move(value); }
    inline void SetEventSourceToken(const char* value) { m_eventSourceTokenHasBeenSet = true; m_eventSourceToken.assign(value); }
    inline AddPermissionRequest& WithEventSourceToken(const Aws::String& value) { SetEventSourceToken(value); return *this;}
    inline AddPermissionRequest& WithEventSourceToken(Aws::String&& value) { SetEventSourceToken(std::move(value)); return *this;}
    inline AddPermissionRequest& WithEventSourceToken(const char* value) { SetEventSourceToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify a version or alias to add permissions to a published version of the
     * function.</p>
     */
    inline const Aws::String& GetQualifier() const{ return m_qualifier; }
    inline bool QualifierHasBeenSet() const { return m_qualifierHasBeenSet; }
    inline void SetQualifier(const Aws::String& value) { m_qualifierHasBeenSet = true; m_qualifier = value; }
    inline void SetQualifier(Aws::String&& value) { m_qualifierHasBeenSet = true; m_qualifier = std::move(value); }
    inline void SetQualifier(const char* value) { m_qualifierHasBeenSet = true; m_qualifier.assign(value); }
    inline AddPermissionRequest& WithQualifier(const Aws::String& value) { SetQualifier(value); return *this;}
    inline AddPermissionRequest& WithQualifier(Aws::String&& value) { SetQualifier(std::move(value)); return *this;}
    inline AddPermissionRequest& WithQualifier(const char* value) { SetQualifier(value); return *this;}
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
    inline AddPermissionRequest& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline AddPermissionRequest& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline AddPermissionRequest& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The identifier for your organization in Organizations. Use this to grant
     * permissions to all the Amazon Web Services accounts under this organization.</p>
     */
    inline const Aws::String& GetPrincipalOrgID() const{ return m_principalOrgID; }
    inline bool PrincipalOrgIDHasBeenSet() const { return m_principalOrgIDHasBeenSet; }
    inline void SetPrincipalOrgID(const Aws::String& value) { m_principalOrgIDHasBeenSet = true; m_principalOrgID = value; }
    inline void SetPrincipalOrgID(Aws::String&& value) { m_principalOrgIDHasBeenSet = true; m_principalOrgID = std::move(value); }
    inline void SetPrincipalOrgID(const char* value) { m_principalOrgIDHasBeenSet = true; m_principalOrgID.assign(value); }
    inline AddPermissionRequest& WithPrincipalOrgID(const Aws::String& value) { SetPrincipalOrgID(value); return *this;}
    inline AddPermissionRequest& WithPrincipalOrgID(Aws::String&& value) { SetPrincipalOrgID(std::move(value)); return *this;}
    inline AddPermissionRequest& WithPrincipalOrgID(const char* value) { SetPrincipalOrgID(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of authentication that your function URL uses. Set to
     * <code>AWS_IAM</code> if you want to restrict access to authenticated users only.
     * Set to <code>NONE</code> if you want to bypass IAM authentication to create a
     * public endpoint. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/urls-auth.html">Security and
     * auth model for Lambda function URLs</a>.</p>
     */
    inline const FunctionUrlAuthType& GetFunctionUrlAuthType() const{ return m_functionUrlAuthType; }
    inline bool FunctionUrlAuthTypeHasBeenSet() const { return m_functionUrlAuthTypeHasBeenSet; }
    inline void SetFunctionUrlAuthType(const FunctionUrlAuthType& value) { m_functionUrlAuthTypeHasBeenSet = true; m_functionUrlAuthType = value; }
    inline void SetFunctionUrlAuthType(FunctionUrlAuthType&& value) { m_functionUrlAuthTypeHasBeenSet = true; m_functionUrlAuthType = std::move(value); }
    inline AddPermissionRequest& WithFunctionUrlAuthType(const FunctionUrlAuthType& value) { SetFunctionUrlAuthType(value); return *this;}
    inline AddPermissionRequest& WithFunctionUrlAuthType(FunctionUrlAuthType&& value) { SetFunctionUrlAuthType(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_statementId;
    bool m_statementIdHasBeenSet = false;

    Aws::String m_action;
    bool m_actionHasBeenSet = false;

    Aws::String m_principal;
    bool m_principalHasBeenSet = false;

    Aws::String m_sourceArn;
    bool m_sourceArnHasBeenSet = false;

    Aws::String m_sourceAccount;
    bool m_sourceAccountHasBeenSet = false;

    Aws::String m_eventSourceToken;
    bool m_eventSourceTokenHasBeenSet = false;

    Aws::String m_qualifier;
    bool m_qualifierHasBeenSet = false;

    Aws::String m_revisionId;
    bool m_revisionIdHasBeenSet = false;

    Aws::String m_principalOrgID;
    bool m_principalOrgIDHasBeenSet = false;

    FunctionUrlAuthType m_functionUrlAuthType;
    bool m_functionUrlAuthTypeHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
