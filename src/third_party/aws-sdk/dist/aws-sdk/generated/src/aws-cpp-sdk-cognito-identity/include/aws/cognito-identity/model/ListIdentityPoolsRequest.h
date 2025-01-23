/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/CognitoIdentityRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>Input to the ListIdentityPools action.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/ListIdentityPoolsInput">AWS
   * API Reference</a></p>
   */
  class ListIdentityPoolsRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API ListIdentityPoolsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListIdentityPools"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>The maximum number of identities to return.</p>
     */
    inline int GetMaxResults() const{ return m_maxResults; }
    inline bool MaxResultsHasBeenSet() const { return m_maxResultsHasBeenSet; }
    inline void SetMaxResults(int value) { m_maxResultsHasBeenSet = true; m_maxResults = value; }
    inline ListIdentityPoolsRequest& WithMaxResults(int value) { SetMaxResults(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A pagination token.</p>
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline bool NextTokenHasBeenSet() const { return m_nextTokenHasBeenSet; }
    inline void SetNextToken(const Aws::String& value) { m_nextTokenHasBeenSet = true; m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextTokenHasBeenSet = true; m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextTokenHasBeenSet = true; m_nextToken.assign(value); }
    inline ListIdentityPoolsRequest& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListIdentityPoolsRequest& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListIdentityPoolsRequest& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}
  private:

    int m_maxResults;
    bool m_maxResultsHasBeenSet = false;

    Aws::String m_nextToken;
    bool m_nextTokenHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
