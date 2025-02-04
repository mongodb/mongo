/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateAccountAliasRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateAccountAliasRequest::CreateAccountAliasRequest() : 
    m_accountAliasHasBeenSet(false)
{
}

Aws::String CreateAccountAliasRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateAccountAlias&";
  if(m_accountAliasHasBeenSet)
  {
    ss << "AccountAlias=" << StringUtils::URLEncode(m_accountAlias.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  CreateAccountAliasRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
