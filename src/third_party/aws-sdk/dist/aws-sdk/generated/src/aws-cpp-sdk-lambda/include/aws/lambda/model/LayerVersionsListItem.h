/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/Runtime.h>
#include <aws/lambda/model/Architecture.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  /**
   * <p>Details about a version of an <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
   * layer</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/LayerVersionsListItem">AWS
   * API Reference</a></p>
   */
  class LayerVersionsListItem
  {
  public:
    AWS_LAMBDA_API LayerVersionsListItem();
    AWS_LAMBDA_API LayerVersionsListItem(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API LayerVersionsListItem& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The ARN of the layer version.</p>
     */
    inline const Aws::String& GetLayerVersionArn() const{ return m_layerVersionArn; }
    inline bool LayerVersionArnHasBeenSet() const { return m_layerVersionArnHasBeenSet; }
    inline void SetLayerVersionArn(const Aws::String& value) { m_layerVersionArnHasBeenSet = true; m_layerVersionArn = value; }
    inline void SetLayerVersionArn(Aws::String&& value) { m_layerVersionArnHasBeenSet = true; m_layerVersionArn = std::move(value); }
    inline void SetLayerVersionArn(const char* value) { m_layerVersionArnHasBeenSet = true; m_layerVersionArn.assign(value); }
    inline LayerVersionsListItem& WithLayerVersionArn(const Aws::String& value) { SetLayerVersionArn(value); return *this;}
    inline LayerVersionsListItem& WithLayerVersionArn(Aws::String&& value) { SetLayerVersionArn(std::move(value)); return *this;}
    inline LayerVersionsListItem& WithLayerVersionArn(const char* value) { SetLayerVersionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The version number.</p>
     */
    inline long long GetVersion() const{ return m_version; }
    inline bool VersionHasBeenSet() const { return m_versionHasBeenSet; }
    inline void SetVersion(long long value) { m_versionHasBeenSet = true; m_version = value; }
    inline LayerVersionsListItem& WithVersion(long long value) { SetVersion(value); return *this;}
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
    inline LayerVersionsListItem& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline LayerVersionsListItem& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline LayerVersionsListItem& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date that the version was created, in ISO 8601 format. For example,
     * <code>2018-11-27T15:10:45.123+0000</code>.</p>
     */
    inline const Aws::String& GetCreatedDate() const{ return m_createdDate; }
    inline bool CreatedDateHasBeenSet() const { return m_createdDateHasBeenSet; }
    inline void SetCreatedDate(const Aws::String& value) { m_createdDateHasBeenSet = true; m_createdDate = value; }
    inline void SetCreatedDate(Aws::String&& value) { m_createdDateHasBeenSet = true; m_createdDate = std::move(value); }
    inline void SetCreatedDate(const char* value) { m_createdDateHasBeenSet = true; m_createdDate.assign(value); }
    inline LayerVersionsListItem& WithCreatedDate(const Aws::String& value) { SetCreatedDate(value); return *this;}
    inline LayerVersionsListItem& WithCreatedDate(Aws::String&& value) { SetCreatedDate(std::move(value)); return *this;}
    inline LayerVersionsListItem& WithCreatedDate(const char* value) { SetCreatedDate(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The layer's compatible runtimes.</p> <p>The following list includes
     * deprecated runtimes. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtime-deprecation-levels">Runtime
     * use after deprecation</a>.</p> <p>For a list of all currently supported
     * runtimes, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtimes-supported">Supported
     * runtimes</a>.</p>
     */
    inline const Aws::Vector<Runtime>& GetCompatibleRuntimes() const{ return m_compatibleRuntimes; }
    inline bool CompatibleRuntimesHasBeenSet() const { return m_compatibleRuntimesHasBeenSet; }
    inline void SetCompatibleRuntimes(const Aws::Vector<Runtime>& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes = value; }
    inline void SetCompatibleRuntimes(Aws::Vector<Runtime>&& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes = std::move(value); }
    inline LayerVersionsListItem& WithCompatibleRuntimes(const Aws::Vector<Runtime>& value) { SetCompatibleRuntimes(value); return *this;}
    inline LayerVersionsListItem& WithCompatibleRuntimes(Aws::Vector<Runtime>&& value) { SetCompatibleRuntimes(std::move(value)); return *this;}
    inline LayerVersionsListItem& AddCompatibleRuntimes(const Runtime& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes.push_back(value); return *this; }
    inline LayerVersionsListItem& AddCompatibleRuntimes(Runtime&& value) { m_compatibleRuntimesHasBeenSet = true; m_compatibleRuntimes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The layer's open-source license.</p>
     */
    inline const Aws::String& GetLicenseInfo() const{ return m_licenseInfo; }
    inline bool LicenseInfoHasBeenSet() const { return m_licenseInfoHasBeenSet; }
    inline void SetLicenseInfo(const Aws::String& value) { m_licenseInfoHasBeenSet = true; m_licenseInfo = value; }
    inline void SetLicenseInfo(Aws::String&& value) { m_licenseInfoHasBeenSet = true; m_licenseInfo = std::move(value); }
    inline void SetLicenseInfo(const char* value) { m_licenseInfoHasBeenSet = true; m_licenseInfo.assign(value); }
    inline LayerVersionsListItem& WithLicenseInfo(const Aws::String& value) { SetLicenseInfo(value); return *this;}
    inline LayerVersionsListItem& WithLicenseInfo(Aws::String&& value) { SetLicenseInfo(std::move(value)); return *this;}
    inline LayerVersionsListItem& WithLicenseInfo(const char* value) { SetLicenseInfo(value); return *this;}
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
    inline LayerVersionsListItem& WithCompatibleArchitectures(const Aws::Vector<Architecture>& value) { SetCompatibleArchitectures(value); return *this;}
    inline LayerVersionsListItem& WithCompatibleArchitectures(Aws::Vector<Architecture>&& value) { SetCompatibleArchitectures(std::move(value)); return *this;}
    inline LayerVersionsListItem& AddCompatibleArchitectures(const Architecture& value) { m_compatibleArchitecturesHasBeenSet = true; m_compatibleArchitectures.push_back(value); return *this; }
    inline LayerVersionsListItem& AddCompatibleArchitectures(Architecture&& value) { m_compatibleArchitecturesHasBeenSet = true; m_compatibleArchitectures.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_layerVersionArn;
    bool m_layerVersionArnHasBeenSet = false;

    long long m_version;
    bool m_versionHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    Aws::String m_createdDate;
    bool m_createdDateHasBeenSet = false;

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
