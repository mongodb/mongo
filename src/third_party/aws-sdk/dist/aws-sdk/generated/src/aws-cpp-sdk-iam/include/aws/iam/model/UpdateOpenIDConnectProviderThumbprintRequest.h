/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class UpdateOpenIDConnectProviderThumbprintRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateOpenIDConnectProviderThumbprintRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateOpenIDConnectProviderThumbprint"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the IAM OIDC provider resource object for
     * which you want to update the thumbprint. You can get a list of OIDC provider
     * ARNs by using the <a>ListOpenIDConnectProviders</a> operation.</p> <p>For more
     * information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetOpenIDConnectProviderArn() const{ return m_openIDConnectProviderArn; }
    inline bool OpenIDConnectProviderArnHasBeenSet() const { return m_openIDConnectProviderArnHasBeenSet; }
    inline void SetOpenIDConnectProviderArn(const Aws::String& value) { m_openIDConnectProviderArnHasBeenSet = true; m_openIDConnectProviderArn = value; }
    inline void SetOpenIDConnectProviderArn(Aws::String&& value) { m_openIDConnectProviderArnHasBeenSet = true; m_openIDConnectProviderArn = std::move(value); }
    inline void SetOpenIDConnectProviderArn(const char* value) { m_openIDConnectProviderArnHasBeenSet = true; m_openIDConnectProviderArn.assign(value); }
    inline UpdateOpenIDConnectProviderThumbprintRequest& WithOpenIDConnectProviderArn(const Aws::String& value) { SetOpenIDConnectProviderArn(value); return *this;}
    inline UpdateOpenIDConnectProviderThumbprintRequest& WithOpenIDConnectProviderArn(Aws::String&& value) { SetOpenIDConnectProviderArn(std::move(value)); return *this;}
    inline UpdateOpenIDConnectProviderThumbprintRequest& WithOpenIDConnectProviderArn(const char* value) { SetOpenIDConnectProviderArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of certificate thumbprints that are associated with the specified IAM
     * OpenID Connect provider. For more information, see
     * <a>CreateOpenIDConnectProvider</a>. </p>
     */
    inline const Aws::Vector<Aws::String>& GetThumbprintList() const{ return m_thumbprintList; }
    inline bool ThumbprintListHasBeenSet() const { return m_thumbprintListHasBeenSet; }
    inline void SetThumbprintList(const Aws::Vector<Aws::String>& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList = value; }
    inline void SetThumbprintList(Aws::Vector<Aws::String>&& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList = std::move(value); }
    inline UpdateOpenIDConnectProviderThumbprintRequest& WithThumbprintList(const Aws::Vector<Aws::String>& value) { SetThumbprintList(value); return *this;}
    inline UpdateOpenIDConnectProviderThumbprintRequest& WithThumbprintList(Aws::Vector<Aws::String>&& value) { SetThumbprintList(std::move(value)); return *this;}
    inline UpdateOpenIDConnectProviderThumbprintRequest& AddThumbprintList(const Aws::String& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList.push_back(value); return *this; }
    inline UpdateOpenIDConnectProviderThumbprintRequest& AddThumbprintList(Aws::String&& value) { m_thumbprintListHasBeenSet = true; m_thumbprintList.push_back(std::move(value)); return *this; }
    inline UpdateOpenIDConnectProviderThumbprintRequest& AddThumbprintList(const char* value) { m_thumbprintListHasBeenSet = true; m_thumbprintList.push_back(value); return *this; }
    ///@}
  private:

    Aws::String m_openIDConnectProviderArn;
    bool m_openIDConnectProviderArnHasBeenSet = false;

    Aws::Vector<Aws::String> m_thumbprintList;
    bool m_thumbprintListHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
