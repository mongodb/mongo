/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/CognitoIdentityRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <utility>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>Input to the UnlinkIdentity action.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/UnlinkIdentityInput">AWS
   * API Reference</a></p>
   */
  class UnlinkIdentityRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API UnlinkIdentityRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UnlinkIdentity"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline bool IdentityIdHasBeenSet() const { return m_identityIdHasBeenSet; }
    inline void SetIdentityId(const Aws::String& value) { m_identityIdHasBeenSet = true; m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityIdHasBeenSet = true; m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityIdHasBeenSet = true; m_identityId.assign(value); }
    inline UnlinkIdentityRequest& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline UnlinkIdentityRequest& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline UnlinkIdentityRequest& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A set of optional name-value pairs that map provider names to provider
     * tokens.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetLogins() const{ return m_logins; }
    inline bool LoginsHasBeenSet() const { return m_loginsHasBeenSet; }
    inline void SetLogins(const Aws::Map<Aws::String, Aws::String>& value) { m_loginsHasBeenSet = true; m_logins = value; }
    inline void SetLogins(Aws::Map<Aws::String, Aws::String>&& value) { m_loginsHasBeenSet = true; m_logins = std::move(value); }
    inline UnlinkIdentityRequest& WithLogins(const Aws::Map<Aws::String, Aws::String>& value) { SetLogins(value); return *this;}
    inline UnlinkIdentityRequest& WithLogins(Aws::Map<Aws::String, Aws::String>&& value) { SetLogins(std::move(value)); return *this;}
    inline UnlinkIdentityRequest& AddLogins(const Aws::String& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    inline UnlinkIdentityRequest& AddLogins(Aws::String&& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline UnlinkIdentityRequest& AddLogins(const Aws::String& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline UnlinkIdentityRequest& AddLogins(Aws::String&& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), std::move(value)); return *this; }
    inline UnlinkIdentityRequest& AddLogins(const char* key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline UnlinkIdentityRequest& AddLogins(Aws::String&& key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline UnlinkIdentityRequest& AddLogins(const char* key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Provider names to unlink from this identity.</p>
     */
    inline const Aws::Vector<Aws::String>& GetLoginsToRemove() const{ return m_loginsToRemove; }
    inline bool LoginsToRemoveHasBeenSet() const { return m_loginsToRemoveHasBeenSet; }
    inline void SetLoginsToRemove(const Aws::Vector<Aws::String>& value) { m_loginsToRemoveHasBeenSet = true; m_loginsToRemove = value; }
    inline void SetLoginsToRemove(Aws::Vector<Aws::String>&& value) { m_loginsToRemoveHasBeenSet = true; m_loginsToRemove = std::move(value); }
    inline UnlinkIdentityRequest& WithLoginsToRemove(const Aws::Vector<Aws::String>& value) { SetLoginsToRemove(value); return *this;}
    inline UnlinkIdentityRequest& WithLoginsToRemove(Aws::Vector<Aws::String>&& value) { SetLoginsToRemove(std::move(value)); return *this;}
    inline UnlinkIdentityRequest& AddLoginsToRemove(const Aws::String& value) { m_loginsToRemoveHasBeenSet = true; m_loginsToRemove.push_back(value); return *this; }
    inline UnlinkIdentityRequest& AddLoginsToRemove(Aws::String&& value) { m_loginsToRemoveHasBeenSet = true; m_loginsToRemove.push_back(std::move(value)); return *this; }
    inline UnlinkIdentityRequest& AddLoginsToRemove(const char* value) { m_loginsToRemoveHasBeenSet = true; m_loginsToRemove.push_back(value); return *this; }
    ///@}
  private:

    Aws::String m_identityId;
    bool m_identityIdHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_logins;
    bool m_loginsHasBeenSet = false;

    Aws::Vector<Aws::String> m_loginsToRemove;
    bool m_loginsToRemoveHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
