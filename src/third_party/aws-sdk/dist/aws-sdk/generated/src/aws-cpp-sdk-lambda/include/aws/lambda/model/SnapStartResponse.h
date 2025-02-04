/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/SnapStartApplyOn.h>
#include <aws/lambda/model/SnapStartOptimizationStatus.h>
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
   * href="https://docs.aws.amazon.com/lambda/latest/dg/snapstart.html">SnapStart</a>
   * setting.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/SnapStartResponse">AWS
   * API Reference</a></p>
   */
  class SnapStartResponse
  {
  public:
    AWS_LAMBDA_API SnapStartResponse();
    AWS_LAMBDA_API SnapStartResponse(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API SnapStartResponse& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>When set to <code>PublishedVersions</code>, Lambda creates a snapshot of the
     * execution environment when you publish a function version.</p>
     */
    inline const SnapStartApplyOn& GetApplyOn() const{ return m_applyOn; }
    inline bool ApplyOnHasBeenSet() const { return m_applyOnHasBeenSet; }
    inline void SetApplyOn(const SnapStartApplyOn& value) { m_applyOnHasBeenSet = true; m_applyOn = value; }
    inline void SetApplyOn(SnapStartApplyOn&& value) { m_applyOnHasBeenSet = true; m_applyOn = std::move(value); }
    inline SnapStartResponse& WithApplyOn(const SnapStartApplyOn& value) { SetApplyOn(value); return *this;}
    inline SnapStartResponse& WithApplyOn(SnapStartApplyOn&& value) { SetApplyOn(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>When you provide a <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-versions.html#versioning-versions-using">qualified
     * Amazon Resource Name (ARN)</a>, this response element indicates whether
     * SnapStart is activated for the specified function version.</p>
     */
    inline const SnapStartOptimizationStatus& GetOptimizationStatus() const{ return m_optimizationStatus; }
    inline bool OptimizationStatusHasBeenSet() const { return m_optimizationStatusHasBeenSet; }
    inline void SetOptimizationStatus(const SnapStartOptimizationStatus& value) { m_optimizationStatusHasBeenSet = true; m_optimizationStatus = value; }
    inline void SetOptimizationStatus(SnapStartOptimizationStatus&& value) { m_optimizationStatusHasBeenSet = true; m_optimizationStatus = std::move(value); }
    inline SnapStartResponse& WithOptimizationStatus(const SnapStartOptimizationStatus& value) { SetOptimizationStatus(value); return *this;}
    inline SnapStartResponse& WithOptimizationStatus(SnapStartOptimizationStatus&& value) { SetOptimizationStatus(std::move(value)); return *this;}
    ///@}
  private:

    SnapStartApplyOn m_applyOn;
    bool m_applyOnHasBeenSet = false;

    SnapStartOptimizationStatus m_optimizationStatus;
    bool m_optimizationStatusHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
