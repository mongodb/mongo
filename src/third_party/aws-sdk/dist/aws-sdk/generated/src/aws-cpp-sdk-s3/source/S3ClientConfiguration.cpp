/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/S3ClientConfiguration.h>

namespace Aws
{
namespace S3
{

static const char US_EAST_1_REGIONAL_ENDPOINT_ENV_VAR[] = "AWS_S3_US_EAST_1_REGIONAL_ENDPOINT";
static const char US_EAST_1_REGIONAL_ENDPOINT_CONFIG_VAR[] = "s3_us_east_1_regional_endpoint";
static const char S3_DISABLE_MULTIREGION_ACCESS_POINTS_ENV_VAR[] = "AWS_S3_DISABLE_MULTIREGION_ACCESS_POINTS";
static const char S3_DISABLE_MULTIREGION_ACCESS_POINTS_CONFIG_VAR[] = "s3_disable_multiregion_access_points";
static const char S3_DISABLE_EXPRESS_SESSION_ENVIRONMENT_VARIABLE[] = "AWS_S3_DISABLE_S3_EXPRESS_AUTH";
static const char S3_DISABLE_EXPRESS_SESSION_CONFIG_FILE_OPTION[] = "s3_disable_s3_express_auth";
static const char S3_USE_ARN_REGION_ENVIRONMENT_VARIABLE[] = "AWS_S3_USE_ARN_REGION";
static const char S3_USE_ARN_REGION_CONFIG_FILE_OPTION[] = "s3_use_arn_region";

void S3ClientConfiguration::LoadS3SpecificConfig(const Aws::String& inputProfileName)
{
  if (Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET == this->useUSEast1RegionalEndPointOption)
  {
    const Aws::String& useUSEastOption =
        BaseClientConfigClass::LoadConfigFromEnvOrProfile(US_EAST_1_REGIONAL_ENDPOINT_ENV_VAR,
                                                          inputProfileName,
                                                          US_EAST_1_REGIONAL_ENDPOINT_CONFIG_VAR,
                                                          {"legacy", "regional"},
                                                          "regional");
    if (useUSEastOption == "legacy") {
      this->useUSEast1RegionalEndPointOption = Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::LEGACY;
    } else {
      this->useUSEast1RegionalEndPointOption = Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::REGIONAL;
    }
  }

  Aws::String s3DisableMultiRegionAccessPoints = ClientConfiguration::LoadConfigFromEnvOrProfile(S3_DISABLE_MULTIREGION_ACCESS_POINTS_ENV_VAR,
                                                                                        inputProfileName,
                                                                                        S3_DISABLE_MULTIREGION_ACCESS_POINTS_CONFIG_VAR,
                                                                                        {"true", "false"},
                                                                                        "false");
  if (s3DisableMultiRegionAccessPoints == "true")
  {
    disableMultiRegionAccessPoints = true;
  }

  Aws::String disableS3ExpressAuthCfg = ClientConfiguration::LoadConfigFromEnvOrProfile(S3_DISABLE_EXPRESS_SESSION_ENVIRONMENT_VARIABLE,
                                                                                        inputProfileName,
                                                                                        S3_DISABLE_EXPRESS_SESSION_CONFIG_FILE_OPTION,
                                                                                        {"true", "false"},
                                                                                        "false");
  if (disableS3ExpressAuthCfg == "true")
  {
    disableS3ExpressAuth = true;
  }

  Aws::String useArnRegionCfg = ClientConfiguration::LoadConfigFromEnvOrProfile(S3_USE_ARN_REGION_ENVIRONMENT_VARIABLE,
                                                                               inputProfileName,
                                                                               S3_USE_ARN_REGION_CONFIG_FILE_OPTION,
                                                                               {"true", "false"},
                                                                               "false");
  if (useArnRegionCfg == "true")
  {
    useArnRegion = true;
  }
}

S3ClientConfiguration::S3ClientConfiguration(const Client::ClientConfigurationInitValues &configuration)
: BaseClientConfigClass(configuration)
{
  LoadS3SpecificConfig(this->profileName);
}

S3ClientConfiguration::S3ClientConfiguration(const char* inputProfileName, bool shouldDisableIMDS)
: BaseClientConfigClass(inputProfileName, shouldDisableIMDS)
{
  LoadS3SpecificConfig(Aws::String(inputProfileName));
}

S3ClientConfiguration::S3ClientConfiguration(bool useSmartDefaults, const char* defaultMode, bool shouldDisableIMDS)
: BaseClientConfigClass(useSmartDefaults, defaultMode, shouldDisableIMDS)
{
  LoadS3SpecificConfig(this->profileName);
}

S3ClientConfiguration::S3ClientConfiguration(const Client::ClientConfiguration& config,
                         Client::AWSAuthV4Signer::PayloadSigningPolicy iPayloadSigningPolicy,
                         bool iUseVirtualAddressing,
                         US_EAST_1_REGIONAL_ENDPOINT_OPTION iUseUSEast1RegionalEndPointOption)
  : BaseClientConfigClass(config),
    useVirtualAddressing(iUseVirtualAddressing),
    useUSEast1RegionalEndPointOption(iUseUSEast1RegionalEndPointOption),
    payloadSigningPolicy(iPayloadSigningPolicy)
{
  LoadS3SpecificConfig(this->profileName);
}


} // namespace S3
} // namespace Aws
