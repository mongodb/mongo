/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/LayerVersionContentInput.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/Runtime.h>
#include <aws/lambda/model/Architecture.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class PublishLayerVersionRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API PublishLayerVersionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PublishLayerVersion"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>The name or Amazon Resource Name (ARN) of the layer.</p>
     */
    inline const Aws::String& GetLayerName() const{ return m_layerName; }
    inline bool LayerNameHasBeenSet() const { return m_layerNameHasBeenSet; }
    inline void SetLayerName(const Aws::String& value) { m_layerNameHasBeenSet = true; m_layerName = value; }
    inline void SetLayerName(Aws::String&& value) { m_layerNameHasBeenSet = true; m_layerName = std::move(value); }
    inline void SetLayerName(const char* value) { m_layerNameHasBeenSet = true; m_layerName.assign(value); }
    inline PublishLayerVersionRequest& WithLayerName(const Aws::String& value) { SetLayerName(value); return *this;}
    inline PublishLayerVersionRequest& WithLayerName(Aws::String&& value) { SetLayerName(std::move(value)); return *this;}
    inline PublishLayerVersionRequest& WithLayerName(const char* value) { SetLayerName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The description of the version.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline PublishLayerVersionRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline PublishLayerVersionRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline PublishLayerVersionRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function layer archive.</p>
     */
    inline const LayerVersionContentInput& GetContent() const{ return m_content; }
    inline bool ContentHasBeenSet() const { return m_contentHasBeenSet; }
    inline void SetContent(const LayerVersionContentInput& value) { m_contentHasBeenSet = true; m_content = value; }
    inline void SetContent(LayerVersionContentInput&& value) { m_contentHasBeenSet = true; m_content = std::move(value); }
    inline PublishLayerVersionRequest& WithContent(const LayerVersionContentInput& value) { SetContent(value); return *this;}
    inline PublishLayerVersionRequest& WithContent(LayerVersionContentInput&& value) { SetContent(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of compatible <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html">function
     * runtimes</a>. Used for filtering with <a>ListLayers</a> and
     * <a>ListLayerVersions</a>.</p> <p>The following list includes deprecated
     * runtimes. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtime-support-policy">Runtime
     * deprecation policy</a>.</p>
     */
    inline const Aws::Vector<Runtime>& GetCompatibleRuntimes() const{ return m_compatibleRuntimes; }
    inline bool CompatibleRuntimesHasBeenSet() const { return m_compatibleRuntimesHasBeenSet; }
    inline void SetCompatibleRuntimes(const Aws::Vector<Runtime>& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes = value; }
    inline void SetCompatibleRuntimes(Aws::Vector<Runtime>&& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes = std::move(value); }
    inline PublishLayerVersionRequest& WithCompatibleRuntimes(const Aws::Vector<Runtime>& value) { SetCompatibleRuntimes(value); return *this;}
    inline PublishLayerVersionRequest& WithCompatibleRuntimes(Aws::Vector<Runtime>&& value) { SetCompatibleRuntimes(std::move(value)); return *this;}
    inline PublishLayerVersionRequest& AddCompatibleRuntimes(const Runtime& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes.push_back(value); return *this; }
    inline PublishLayerVersionRequest& AddCompatibleRuntimes(Runtime&& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The layer's software license. It can be any of the following:</p> <ul> <li>
     * <p>An <a href="https://spdx.org/licenses/">SPDX license identifier</a>. For
     * example, <code>MIT</code>.</p> </li> <li> <p>The URL of a license hosted on the
     * internet. For example, <code>https://opensource.org/licenses/MIT</code>.</p>
     * </li> <li> <p>The full text of the license.</p> </li> </ul>
     */
    inline const Aws::String& GetLicenseInfo() const{ return m_licenseInfo; }
    inline bool LicenseInfoHasBeenSet() const { return m_licenseInfoHasBeenSet; }
    inline void SetLicenseInfo(const Aws::String& value) { m_licenseInfoHasBeenSet = true; m_licenseInfo = value; }
    inline void SetLicenseInfo(Aws::String&& value) { m_licenseInfoHasBeenSet = true; m_licenseInfo = std::move(value); }
    inline void SetLicenseInfo(const char* value) { m_licenseInfoHasBeenSet = true; m_licenseInfo.assign(value); }
    inline PublishLayerVersionRequest& WithLicenseInfo(const Aws::String& value) { SetLicenseInfo(value); return *this;}
    inline PublishLayerVersionRequest& WithLicenseInfo(Aws::String&& value) { SetLicenseInfo(std::move(value)); return *this;}
    inline PublishLayerVersionRequest& WithLicenseInfo(const char* value) { SetLicenseInfo(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of compatible <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/foundation-arch.html">instruction
     * set architectures</a>.</p>
     */
    inline const Aws::Vector<Architecture>& GetCompatibleArchitectures() const{ return m_compatibleArchitectures; }
    inline bool CompatibleArchitecturesHasBeenSet() const { return m_compatibleArchitecturesHasBeenSet; }
    inline void SetCompatibleArchitectures(const Aws::Vector<Architecture>& value) { m_compatibleArchitecturesHasBeenSet = true; m_compatibleArchitectures = value; }
    inline void SetCompatibleArchitectures(Aws::Vector<Architecture>&& value) { m_compatibleArchitecturesHasBeenSet = true; m_compatibleArchitectures = std::move(value); }
    inline PublishLayerVersionRequest& WithCompatibleArchitectures(const Aws::Vector<Architecture>& value) { SetCompatibleArchitectures(value); return *this;}
    inline PublishLayerVersionRequest& WithCompatibleArchitectures(Aws::Vector<Architecture>&& value) { SetCompatibleArchitectures(std::move(value)); return *this;}
    inline PublishLayerVersionRequest& AddCompatibleArchitectures(const Architecture& value) { m_compatibleArchitecturesHasBeenSet = true; m_compatibleArchitectures.push_back(value); return *this; }
    inline PublishLayerVersionRequest& AddCompatibleArchitectures(Architecture&& value) { m_compatibleArchitecturesHasBeenSet = true; m_compatibleArchitectures.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_layerName;
    bool m_layerNameHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    LayerVersionContentInput m_content;
    bool m_contentHasBeenSet = false;

    Aws::Vector<Runtime> m_compatibleRuntimes;
    bool m_compatibleRuntimesHasBeenSet = false;

    Aws::String m_licenseInfo;
    bool m_licenseInfoHasBeenSet = false;

    Aws::Vector<Architecture> m_compatibleArchitectures;
    bool m_compatibleArchitecturesHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
