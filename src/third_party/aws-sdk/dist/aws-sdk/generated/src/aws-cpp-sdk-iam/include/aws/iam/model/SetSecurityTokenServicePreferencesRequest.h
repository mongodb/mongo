/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/iam/model/GlobalEndpointTokenVersion.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class SetSecurityTokenServicePreferencesRequest : public IAMRequest
  {
  public:
    AWS_IAM_API SetSecurityTokenServicePreferencesRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "SetSecurityTokenServicePreferences"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The version of the global endpoint token. Version 1 tokens are valid only in
     * Amazon Web Services Regions that are available by default. These tokens do not
     * work in manually enabled Regions, such as Asia Pacific (Hong Kong). Version 2
     * tokens are valid in all Regions. However, version 2 tokens are longer and might
     * affect systems where you temporarily store tokens.</p> <p>For information, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_temp_enable-regions.html">Activating
     * and deactivating STS in an Amazon Web Services Region</a> in the <i>IAM User
     * Guide</i>.</p>
     */
    inline const GlobalEndpointTokenVersion& GetGlobalEndpointTokenVersion() const{ return m_globalEndpointTokenVersion; }
    inline bool GlobalEndpointTokenVersionHasBeenSet() const { return m_globalEndpointTokenVersionHasBeenSet; }
    inline void SetGlobalEndpointTokenVersion(const GlobalEndpointTokenVersion& value) { m_globalEndpointTokenVersionHasBeenSet = true; m_globalEndpointTokenVersion = value; }
    inline void SetGlobalEndpointTokenVersion(GlobalEndpointTokenVersion&& value) { m_globalEndpointTokenVersionHasBeenSet = true; m_globalEndpointTokenVersion = std::move(value); }
    inline SetSecurityTokenServicePreferencesRequest& WithGlobalEndpointTokenVersion(const GlobalEndpointTokenVersion& value) { SetGlobalEndpointTokenVersion(value); return *this;}
    inline SetSecurityTokenServicePreferencesRequest& WithGlobalEndpointTokenVersion(GlobalEndpointTokenVersion&& value) { SetGlobalEndpointTokenVersion(std::move(value)); return *this;}
    ///@}
  private:

    GlobalEndpointTokenVersion m_globalEndpointTokenVersion;
    bool m_globalEndpointTokenVersionHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
