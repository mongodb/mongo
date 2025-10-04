/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class GetContextKeysForPrincipalPolicyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API GetContextKeysForPrincipalPolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetContextKeysForPrincipalPolicy"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The ARN of a user, group, or role whose policies contain the context keys
     * that you want listed. If you specify a user, the list includes context keys that
     * are found in all policies that are attached to the user. The list also includes
     * all groups that the user is a member of. If you pick a group or a role, then it
     * includes only those context keys that are found in policies attached to that
     * entity. Note that all parameters are shown in unencoded form here for clarity,
     * but must be URL encoded to be included as a part of a real HTML request.</p>
     * <p>For more information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetPolicySourceArn() const{ return m_policySourceArn; }
    inline bool PolicySourceArnHasBeenSet() const { return m_policySourceArnHasBeenSet; }
    inline void SetPolicySourceArn(const Aws::String& value) { m_policySourceArnHasBeenSet = true; m_policySourceArn = value; }
    inline void SetPolicySourceArn(Aws::String&& value) { m_policySourceArnHasBeenSet = true; m_policySourceArn = std::move(value); }
    inline void SetPolicySourceArn(const char* value) { m_policySourceArnHasBeenSet = true; m_policySourceArn.assign(value); }
    inline GetContextKeysForPrincipalPolicyRequest& WithPolicySourceArn(const Aws::String& value) { SetPolicySourceArn(value); return *this;}
    inline GetContextKeysForPrincipalPolicyRequest& WithPolicySourceArn(Aws::String&& value) { SetPolicySourceArn(std::move(value)); return *this;}
    inline GetContextKeysForPrincipalPolicyRequest& WithPolicySourceArn(const char* value) { SetPolicySourceArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An optional list of additional policies for which you want the list of
     * context keys that are referenced.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::Vector<Aws::String>& GetPolicyInputList() const{ return m_policyInputList; }
    inline bool PolicyInputListHasBeenSet() const { return m_policyInputListHasBeenSet; }
    inline void SetPolicyInputList(const Aws::Vector<Aws::String>& value) { m_policyInputListHasBeenSet = true; m_policyInputList = value; }
    inline void SetPolicyInputList(Aws::Vector<Aws::String>&& value) { m_policyInputListHasBeenSet = true; m_policyInputList = std::move(value); }
    inline GetContextKeysForPrincipalPolicyRequest& WithPolicyInputList(const Aws::Vector<Aws::String>& value) { SetPolicyInputList(value); return *this;}
    inline GetContextKeysForPrincipalPolicyRequest& WithPolicyInputList(Aws::Vector<Aws::String>&& value) { SetPolicyInputList(std::move(value)); return *this;}
    inline GetContextKeysForPrincipalPolicyRequest& AddPolicyInputList(const Aws::String& value) { m_policyInputListHasBeenSet = true; m_policyInputList.push_back(value); return *this; }
    inline GetContextKeysForPrincipalPolicyRequest& AddPolicyInputList(Aws::String&& value) { m_policyInputListHasBeenSet = true; m_policyInputList.push_back(std::move(value)); return *this; }
    inline GetContextKeysForPrincipalPolicyRequest& AddPolicyInputList(const char* value) { m_policyInputListHasBeenSet = true; m_policyInputList.push_back(value); return *this; }
    ///@}
  private:

    Aws::String m_policySourceArn;
    bool m_policySourceArnHasBeenSet = false;

    Aws::Vector<Aws::String> m_policyInputList;
    bool m_policyInputListHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
