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
  class GetSAMLProviderRequest : public IAMRequest
  {
  public:
    AWS_IAM_API GetSAMLProviderRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetSAMLProvider"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the SAML provider resource object in IAM to
     * get information about.</p> <p>For more information about ARNs, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">Amazon
     * Resource Names (ARNs)</a> in the <i>Amazon Web Services General
     * Reference</i>.</p>
     */
    inline const Aws::String& GetSAMLProviderArn() const{ return m_sAMLProviderArn; }
    inline bool SAMLProviderArnHasBeenSet() const { return m_sAMLProviderArnHasBeenSet; }
    inline void SetSAMLProviderArn(const Aws::String& value) { m_sAMLProviderArnHasBeenSet = true; m_sAMLProviderArn = value; }
    inline void SetSAMLProviderArn(Aws::String&& value) { m_sAMLProviderArnHasBeenSet = true; m_sAMLProviderArn = std::move(value); }
    inline void SetSAMLProviderArn(const char* value) { m_sAMLProviderArnHasBeenSet = true; m_sAMLProviderArn.assign(value); }
    inline GetSAMLProviderRequest& WithSAMLProviderArn(const Aws::String& value) { SetSAMLProviderArn(value); return *this;}
    inline GetSAMLProviderRequest& WithSAMLProviderArn(Aws::String&& value) { SetSAMLProviderArn(std::move(value)); return *this;}
    inline GetSAMLProviderRequest& WithSAMLProviderArn(const char* value) { SetSAMLProviderArn(value); return *this;}
    ///@}
  private:

    Aws::String m_sAMLProviderArn;
    bool m_sAMLProviderArnHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
