/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/RuntimeVersionError.h>
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
   * <p>The ARN of the runtime and any errors that occured.</p><p><h3>See Also:</h3> 
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/RuntimeVersionConfig">AWS
   * API Reference</a></p>
   */
  class RuntimeVersionConfig
  {
  public:
    AWS_LAMBDA_API RuntimeVersionConfig();
    AWS_LAMBDA_API RuntimeVersionConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API RuntimeVersionConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The ARN of the runtime version you want the function to use.</p>
     */
    inline const Aws::String& GetRuntimeVersionArn() const{ return m_runtimeVersionArn; }
    inline bool RuntimeVersionArnHasBeenSet() const { return m_runtimeVersionArnHasBeenSet; }
    inline void SetRuntimeVersionArn(const Aws::String& value) { m_runtimeVersionArnHasBeenSet = true; m_runtimeVersionArn = value; }
    inline void SetRuntimeVersionArn(Aws::String&& value) { m_runtimeVersionArnHasBeenSet = true; m_runtimeVersionArn = std::move(value); }
    inline void SetRuntimeVersionArn(const char* value) { m_runtimeVersionArnHasBeenSet = true; m_runtimeVersionArn.assign(value); }
    inline RuntimeVersionConfig& WithRuntimeVersionArn(const Aws::String& value) { SetRuntimeVersionArn(value); return *this;}
    inline RuntimeVersionConfig& WithRuntimeVersionArn(Aws::String&& value) { SetRuntimeVersionArn(std::move(value)); return *this;}
    inline RuntimeVersionConfig& WithRuntimeVersionArn(const char* value) { SetRuntimeVersionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Error response when Lambda is unable to retrieve the runtime version for a
     * function.</p>
     */
    inline const RuntimeVersionError& GetError() const{ return m_error; }
    inline bool ErrorHasBeenSet() const { return m_errorHasBeenSet; }
    inline void SetError(const RuntimeVersionError& value) { m_errorHasBeenSet = true; m_error = value; }
    inline void SetError(RuntimeVersionError&& value) { m_errorHasBeenSet = true; m_error = std::move(value); }
    inline RuntimeVersionConfig& WithError(const RuntimeVersionError& value) { SetError(value); return *this;}
    inline RuntimeVersionConfig& WithError(RuntimeVersionError&& value) { SetError(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_runtimeVersionArn;
    bool m_runtimeVersionArnHasBeenSet = false;

    RuntimeVersionError m_error;
    bool m_errorHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
