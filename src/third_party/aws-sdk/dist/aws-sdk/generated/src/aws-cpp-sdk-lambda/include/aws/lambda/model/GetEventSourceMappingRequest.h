/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class GetEventSourceMappingRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API GetEventSourceMappingRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetEventSourceMapping"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>The identifier of the event source mapping.</p>
     */
    inline const Aws::String& GetUUID() const{ return m_uUID; }
    inline bool UUIDHasBeenSet() const { return m_uUIDHasBeenSet; }
    inline void SetUUID(const Aws::String& value) { m_uUIDHasBeenSet = true; m_uUID = value; }
    inline void SetUUID(Aws::String&& value) { m_uUIDHasBeenSet = true; m_uUID = std::move(value); }
    inline void SetUUID(const char* value) { m_uUIDHasBeenSet = true; m_uUID.assign(value); }
    inline GetEventSourceMappingRequest& WithUUID(const Aws::String& value) { SetUUID(value); return *this;}
    inline GetEventSourceMappingRequest& WithUUID(Aws::String&& value) { SetUUID(std::move(value)); return *this;}
    inline GetEventSourceMappingRequest& WithUUID(const char* value) { SetUUID(value); return *this;}
    ///@}
  private:

    Aws::String m_uUID;
    bool m_uUIDHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
