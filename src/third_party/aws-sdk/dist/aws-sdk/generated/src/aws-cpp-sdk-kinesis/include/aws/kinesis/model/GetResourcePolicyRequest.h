/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Kinesis
{
namespace Model
{

  /**
   */
  class GetResourcePolicyRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API GetResourcePolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetResourcePolicy"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the data stream or consumer.</p>
     */
    inline const Aws::String& GetResourceARN() const{ return m_resourceARN; }
    inline bool ResourceARNHasBeenSet() const { return m_resourceARNHasBeenSet; }
    inline void SetResourceARN(const Aws::String& value) { m_resourceARNHasBeenSet = true; m_resourceARN = value; }
    inline void SetResourceARN(Aws::String&& value) { m_resourceARNHasBeenSet = true; m_resourceARN = std::move(value); }
    inline void SetResourceARN(const char* value) { m_resourceARNHasBeenSet = true; m_resourceARN.assign(value); }
    inline GetResourcePolicyRequest& WithResourceARN(const Aws::String& value) { SetResourceARN(value); return *this;}
    inline GetResourcePolicyRequest& WithResourceARN(Aws::String&& value) { SetResourceARN(std::move(value)); return *this;}
    inline GetResourcePolicyRequest& WithResourceARN(const char* value) { SetResourceARN(value); return *this;}
    ///@}
  private:

    Aws::String m_resourceARN;
    bool m_resourceARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
