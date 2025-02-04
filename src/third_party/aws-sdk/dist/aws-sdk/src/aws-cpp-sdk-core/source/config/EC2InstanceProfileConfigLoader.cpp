/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/memory/stl/AWSList.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <fstream>
#include <random>

namespace Aws
{
    namespace Config
    {
        using namespace Aws::Utils;
        using namespace Aws::Auth;

        static const char* const INTERNAL_EXCEPTION_PHRASE = "InternalServiceException";
        static const int64_t FIVE_MINUTE_MILLIS = 60000 * 5;
        static const int64_t TEN_MINUTE_MILLIS = 60000 * 10;

        static const char* const EC2_INSTANCE_PROFILE_LOG_TAG = "Aws::Config::EC2InstanceProfileConfigLoader";

        EC2InstanceProfileConfigLoader::EC2InstanceProfileConfigLoader(const std::shared_ptr<Aws::Internal::EC2MetadataClient>& client)
        {
            if(client == nullptr)
            {
                Aws::Internal::InitEC2MetadataClient();
                m_ec2metadataClient = Aws::Internal::GetEC2MetadataClient();
            }
            else
            {
                m_ec2metadataClient = client;
            }
        }

        bool EC2InstanceProfileConfigLoader::LoadInternal()
        {
            // re-use old credentials until we need to call IMDS again.
            if (DateTime::Now().Millis() < this->credentialsValidUntilMillis) {
                AWS_LOGSTREAM_ERROR(EC2_INSTANCE_PROFILE_LOG_TAG,
                                    "Skipping IMDS call until " << this->credentialsValidUntilMillis);
                return true;
            }
            this->credentialsValidUntilMillis = DateTime::Now().Millis();

            if (!m_ec2metadataClient) {
                AWS_LOGSTREAM_FATAL(EC2_INSTANCE_PROFILE_LOG_TAG, "EC2MetadataClient is a nullptr!")
                return false;
            }
            auto credentialsStr = m_ec2metadataClient->GetDefaultCredentialsSecurely();
            if(credentialsStr.empty()) return false;

            Json::JsonValue credentialsDoc(credentialsStr);
            if (!credentialsDoc.WasParseSuccessful())
            {
                AWS_LOGSTREAM_ERROR(EC2_INSTANCE_PROFILE_LOG_TAG,
                        "Failed to parse output from EC2MetadataService.");
                return false;
            }

            const char* accessKeyId = "AccessKeyId";
            const char* secretAccessKey = "SecretAccessKey";
            const char* expiration = "Expiration";
            const char* code = "Code";
            Aws::String accessKey, secretKey, token;

            auto credentialsView = credentialsDoc.View();
            DateTime expirationTime(credentialsView.GetString(expiration), Aws::Utils::DateFormat::ISO_8601);
            // re-use old credentials and not block if the IMDS call failed or if the latest credential is in the past
            if (expirationTime.WasParseSuccessful() && DateTime::Now() > expirationTime) {
                AWS_LOGSTREAM_ERROR(EC2_INSTANCE_PROFILE_LOG_TAG,
                                    "Expiration Time of Credentials in the past, refusing to update credentials");
                this->credentialsValidUntilMillis = DateTime::Now().Millis() + calculateRetryTime();
                return true;
            } else if (credentialsView.GetString(code) == INTERNAL_EXCEPTION_PHRASE) {
                AWS_LOGSTREAM_ERROR(EC2_INSTANCE_PROFILE_LOG_TAG,
                                    "IMDS call failed, refusing to update credentials");
                this->credentialsValidUntilMillis = DateTime::Now().Millis() + calculateRetryTime();
                return true;
            }
            accessKey = credentialsView.GetString(accessKeyId);
            AWS_LOGSTREAM_INFO(EC2_INSTANCE_PROFILE_LOG_TAG,
                    "Successfully pulled credentials from metadata service with access key " << accessKey);

            secretKey = credentialsView.GetString(secretAccessKey);
            token = credentialsView.GetString("Token");

            auto region = m_ec2metadataClient->GetCurrentRegion();

            Profile profile;
            profile.SetCredentials(AWSCredentials(accessKey, secretKey, token));
            profile.SetRegion(region);
            profile.SetName(INSTANCE_PROFILE_KEY);

            m_profiles[INSTANCE_PROFILE_KEY] = profile;

            return true;
        }

        int64_t EC2InstanceProfileConfigLoader::calculateRetryTime() const {
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::uniform_int_distribution<int64_t> dist(FIVE_MINUTE_MILLIS, TEN_MINUTE_MILLIS);
            return dist(gen);
        }
    } // Config namespace
} // Aws namespace
