/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/AllowedPublishers.h>
#include <aws/lambda/model/CodeSigningPolicies.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class CreateCodeSigningConfigRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API CreateCodeSigningConfigRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateCodeSigningConfig"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>Descriptive name for this code signing configuration.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline CreateCodeSigningConfigRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CreateCodeSigningConfigRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CreateCodeSigningConfigRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Signing profiles for this code signing configuration.</p>
     */
    inline const AllowedPublishers& GetAllowedPublishers() const{ return m_allowedPublishers; }
    inline bool AllowedPublishersHasBeenSet() const { return m_allowedPublishersHasBeenSet; }
    inline void SetAllowedPublishers(const AllowedPublishers& value) { m_allowedPublishersHasBeenSet = true; m_allowedPublishers = value; }
    inline void SetAllowedPublishers(AllowedPublishers&& value) { m_allowedPublishersHasBeenSet = true; m_allowedPublishers = std::move(value); }
    inline CreateCodeSigningConfigRequest& WithAllowedPublishers(const AllowedPublishers& value) { SetAllowedPublishers(value); return *this;}
    inline CreateCodeSigningConfigRequest& WithAllowedPublishers(AllowedPublishers&& value) { SetAllowedPublishers(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The code signing policies define the actions to take if the validation checks
     * fail. </p>
     */
    inline const CodeSigningPolicies& GetCodeSigningPolicies() const{ return m_codeSigningPolicies; }
    inline bool CodeSigningPoliciesHasBeenSet() const { return m_codeSigningPoliciesHasBeenSet; }
    inline void SetCodeSigningPolicies(const CodeSigningPolicies& value) { m_codeSigningPoliciesHasBeenSet = true; m_codeSigningPolicies = value; }
    inline void SetCodeSigningPolicies(CodeSigningPolicies&& value) { m_codeSigningPoliciesHasBeenSet = true; m_codeSigningPolicies = std::move(value); }
    inline CreateCodeSigningConfigRequest& WithCodeSigningPolicies(const CodeSigningPolicies& value) { SetCodeSigningPolicies(value); return *this;}
    inline CreateCodeSigningConfigRequest& WithCodeSigningPolicies(CodeSigningPolicies&& value) { SetCodeSigningPolicies(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags to add to the code signing configuration.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Map<Aws::String, Aws::String>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Map<Aws::String, Aws::String>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline CreateCodeSigningConfigRequest& WithTags(const Aws::Map<Aws::String, Aws::String>& value) { SetTags(value); return *this;}
    inline CreateCodeSigningConfigRequest& WithTags(Aws::Map<Aws::String, Aws::String>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateCodeSigningConfigRequest& AddTags(const Aws::String& key, const Aws::String& value) { m_tagsHasBeenSet = true; m_tags.emplace(key, value); return *this; }
    inline CreateCodeSigningConfigRequest& AddTags(Aws::String&& key, const Aws::String& value) { m_tagsHasBeenSet = true; m_tags.emplace(std::move(key), value); return *this; }
    inline CreateCodeSigningConfigRequest& AddTags(const Aws::String& key, Aws::String&& value) { m_tagsHasBeenSet = true; m_tags.emplace(key, std::move(value)); return *this; }
    inline CreateCodeSigningConfigRequest& AddTags(Aws::String&& key, Aws::String&& value) { m_tagsHasBeenSet = true; m_tags.emplace(std::move(key), std::move(value)); return *this; }
    inline CreateCodeSigningConfigRequest& AddTags(const char* key, Aws::String&& value) { m_tagsHasBeenSet = true; m_tags.emplace(key, std::move(value)); return *this; }
    inline CreateCodeSigningConfigRequest& AddTags(Aws::String&& key, const char* value) { m_tagsHasBeenSet = true; m_tags.emplace(std::move(key), value); return *this; }
    inline CreateCodeSigningConfigRequest& AddTags(const char* key, const char* value) { m_tagsHasBeenSet = true; m_tags.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    AllowedPublishers m_allowedPublishers;
    bool m_allowedPublishersHasBeenSet = false;

    CodeSigningPolicies m_codeSigningPolicies;
    bool m_codeSigningPoliciesHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
