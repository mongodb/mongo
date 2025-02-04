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
namespace Http
{
    class URI;
} //namespace Http
namespace Lambda
{
namespace Model
{

  /**
   */
  class ListAliasesRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API ListAliasesRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListAliases"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;

    AWS_LAMBDA_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function.</p> <p class="title"> <b>Name
     * formats</b> </p> <ul> <li> <p> <b>Function name</b> -
     * <code>MyFunction</code>.</p> </li> <li> <p> <b>Function ARN</b> -
     * <code>arn:aws:lambda:us-west-2:123456789012:function:MyFunction</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> -
     * <code>123456789012:function:MyFunction</code>.</p> </li> </ul> <p>The length
     * constraint applies only to the full ARN. If you specify only the function name,
     * it is limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline ListAliasesRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline ListAliasesRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline ListAliasesRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify a function version to only list aliases that invoke that version.</p>
     */
    inline const Aws::String& GetFunctionVersion() const{ return m_functionVersion; }
    inline bool FunctionVersionHasBeenSet() const { return m_functionVersionHasBeenSet; }
    inline void SetFunctionVersion(const Aws::String& value) { m_functionVersionHasBeenSet = true; m_functionVersion = value; }
    inline void SetFunctionVersion(Aws::String&& value) { m_functionVersionHasBeenSet = true; m_functionVersion = std::move(value); }
    inline void SetFunctionVersion(const char* value) { m_functionVersionHasBeenSet = true; m_functionVersion.assign(value); }
    inline ListAliasesRequest& WithFunctionVersion(const Aws::String& value) { SetFunctionVersion(value); return *this;}
    inline ListAliasesRequest& WithFunctionVersion(Aws::String&& value) { SetFunctionVersion(std::move(value)); return *this;}
    inline ListAliasesRequest& WithFunctionVersion(const char* value) { SetFunctionVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify the pagination token that's returned by a previous request to
     * retrieve the next page of results.</p>
     */
    inline const Aws::String& GetMarker() const{ return m_marker; }
    inline bool MarkerHasBeenSet() const { return m_markerHasBeenSet; }
    inline void SetMarker(const Aws::String& value) { m_markerHasBeenSet = true; m_marker = value; }
    inline void SetMarker(Aws::String&& value) { m_markerHasBeenSet = true; m_marker = std::move(value); }
    inline void SetMarker(const char* value) { m_markerHasBeenSet = true; m_marker.assign(value); }
    inline ListAliasesRequest& WithMarker(const Aws::String& value) { SetMarker(value); return *this;}
    inline ListAliasesRequest& WithMarker(Aws::String&& value) { SetMarker(std::move(value)); return *this;}
    inline ListAliasesRequest& WithMarker(const char* value) { SetMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Limit the number of aliases returned.</p>
     */
    inline int GetMaxItems() const{ return m_maxItems; }
    inline bool MaxItemsHasBeenSet() const { return m_maxItemsHasBeenSet; }
    inline void SetMaxItems(int value) { m_maxItemsHasBeenSet = true; m_maxItems = value; }
    inline ListAliasesRequest& WithMaxItems(int value) { SetMaxItems(value); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_functionVersion;
    bool m_functionVersionHasBeenSet = false;

    Aws::String m_marker;
    bool m_markerHasBeenSet = false;

    int m_maxItems;
    bool m_maxItemsHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
