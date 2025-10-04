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
  class CreateAccountAliasRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreateAccountAliasRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateAccountAlias"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The account alias to create.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of lowercase letters, digits, and dashes. You cannot start or finish
     * with a dash, nor can you have two dashes in a row.</p>
     */
    inline const Aws::String& GetAccountAlias() const{ return m_accountAlias; }
    inline bool AccountAliasHasBeenSet() const { return m_accountAliasHasBeenSet; }
    inline void SetAccountAlias(const Aws::String& value) { m_accountAliasHasBeenSet = true; m_accountAlias = value; }
    inline void SetAccountAlias(Aws::String&& value) { m_accountAliasHasBeenSet = true; m_accountAlias = std::move(value); }
    inline void SetAccountAlias(const char* value) { m_accountAliasHasBeenSet = true; m_accountAlias.assign(value); }
    inline CreateAccountAliasRequest& WithAccountAlias(const Aws::String& value) { SetAccountAlias(value); return *this;}
    inline CreateAccountAliasRequest& WithAccountAlias(Aws::String&& value) { SetAccountAlias(std::move(value)); return *this;}
    inline CreateAccountAliasRequest& WithAccountAlias(const char* value) { SetAccountAlias(value); return *this;}
    ///@}
  private:

    Aws::String m_accountAlias;
    bool m_accountAliasHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
