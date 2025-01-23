/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>A function's environment variable settings. You can use environment variables
   * to adjust your function's behavior without updating code. An environment
   * variable is a pair of strings that are stored in a function's version-specific
   * configuration.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/Environment">AWS
   * API Reference</a></p>
   */
  class Environment
  {
  public:
    AWS_LAMBDA_API Environment();
    AWS_LAMBDA_API Environment(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Environment& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>Environment variable key-value pairs. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-envvars.html">Using
     * Lambda environment variables</a>.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetVariables() const{ return m_variables; }
    inline bool VariablesHasBeenSet() const { return m_variablesHasBeenSet; }
    inline void SetVariables(const Aws::Map<Aws::String, Aws::String>& value) { m_variablesHasBeenSet = true; m_variables = value; }
    inline void SetVariables(Aws::Map<Aws::String, Aws::String>&& value) { m_variablesHasBeenSet = true; m_variables = std::move(value); }
    inline Environment& WithVariables(const Aws::Map<Aws::String, Aws::String>& value) { SetVariables(value); return *this;}
    inline Environment& WithVariables(Aws::Map<Aws::String, Aws::String>&& value) { SetVariables(std::move(value)); return *this;}
    inline Environment& AddVariables(const Aws::String& key, const Aws::String& value) { m_variablesHasBeenSet = true; m_variables.emplace(key, value); return *this; }
    inline Environment& AddVariables(Aws::String&& key, const Aws::String& value) { m_variablesHasBeenSet = true; m_variables.emplace(std::move(key), value); return *this; }
    inline Environment& AddVariables(const Aws::String& key, Aws::String&& value) { m_variablesHasBeenSet = true; m_variables.emplace(key, std::move(value)); return *this; }
    inline Environment& AddVariables(Aws::String&& key, Aws::String&& value) { m_variablesHasBeenSet = true; m_variables.emplace(std::move(key), std::move(value)); return *this; }
    inline Environment& AddVariables(const char* key, Aws::String&& value) { m_variablesHasBeenSet = true; m_variables.emplace(key, std::move(value)); return *this; }
    inline Environment& AddVariables(Aws::String&& key, const char* value) { m_variablesHasBeenSet = true; m_variables.emplace(std::move(key), value); return *this; }
    inline Environment& AddVariables(const char* key, const char* value) { m_variablesHasBeenSet = true; m_variables.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::Map<Aws::String, Aws::String> m_variables;
    bool m_variablesHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
