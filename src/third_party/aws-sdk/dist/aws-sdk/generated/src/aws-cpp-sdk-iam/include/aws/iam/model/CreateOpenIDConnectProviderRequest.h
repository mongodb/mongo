/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class CreateOpenIDConnectProviderRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreateOpenIDConnectProviderRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateOpenIDConnectProvider"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The URL of the identity provider. The URL must begin with
     * <code>https://</code> and should correspond to the <code>iss</code> claim in the
     * provider's OpenID Connect ID tokens. Per the OIDC standard, path components are
     * allowed but query parameters are not. Typically the URL consists of only a
     * hostname, like <code>https://server.example.org</code> or
     * <code>https://example.com</code>. The URL should not contain a port number. </p>
     * <p>You cannot register the same provider multiple times in a single Amazon Web
     * Services account. If you try to submit a URL that has already been used for an
     * OpenID Connect provider in the Amazon Web Services account, you will get an
     * error.</p>
     */
    inline const Aws::String& GetUrl() const{ return m_url; }
    inline bool UrlHasBeenSet() const { return m_urlHasBeenSet; }
    inline void SetUrl(const Aws::String& value) { m_urlHasBeenSet = true; m_url = value; }
    inline void SetUrl(Aws::String&& value) { m_urlHasBeenSet = true; m_url = std::move(value); }
    inline void SetUrl(const char* value) { m_urlHasBeenSet = true; m_url.assign(value); }
    inline CreateOpenIDConnectProviderRequest& WithUrl(const Aws::String& value) { SetUrl(value); return *this;}
    inline CreateOpenIDConnectProviderRequest& WithUrl(Aws::String&& value) { SetUrl(std::move(value)); return *this;}
    inline CreateOpenIDConnectProviderRequest& WithUrl(const char* value) { SetUrl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides a list of client IDs, also known as audiences. When a mobile or web
     * app registers with an OpenID Connect provider, they establish a value that
     * identifies the application. This is the value that's sent as the
     * <code>client_id</code> parameter on OAuth requests.</p> <p>You can register
     * multiple client IDs with the same provider. For example, you might have multiple
     * applications that use the same OIDC provider. You cannot register more than 100
     * client IDs with a single IAM OIDC provider.</p> <p>There is no defined format
     * for a client ID. The <code>CreateOpenIDConnectProviderRequest</code> operation
     * accepts client IDs up to 255 characters long.</p>
     */
    inline const Aws::Vector<Aws::String>& GetClientIDList() const{ return m_clientIDList; }
    inline bool ClientIDListHasBeenSet() const { return m_clientIDListHasBeenSet; }
    inline void SetClientIDList(const Aws::Vector<Aws::String>& value) { m_clientIDListHasBeenSet = true; m_clientIDList = value; }
    inline void SetClientIDList(Aws::Vector<Aws::String>&& value) { m_clientIDListHasBeenSet = true; m_clientIDList = std::move(value); }
    inline CreateOpenIDConnectProviderRequest& WithClientIDList(const Aws::Vector<Aws::String>& value) { SetClientIDList(value); return *this;}
    inline CreateOpenIDConnectProviderRequest& WithClientIDList(Aws::Vector<Aws::String>&& value) { SetClientIDList(std::move(value)); return *this;}
    inline CreateOpenIDConnectProviderRequest& AddClientIDList(const Aws::String& value) { m_clientIDListHasBeenSet = true; m_clientIDList.push_back(value); return *this; }
    inline CreateOpenIDConnectProviderRequest& AddClientIDList(Aws::String&& value) { m_clientIDListHasBeenSet = true; m_clientIDList.push_back(std::move(value)); return *this; }
    inline CreateOpenIDConnectProviderRequest& AddClientIDList(const char* value) { m_clientIDListHasBeenSet = true; m_clientIDList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of server certificate thumbprints for the OpenID Connect (OIDC)
     * identity provider's server certificates. Typically this list includes only one
     * entry. However, IAM lets you have up to five thumbprints for an OIDC provider.
     * This lets you maintain multiple thumbprints if the identity provider is rotating
     * certificates.</p> <p>This parameter is optional. If it is not included, IAM will
     * retrieve and use the top intermediate certificate authority (CA) thumbprint of
     * the OpenID Connect identity provider server certificate.</p> <p>The server
     * certificate thumbprint is the hex-encoded SHA-1 hash value of the X.509
     * certificate used by the domain where the OpenID Connect provider makes its keys
     * available. It is always a 40-character string.</p> <p>For example, assume that
     * the OIDC provider is <code>server.example.com</code> and the provider stores its
     * keys at https://keys.server.example.com/openid-connect. In that case, the
     * thumbprint string would be the hex-encoded SHA-1 hash value of the certificate
     * used by <code>https://keys.server.example.com.</code> </p> <p>For more
     * information about obtaining the OIDC provider thumbprint, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/identity-providers-oidc-obtain-thumbprint.html">Obtaining
     * the thumbprint for an OpenID Connect provider</a> in the <i>IAM user
     * Guide</i>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetThumbprintList() const{ return m_thumbprintList; }
    inline bool ThumbprintListHasBeenSet() const { return m_thumbprintListHasBeenSet; }
    inline void SetThumbprintList(const Aws::Vector<Aws::String>& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList = value; }
    inline void SetThumbprintList(Aws::Vector<Aws::String>&& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList = std::move(value); }
    inline CreateOpenIDConnectProviderRequest& WithThumbprintList(const Aws::Vector<Aws::String>& value) { SetThumbprintList(value); return *this;}
    inline CreateOpenIDConnectProviderRequest& WithThumbprintList(Aws::Vector<Aws::String>&& value) { SetThumbprintList(std::move(value)); return *this;}
    inline CreateOpenIDConnectProviderRequest& AddThumbprintList(const Aws::String& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList.push_back(value); return *this; }
    inline CreateOpenIDConnectProviderRequest& AddThumbprintList(Aws::String&& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList.push_back(std::move(value)); return *this; }
    inline CreateOpenIDConnectProviderRequest& AddThumbprintList(const char* value) { m_thumbprintListHasBeenSet = true; m_thumbprintList.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of tags that you want to attach to the new IAM OpenID Connect (OIDC)
     * provider. Each tag consists of a key name and an associated value. For more
     * information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>  <p>If any one of the tags
     * is invalid or if you exceed the allowed maximum number of tags, then the entire
     * request fails and the resource is not created.</p> 
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline CreateOpenIDConnectProviderRequest& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline CreateOpenIDConnectProviderRequest& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateOpenIDConnectProviderRequest& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline CreateOpenIDConnectProviderRequest& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_url;
    bool m_urlHasBeenSet = false;

    Aws::Vector<Aws::String> m_clientIDList;
    bool m_clientIDListHasBeenSet = false;

    Aws::Vector<Aws::String> m_thumbprintList;
    bool m_thumbprintListHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
