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
  class ListTagsRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API ListTagsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListTags"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>The resource's Amazon Resource Name (ARN). Note: Lambda does not support
     * adding tags to function aliases or versions.</p>
     */
    inline const Aws::String& GetResource() const{ return m_resource; }
    inline bool ResourceHasBeenSet() const { return m_resourceHasBeenSet; }
    inline void SetResource(const Aws::String& value) { m_resourceHasBeenSet = true; m_resource = value; }
    inline void SetResource(Aws::String&& value) { m_resourceHasBeenSet = true; m_resource = std::move(value); }
    inline void SetResource(const char* value) { m_resourceHasBeenSet = true; m_resource.assign(value); }
    inline ListTagsRequest& WithResource(const Aws::String& value) { SetResource(value); return *this;}
    inline ListTagsRequest& WithResource(Aws::String&& value) { SetResource(std::move(value)); return *this;}
    inline ListTagsRequest& WithResource(const char* value) { SetResource(value); return *this;}
    ///@}
  private:

    Aws::String m_resource;
    bool m_resourceHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
