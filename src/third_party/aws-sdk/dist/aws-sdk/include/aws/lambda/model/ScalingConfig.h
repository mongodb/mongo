/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>

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
   * <p>(Amazon SQS only) The scaling configuration for the event source. To remove
   * the configuration, pass an empty value.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ScalingConfig">AWS
   * API Reference</a></p>
   */
  class ScalingConfig
  {
  public:
    AWS_LAMBDA_API ScalingConfig();
    AWS_LAMBDA_API ScalingConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API ScalingConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>Limits the number of concurrent instances that the Amazon SQS event source
     * can invoke.</p>
     */
    inline int GetMaximumConcurrency() const{ return m_maximumConcurrency; }
    inline bool MaximumConcurrencyHasBeenSet() const { return m_maximumConcurrencyHasBeenSet; }
    inline void SetMaximumConcurrency(int value) { m_maximumConcurrencyHasBeenSet = true; m_maximumConcurrency = value; }
    inline ScalingConfig& WithMaximumConcurrency(int value) { SetMaximumConcurrency(value); return *this;}
    ///@}
  private:

    int m_maximumConcurrency;
    bool m_maximumConcurrencyHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
