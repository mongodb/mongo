/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/SnapStartApplyOn.h>
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
   * href="https://docs.aws.amazon.com/lambda/latest/dg/snapstart.html">Lambda
   * SnapStart</a> setting. Set <code>ApplyOn</code> to
   * <code>PublishedVersions</code> to create a snapshot of the initialized execution
   * environment when you publish a function version.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/SnapStart">AWS
   * API Reference</a></p>
   */
  class SnapStart
  {
  public:
    AWS_LAMBDA_API SnapStart();
    AWS_LAMBDA_API SnapStart(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API SnapStart& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>Set to <code>PublishedVersions</code> to create a snapshot of the initialized
     * execution environment when you publish a function version.</p>
     */
    inline const SnapStartApplyOn& GetApplyOn() const{ return m_applyOn; }
    inline bool ApplyOnHasBeenSet() const { return m_applyOnHasBeenSet; }
    inline void SetApplyOn(const SnapStartApplyOn& value) { m_applyOnHasBeenSet = true; m_applyOn = value; }
    inline void SetApplyOn(SnapStartApplyOn&& value) { m_applyOnHasBeenSet = true; m_applyOn = std::move(value); }
    inline SnapStart& WithApplyOn(const SnapStartApplyOn& value) { SetApplyOn(value); return *this;}
    inline SnapStart& WithApplyOn(SnapStartApplyOn&& value) { SetApplyOn(std::move(value)); return *this;}
    ///@}
  private:

    SnapStartApplyOn m_applyOn;
    bool m_applyOnHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
