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
   * <p>Input to the ListIdentities action.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/ListIdentitiesInput">AWS
   * API Reference</a></p>
   */
  class ListIdentitiesRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API ListIdentitiesRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListIdentities"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>An identity pool ID in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline bool IdentityPoolIdHasBeenSet() const { return m_identityPoolIdHasBeenSet; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId.assign(value); }
    inline ListIdentitiesRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline ListIdentitiesRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline ListIdentitiesRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of identities to return.</p>
     */
    inline int GetMaxResults() const{ return m_maxResults; }
    inline bool MaxResultsHasBeenSet() const { return m_maxResultsHasBeenSet; }
    inline void SetMaxResults(int value) { m_maxResultsHasBeenSet = true; m_maxResults = value; }
    inline ListIdentitiesRequest& WithMaxResults(int value) { SetMaxResults(value); return *this;}
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
    inline ListIdentitiesRequest& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListIdentitiesRequest& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListIdentitiesRequest& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An optional boolean parameter that allows you to hide disabled identities. If
     * omitted, the ListIdentities API will include disabled identities in the
     * response.</p>
     */
    inline bool GetHideDisabled() const{ return m_hideDisabled; }
    inline bool HideDisabledHasBeenSet() const { return m_hideDisabledHasBeenSet; }
    inline void SetHideDisabled(bool value) { m_hideDisabledHasBeenSet = true; m_hideDisabled = value; }
    inline ListIdentitiesRequest& WithHideDisabled(bool value) { SetHideDisabled(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    int m_maxResults;
    bool m_maxResultsHasBeenSet = false;

    Aws::String m_nextToken;
    bool m_nextTokenHasBeenSet = false;

    bool m_hideDisabled;
    bool m_hideDisabledHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
