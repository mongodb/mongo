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
   * <p>The <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-eventsourcemapping.html#invocation-eventsourcemapping-provisioned-mode">
   * Provisioned Mode</a> configuration for the event source. Use Provisioned Mode to
   * customize the minimum and maximum number of event pollers for your event source.
   * An event poller is a compute unit that provides approximately 5 MBps of
   * throughput.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ProvisionedPollerConfig">AWS
   * API Reference</a></p>
   */
  class ProvisionedPollerConfig
  {
  public:
    AWS_LAMBDA_API ProvisionedPollerConfig();
    AWS_LAMBDA_API ProvisionedPollerConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API ProvisionedPollerConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The minimum number of event pollers this event source can scale down to.</p>
     */
    inline int GetMinimumPollers() const{ return m_minimumPollers; }
    inline bool MinimumPollersHasBeenSet() const { return m_minimumPollersHasBeenSet; }
    inline void SetMinimumPollers(int value) { m_minimumPollersHasBeenSet = true; m_minimumPollers = value; }
    inline ProvisionedPollerConfig& WithMinimumPollers(int value) { SetMinimumPollers(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of event pollers this event source can scale up to.</p>
     */
    inline int GetMaximumPollers() const{ return m_maximumPollers; }
    inline bool MaximumPollersHasBeenSet() const { return m_maximumPollersHasBeenSet; }
    inline void SetMaximumPollers(int value) { m_maximumPollersHasBeenSet = true; m_maximumPollers = value; }
    inline ProvisionedPollerConfig& WithMaximumPollers(int value) { SetMaximumPollers(value); return *this;}
    ///@}
  private:

    int m_minimumPollers;
    bool m_minimumPollersHasBeenSet = false;

    int m_maximumPollers;
    bool m_maximumPollersHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
