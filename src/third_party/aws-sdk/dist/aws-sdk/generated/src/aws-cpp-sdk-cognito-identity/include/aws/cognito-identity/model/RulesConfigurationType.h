/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/cognito-identity/model/MappingRule.h>
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
   * <p>A container for rules.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/RulesConfigurationType">AWS
   * API Reference</a></p>
   */
  class RulesConfigurationType
  {
  public:
    AWS_COGNITOIDENTITY_API RulesConfigurationType();
    AWS_COGNITOIDENTITY_API RulesConfigurationType(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API RulesConfigurationType& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>An array of rules. You can specify up to 25 rules per identity provider.</p>
     * <p>Rules are evaluated in order. The first one to match specifies the role.</p>
     */
    inline const Aws::Vector<MappingRule>& GetRules() const{ return m_rules; }
    inline bool RulesHasBeenSet() const { return m_rulesHasBeenSet; }
    inline void SetRules(const Aws::Vector<MappingRule>& value) { m_rulesHasBeenSet = true; m_rules = value; }
    inline void SetRules(Aws::Vector<MappingRule>&& value) { m_rulesHasBeenSet = true; m_rules = std::move(value); }
    inline RulesConfigurationType& WithRules(const Aws::Vector<MappingRule>& value) { SetRules(value); return *this;}
    inline RulesConfigurationType& WithRules(Aws::Vector<MappingRule>&& value) { SetRules(std::move(value)); return *this;}
    inline RulesConfigurationType& AddRules(const MappingRule& value) { m_rulesHasBeenSet = true; m_rules.push_back(value); return *this; }
    inline RulesConfigurationType& AddRules(MappingRule&& value) { m_rulesHasBeenSet = true; m_rules.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<MappingRule> m_rules;
    bool m_rulesHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
