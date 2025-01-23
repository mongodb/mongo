/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/AllowedPublishers.h>
#include <aws/lambda/model/CodeSigningPolicies.h>
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
namespace Lambda
{
namespace Model
{

  /**
   * <p>Details about a <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-codesigning.html">Code
   * signing configuration</a>. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CodeSigningConfig">AWS
   * API Reference</a></p>
   */
  class CodeSigningConfig
  {
  public:
    AWS_LAMBDA_API CodeSigningConfig();
    AWS_LAMBDA_API CodeSigningConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API CodeSigningConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>Unique identifer for the Code signing configuration.</p>
     */
    inline const Aws::String& GetCodeSigningConfigId() const{ return m_codeSigningConfigId; }
    inline bool CodeSigningConfigIdHasBeenSet() const { return m_codeSigningConfigIdHasBeenSet; }
    inline void SetCodeSigningConfigId(const Aws::String& value) { m_codeSigningConfigIdHasBeenSet = true; m_codeSigningConfigId = value; }
    inline void SetCodeSigningConfigId(Aws::String&& value) { m_codeSigningConfigIdHasBeenSet = true; m_codeSigningConfigId = std::move(value); }
    inline void SetCodeSigningConfigId(const char* value) { m_codeSigningConfigIdHasBeenSet = true; m_codeSigningConfigId.assign(value); }
    inline CodeSigningConfig& WithCodeSigningConfigId(const Aws::String& value) { SetCodeSigningConfigId(value); return *this;}
    inline CodeSigningConfig& WithCodeSigningConfigId(Aws::String&& value) { SetCodeSigningConfigId(std::move(value)); return *this;}
    inline CodeSigningConfig& WithCodeSigningConfigId(const char* value) { SetCodeSigningConfigId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the Code signing configuration.</p>
     */
    inline const Aws::String& GetCodeSigningConfigArn() const{ return m_codeSigningConfigArn; }
    inline bool CodeSigningConfigArnHasBeenSet() const { return m_codeSigningConfigArnHasBeenSet; }
    inline void SetCodeSigningConfigArn(const Aws::String& value) { m_codeSigningConfigArnHasBeenSet = true; m_codeSigningConfigArn = value; }
    inline void SetCodeSigningConfigArn(Aws::String&& value) { m_codeSigningConfigArnHasBeenSet = true; m_codeSigningConfigArn = std::move(value); }
    inline void SetCodeSigningConfigArn(const char* value) { m_codeSigningConfigArnHasBeenSet = true; m_codeSigningConfigArn.assign(value); }
    inline CodeSigningConfig& WithCodeSigningConfigArn(const Aws::String& value) { SetCodeSigningConfigArn(value); return *this;}
    inline CodeSigningConfig& WithCodeSigningConfigArn(Aws::String&& value) { SetCodeSigningConfigArn(std::move(value)); return *this;}
    inline CodeSigningConfig& WithCodeSigningConfigArn(const char* value) { SetCodeSigningConfigArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Code signing configuration description.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline CodeSigningConfig& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CodeSigningConfig& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CodeSigningConfig& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>List of allowed publishers.</p>
     */
    inline const AllowedPublishers& GetAllowedPublishers() const{ return m_allowedPublishers; }
    inline bool AllowedPublishersHasBeenSet() const { return m_allowedPublishersHasBeenSet; }
    inline void SetAllowedPublishers(const AllowedPublishers& value) { m_allowedPublishersHasBeenSet = true; m_allowedPublishers = value; }
    inline void SetAllowedPublishers(AllowedPublishers&& value) { m_allowedPublishersHasBeenSet = true; m_allowedPublishers = std::move(value); }
    inline CodeSigningConfig& WithAllowedPublishers(const AllowedPublishers& value) { SetAllowedPublishers(value); return *this;}
    inline CodeSigningConfig& WithAllowedPublishers(AllowedPublishers&& value) { SetAllowedPublishers(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The code signing policy controls the validation failure action for signature
     * mismatch or expiry.</p>
     */
    inline const CodeSigningPolicies& GetCodeSigningPolicies() const{ return m_codeSigningPolicies; }
    inline bool CodeSigningPoliciesHasBeenSet() const { return m_codeSigningPoliciesHasBeenSet; }
    inline void SetCodeSigningPolicies(const CodeSigningPolicies& value) { m_codeSigningPoliciesHasBeenSet = true; m_codeSigningPolicies = value; }
    inline void SetCodeSigningPolicies(CodeSigningPolicies&& value) { m_codeSigningPoliciesHasBeenSet = true; m_codeSigningPolicies = std::move(value); }
    inline CodeSigningConfig& WithCodeSigningPolicies(const CodeSigningPolicies& value) { SetCodeSigningPolicies(value); return *this;}
    inline CodeSigningConfig& WithCodeSigningPolicies(CodeSigningPolicies&& value) { SetCodeSigningPolicies(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time that the Code signing configuration was last modified, in
     * ISO-8601 format (YYYY-MM-DDThh:mm:ss.sTZD). </p>
     */
    inline const Aws::String& GetLastModified() const{ return m_lastModified; }
    inline bool LastModifiedHasBeenSet() const { return m_lastModifiedHasBeenSet; }
    inline void SetLastModified(const Aws::String& value) { m_lastModifiedHasBeenSet = true; m_lastModified = value; }
    inline void SetLastModified(Aws::String&& value) { m_lastModifiedHasBeenSet = true; m_lastModified = std::move(value); }
    inline void SetLastModified(const char* value) { m_lastModifiedHasBeenSet = true; m_lastModified.assign(value); }
    inline CodeSigningConfig& WithLastModified(const Aws::String& value) { SetLastModified(value); return *this;}
    inline CodeSigningConfig& WithLastModified(Aws::String&& value) { SetLastModified(std::move(value)); return *this;}
    inline CodeSigningConfig& WithLastModified(const char* value) { SetLastModified(value); return *this;}
    ///@}
  private:

    Aws::String m_codeSigningConfigId;
    bool m_codeSigningConfigIdHasBeenSet = false;

    Aws::String m_codeSigningConfigArn;
    bool m_codeSigningConfigArnHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    AllowedPublishers m_allowedPublishers;
    bool m_allowedPublishersHasBeenSet = false;

    CodeSigningPolicies m_codeSigningPolicies;
    bool m_codeSigningPoliciesHasBeenSet = false;

    Aws::String m_lastModified;
    bool m_lastModifiedHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
