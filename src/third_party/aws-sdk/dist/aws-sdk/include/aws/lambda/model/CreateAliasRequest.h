/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/AliasRoutingConfiguration.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class CreateAliasRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API CreateAliasRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateAlias"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function.</p> <p class="title"> <b>Name
     * formats</b> </p> <ul> <li> <p> <b>Function name</b> -
     * <code>MyFunction</code>.</p> </li> <li> <p> <b>Function ARN</b> -
     * <code>arn:aws:lambda:us-west-2:123456789012:function:MyFunction</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> -
     * <code>123456789012:function:MyFunction</code>.</p> </li> </ul> <p>The length
     * constraint applies only to the full ARN. If you specify only the function name,
     * it is limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline CreateAliasRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline CreateAliasRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline CreateAliasRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the alias.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline bool NameHasBeenSet() const { return m_nameHasBeenSet; }
    inline void SetName(const Aws::String& value) { m_nameHasBeenSet = true; m_name = value; }
    inline void SetName(Aws::String&& value) { m_nameHasBeenSet = true; m_name = std::move(value); }
    inline void SetName(const char* value) { m_nameHasBeenSet = true; m_name.assign(value); }
    inline CreateAliasRequest& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline CreateAliasRequest& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline CreateAliasRequest& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function version that the alias invokes.</p>
     */
    inline const Aws::String& GetFunctionVersion() const{ return m_functionVersion; }
    inline bool FunctionVersionHasBeenSet() const { return m_functionVersionHasBeenSet; }
    inline void SetFunctionVersion(const Aws::String& value) { m_functionVersionHasBeenSet = true; m_functionVersion = value; }
    inline void SetFunctionVersion(Aws::String&& value) { m_functionVersionHasBeenSet = true; m_functionVersion = std::move(value); }
    inline void SetFunctionVersion(const char* value) { m_functionVersionHasBeenSet = true; m_functionVersion.assign(value); }
    inline CreateAliasRequest& WithFunctionVersion(const Aws::String& value) { SetFunctionVersion(value); return *this;}
    inline CreateAliasRequest& WithFunctionVersion(Aws::String&& value) { SetFunctionVersion(std::move(value)); return *this;}
    inline CreateAliasRequest& WithFunctionVersion(const char* value) { SetFunctionVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A description of the alias.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline CreateAliasRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CreateAliasRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CreateAliasRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html#configuring-alias-routing">routing
     * configuration</a> of the alias.</p>
     */
    inline const AliasRoutingConfiguration& GetRoutingConfig() const{ return m_routingConfig; }
    inline bool RoutingConfigHasBeenSet() const { return m_routingConfigHasBeenSet; }
    inline void SetRoutingConfig(const AliasRoutingConfiguration& value) { m_routingConfigHasBeenSet = true; m_routingConfig = value; }
    inline void SetRoutingConfig(AliasRoutingConfiguration&& value) { m_routingConfigHasBeenSet = true; m_routingConfig = std::move(value); }
    inline CreateAliasRequest& WithRoutingConfig(const AliasRoutingConfiguration& value) { SetRoutingConfig(value); return *this;}
    inline CreateAliasRequest& WithRoutingConfig(AliasRoutingConfiguration&& value) { SetRoutingConfig(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_name;
    bool m_nameHasBeenSet = false;

    Aws::String m_functionVersion;
    bool m_functionVersionHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    AliasRoutingConfiguration m_routingConfig;
    bool m_routingConfigHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
