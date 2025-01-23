/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/TracingMode.h>
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
   * <p>The function's <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/services-xray.html">X-Ray</a>
   * tracing configuration. To sample and record incoming requests, set
   * <code>Mode</code> to <code>Active</code>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/TracingConfig">AWS
   * API Reference</a></p>
   */
  class TracingConfig
  {
  public:
    AWS_LAMBDA_API TracingConfig();
    AWS_LAMBDA_API TracingConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API TracingConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The tracing mode.</p>
     */
    inline const TracingMode& GetMode() const{ return m_mode; }
    inline bool ModeHasBeenSet() const { return m_modeHasBeenSet; }
    inline void SetMode(const TracingMode& value) { m_modeHasBeenSet = true; m_mode = value; }
    inline void SetMode(TracingMode&& value) { m_modeHasBeenSet = true; m_mode = std::move(value); }
    inline TracingConfig& WithMode(const TracingMode& value) { SetMode(value); return *this;}
    inline TracingConfig& WithMode(TracingMode&& value) { SetMode(std::move(value)); return *this;}
    ///@}
  private:

    TracingMode m_mode;
    bool m_modeHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
