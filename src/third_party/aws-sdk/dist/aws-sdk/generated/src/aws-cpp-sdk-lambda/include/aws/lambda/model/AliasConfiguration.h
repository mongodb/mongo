/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/AliasRoutingConfiguration.h>
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
   * <p>Provides configuration information about a Lambda function <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias</a>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/AliasConfiguration">AWS
   * API Reference</a></p>
   */
  class AliasConfiguration
  {
  public:
    AWS_LAMBDA_API AliasConfiguration();
    AWS_LAMBDA_API AliasConfiguration(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API AliasConfiguration& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the alias.</p>
     */
    inline const Aws::String& GetAliasArn() const{ return m_aliasArn; }
    inline bool AliasArnHasBeenSet() const { return m_aliasArnHasBeenSet; }
    inline void SetAliasArn(const Aws::String& value) { m_aliasArnHasBeenSet = true; m_aliasArn = value; }
    inline void SetAliasArn(Aws::String&& value) { m_aliasArnHasBeenSet = true; m_aliasArn = std::move(value); }
    inline void SetAliasArn(const char* value) { m_aliasArnHasBeenSet = true; m_aliasArn.assign(value); }
    inline AliasConfiguration& WithAliasArn(const Aws::String& value) { SetAliasArn(value); return *this;}
    inline AliasConfiguration& WithAliasArn(Aws::String&& value) { SetAliasArn(std::move(value)); return *this;}
    inline AliasConfiguration& WithAliasArn(const char* value) { SetAliasArn(value); return *this;}
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
    inline AliasConfiguration& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline AliasConfiguration& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline AliasConfiguration& WithName(const char* value) { SetName(value); return *this;}
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
    inline AliasConfiguration& WithFunctionVersion(const Aws::String& value) { SetFunctionVersion(value); return *this;}
    inline AliasConfiguration& WithFunctionVersion(Aws::String&& value) { SetFunctionVersion(std::move(value)); return *this;}
    inline AliasConfiguration& WithFunctionVersion(const char* value) { SetFunctionVersion(value); return *this;}
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
    inline AliasConfiguration& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline AliasConfiguration& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline AliasConfiguration& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-traffic-shifting-using-aliases.html">routing
     * configuration</a> of the alias.</p>
     */
    inline const AliasRoutingConfiguration& GetRoutingConfig() const{ return m_routingConfig; }
    inline bool RoutingConfigHasBeenSet() const { return m_routingConfigHasBeenSet; }
    inline void SetRoutingConfig(const AliasRoutingConfiguration& value) { m_routingConfigHasBeenSet = true; m_routingConfig = value; }
    inline void SetRoutingConfig(AliasRoutingConfiguration&& value) { m_routingConfigHasBeenSet = true; m_routingConfig = std::move(value); }
    inline AliasConfiguration& WithRoutingConfig(const AliasRoutingConfiguration& value) { SetRoutingConfig(value); return *this;}
    inline AliasConfiguration& WithRoutingConfig(AliasRoutingConfiguration&& value) { SetRoutingConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique identifier that changes when you update the alias.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline bool RevisionIdHasBeenSet() const { return m_revisionIdHasBeenSet; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionIdHasBeenSet = true; m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionIdHasBeenSet = true; m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionIdHasBeenSet = true; m_revisionId.assign(value); }
    inline AliasConfiguration& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline AliasConfiguration& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline AliasConfiguration& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline bool RequestIdHasBeenSet() const { return m_requestIdHasBeenSet; }
    inline void SetRequestId(const Aws::String& value) { m_requestIdHasBeenSet = true; m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestIdHasBeenSet = true; m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestIdHasBeenSet = true; m_requestId.assign(value); }
    inline AliasConfiguration& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline AliasConfiguration& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline AliasConfiguration& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_aliasArn;
    bool m_aliasArnHasBeenSet = false;

    Aws::String m_name;
    bool m_nameHasBeenSet = false;

    Aws::String m_functionVersion;
    bool m_functionVersionHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    AliasRoutingConfiguration m_routingConfig;
    bool m_routingConfigHasBeenSet = false;

    Aws::String m_revisionId;
    bool m_revisionIdHasBeenSet = false;

    Aws::String m_requestId;
    bool m_requestIdHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
