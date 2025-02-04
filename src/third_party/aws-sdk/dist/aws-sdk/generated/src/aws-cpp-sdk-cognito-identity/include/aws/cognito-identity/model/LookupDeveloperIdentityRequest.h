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
   * <p>Input to the <code>LookupDeveloperIdentityInput</code> action.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/LookupDeveloperIdentityInput">AWS
   * API Reference</a></p>
   */
  class LookupDeveloperIdentityRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API LookupDeveloperIdentityRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "LookupDeveloperIdentity"; }

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
    inline LookupDeveloperIdentityRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline LookupDeveloperIdentityRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline LookupDeveloperIdentityRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline bool IdentityIdHasBeenSet() const { return m_identityIdHasBeenSet; }
    inline void SetIdentityId(const Aws::String& value) { m_identityIdHasBeenSet = true; m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityIdHasBeenSet = true; m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityIdHasBeenSet = true; m_identityId.assign(value); }
    inline LookupDeveloperIdentityRequest& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline LookupDeveloperIdentityRequest& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline LookupDeveloperIdentityRequest& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique ID used by your backend authentication process to identify a user.
     * Typically, a developer identity provider would issue many developer user
     * identifiers, in keeping with the number of users.</p>
     */
    inline const Aws::String& GetDeveloperUserIdentifier() const{ return m_developerUserIdentifier; }
    inline bool DeveloperUserIdentifierHasBeenSet() const { return m_developerUserIdentifierHasBeenSet; }
    inline void SetDeveloperUserIdentifier(const Aws::String& value) { m_developerUserIdentifierHasBeenSet = true; m_developerUserIdentifier = value; }
    inline void SetDeveloperUserIdentifier(Aws::String&& value) { m_developerUserIdentifierHasBeenSet = true; m_developerUserIdentifier = std::move(value); }
    inline void SetDeveloperUserIdentifier(const char* value) { m_developerUserIdentifierHasBeenSet = true; m_developerUserIdentifier.assign(value); }
    inline LookupDeveloperIdentityRequest& WithDeveloperUserIdentifier(const Aws::String& value) { SetDeveloperUserIdentifier(value); return *this;}
    inline LookupDeveloperIdentityRequest& WithDeveloperUserIdentifier(Aws::String&& value) { SetDeveloperUserIdentifier(std::move(value)); return *this;}
    inline LookupDeveloperIdentityRequest& WithDeveloperUserIdentifier(const char* value) { SetDeveloperUserIdentifier(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of identities to return.</p>
     */
    inline int GetMaxResults() const{ return m_maxResults; }
    inline bool MaxResultsHasBeenSet() const { return m_maxResultsHasBeenSet; }
    inline void SetMaxResults(int value) { m_maxResultsHasBeenSet = true; m_maxResults = value; }
    inline LookupDeveloperIdentityRequest& WithMaxResults(int value) { SetMaxResults(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A pagination token. The first call you make will have <code>NextToken</code>
     * set to null. After that the service will return <code>NextToken</code> values as
     * needed. For example, let's say you make a request with <code>MaxResults</code>
     * set to 10, and there are 20 matches in the database. The service will return a
     * pagination token as a part of the response. This token can be used to call the
     * API again and get results starting from the 11th match.</p>
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline bool NextTokenHasBeenSet() const { return m_nextTokenHasBeenSet; }
    inline void SetNextToken(const Aws::String& value) { m_nextTokenHasBeenSet = true; m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextTokenHasBeenSet = true; m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextTokenHasBeenSet = true; m_nextToken.assign(value); }
    inline LookupDeveloperIdentityRequest& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline LookupDeveloperIdentityRequest& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline LookupDeveloperIdentityRequest& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::String m_identityId;
    bool m_identityIdHasBeenSet = false;

    Aws::String m_developerUserIdentifier;
    bool m_developerUserIdentifierHasBeenSet = false;

    int m_maxResults;
    bool m_maxResultsHasBeenSet = false;

    Aws::String m_nextToken;
    bool m_nextTokenHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
