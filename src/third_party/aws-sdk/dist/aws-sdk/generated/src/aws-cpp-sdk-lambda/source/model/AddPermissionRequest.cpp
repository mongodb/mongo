/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AddPermissionRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

AddPermissionRequest::AddPermissionRequest() : 
    m_functionNameHasBeenSet(false),
    m_statementIdHasBeenSet(false),
    m_actionHasBeenSet(false),
    m_principalHasBeenSet(false),
    m_sourceArnHasBeenSet(false),
    m_sourceAccountHasBeenSet(false),
    m_eventSourceTokenHasBeenSet(false),
    m_qualifierHasBeenSet(false),
    m_revisionIdHasBeenSet(false),
    m_principalOrgIDHasBeenSet(false),
    m_functionUrlAuthType(FunctionUrlAuthType::NOT_SET),
    m_functionUrlAuthTypeHasBeenSet(false)
{
}

Aws::String AddPermissionRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_statementIdHasBeenSet)
  {
   payload.WithString("StatementId", m_statementId);

  }

  if(m_actionHasBeenSet)
  {
   payload.WithString("Action", m_action);

  }

  if(m_principalHasBeenSet)
  {
   payload.WithString("Principal", m_principal);

  }

  if(m_sourceArnHasBeenSet)
  {
   payload.WithString("SourceArn", m_sourceArn);

  }

  if(m_sourceAccountHasBeenSet)
  {
   payload.WithString("SourceAccount", m_sourceAccount);

  }

  if(m_eventSourceTokenHasBeenSet)
  {
   payload.WithString("EventSourceToken", m_eventSourceToken);

  }

  if(m_revisionIdHasBeenSet)
  {
   payload.WithString("RevisionId", m_revisionId);

  }

  if(m_principalOrgIDHasBeenSet)
  {
   payload.WithString("PrincipalOrgID", m_principalOrgID);

  }

  if(m_functionUrlAuthTypeHasBeenSet)
  {
   payload.WithString("FunctionUrlAuthType", FunctionUrlAuthTypeMapper::GetNameForFunctionUrlAuthType(m_functionUrlAuthType));
  }

  return payload.View().WriteReadable();
}

void AddPermissionRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

}



