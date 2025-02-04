/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class AddClientIDToOpenIDConnectProviderRequest : public IAMRequest
  {
  public:
    AWS_IAM_API AddClientIDToOpenIDConnectProviderRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "AddClientIDToOpenIDConnectProvider"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the IAM OpenID Connect (OIDC) provider
     * resource to add the client ID to. You can get a list of OIDC provider ARNs by
     * using the <a>ListOpenIDConnectProviders</a> operation.</p>
     */
    inline const Aws::String& GetOpenIDConnectProviderArn() const{ return m_openIDConnectProviderArn; }
    inline bool OpenIDConnectProviderArnHasBeenSet() const { return m_openIDConnectProviderArnHasBeenSet; }
    inline void SetOpenIDConnectProviderArn(const Aws::String& value) { m_openIDConnectProviderArnHasBeenSet = true; m_openIDConnectProviderArn = value; }
    inline void SetOpenIDConnectProviderArn(Aws::String&& value) { m_openIDConnectProviderArnHasBeenSet = true; m_openIDConnectProviderArn = std::move(value); }
    inline void SetOpenIDConnectProviderArn(const char* value) { m_openIDConnectProviderArnHasBeenSet = true; m_openIDConnectProviderArn.assign(value); }
    inline AddClientIDToOpenIDConnectProviderRequest& WithOpenIDConnectProviderArn(const Aws::String& value) { SetOpenIDConnectProviderArn(value); return *this;}
    inline AddClientIDToOpenIDConnectProviderRequest& WithOpenIDConnectProviderArn(Aws::String&& value) { SetOpenIDConnectProviderArn(std::move(value)); return *this;}
    inline AddClientIDToOpenIDConnectProviderRequest& WithOpenIDConnectProviderArn(const char* value) { SetOpenIDConnectProviderArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The client ID (also known as audience) to add to the IAM OpenID Connect
     * provider resource.</p>
     */
    inline const Aws::String& GetClientID() const{ return m_clientID; }
    inline bool ClientIDHasBeenSet() const { return m_clientIDHasBeenSet; }
    inline void SetClientID(const Aws::String& value) { m_clientIDHasBeenSet = true; m_clientID = value; }
    inline void SetClientID(Aws::String&& value) { m_clientIDHasBeenSet = true; m_clientID = std::move(value); }
    inline void SetClientID(const char* value) { m_clientIDHasBeenSet = true; m_clientID.assign(value); }
    inline AddClientIDToOpenIDConnectProviderRequest& WithClientID(const Aws::String& value) { SetClientID(value); return *this;}
    inline AddClientIDToOpenIDConnectProviderRequest& WithClientID(Aws::String&& value) { SetClientID(std::move(value)); return *this;}
    inline AddClientIDToOpenIDConnectProviderRequest& WithClientID(const char* value) { SetClientID(value); return *this;}
    ///@}
  private:

    Aws::String m_openIDConnectProviderArn;
    bool m_openIDConnectProviderArnHasBeenSet = false;

    Aws::String m_clientID;
    bool m_clientIDHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
