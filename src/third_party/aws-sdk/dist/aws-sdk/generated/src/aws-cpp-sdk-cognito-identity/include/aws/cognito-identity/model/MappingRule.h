/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/cognito-identity/model/MappingRuleMatchType.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>A rule that maps a claim name, a claim value, and a match type to a role
   * ARN.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/MappingRule">AWS
   * API Reference</a></p>
   */
  class MappingRule
  {
  public:
    AWS_COGNITOIDENTITY_API MappingRule();
    AWS_COGNITOIDENTITY_API MappingRule(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API MappingRule& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The claim name that must be present in the token, for example, "isAdmin" or
     * "paid".</p>
     */
    inline const Aws::String& GetClaim() const{ return m_claim; }
    inline bool ClaimHasBeenSet() const { return m_claimHasBeenSet; }
    inline void SetClaim(const Aws::String& value) { m_claimHasBeenSet = true; m_claim = value; }
    inline void SetClaim(Aws::String&& value) { m_claimHasBeenSet = true; m_claim = std::move(value); }
    inline void SetClaim(const char* value) { m_claimHasBeenSet = true; m_claim.assign(value); }
    inline MappingRule& WithClaim(const Aws::String& value) { SetClaim(value); return *this;}
    inline MappingRule& WithClaim(Aws::String&& value) { SetClaim(std::move(value)); return *this;}
    inline MappingRule& WithClaim(const char* value) { SetClaim(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The match condition that specifies how closely the claim value in the IdP
     * token must match <code>Value</code>.</p>
     */
    inline const MappingRuleMatchType& GetMatchType() const{ return m_matchType; }
    inline bool MatchTypeHasBeenSet() const { return m_matchTypeHasBeenSet; }
    inline void SetMatchType(const MappingRuleMatchType& value) { m_matchTypeHasBeenSet = true; m_matchType = value; }
    inline void SetMatchType(MappingRuleMatchType&& value) { m_matchTypeHasBeenSet = true; m_matchType = std::move(value); }
    inline MappingRule& WithMatchType(const MappingRuleMatchType& value) { SetMatchType(value); return *this;}
    inline MappingRule& WithMatchType(MappingRuleMatchType&& value) { SetMatchType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A brief string that the claim must match, for example, "paid" or "yes".</p>
     */
    inline const Aws::String& GetValue() const{ return m_value; }
    inline bool ValueHasBeenSet() const { return m_valueHasBeenSet; }
    inline void SetValue(const Aws::String& value) { m_valueHasBeenSet = true; m_value = value; }
    inline void SetValue(Aws::String&& value) { m_valueHasBeenSet = true; m_value = std::move(value); }
    inline void SetValue(const char* value) { m_valueHasBeenSet = true; m_value.assign(value); }
    inline MappingRule& WithValue(const Aws::String& value) { SetValue(value); return *this;}
    inline MappingRule& WithValue(Aws::String&& value) { SetValue(std::move(value)); return *this;}
    inline MappingRule& WithValue(const char* value) { SetValue(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The role ARN.</p>
     */
    inline const Aws::String& GetRoleARN() const{ return m_roleARN; }
    inline bool RoleARNHasBeenSet() const { return m_roleARNHasBeenSet; }
    inline void SetRoleARN(const Aws::String& value) { m_roleARNHasBeenSet = true; m_roleARN = value; }
    inline void SetRoleARN(Aws::String&& value) { m_roleARNHasBeenSet = true; m_roleARN = std::move(value); }
    inline void SetRoleARN(const char* value) { m_roleARNHasBeenSet = true; m_roleARN.assign(value); }
    inline MappingRule& WithRoleARN(const Aws::String& value) { SetRoleARN(value); return *this;}
    inline MappingRule& WithRoleARN(Aws::String&& value) { SetRoleARN(std::move(value)); return *this;}
    inline MappingRule& WithRoleARN(const char* value) { SetRoleARN(value); return *this;}
    ///@}
  private:

    Aws::String m_claim;
    bool m_claimHasBeenSet = false;

    MappingRuleMatchType m_matchType;
    bool m_matchTypeHasBeenSet = false;

    Aws::String m_value;
    bool m_valueHasBeenSet = false;

    Aws::String m_roleARN;
    bool m_roleARNHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
