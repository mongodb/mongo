#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/DateTime.h>
#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>
#include <functional>

struct aws_credentials;
struct aws_imds_client;
struct aws_imds_instance_info;
struct aws_imds_iam_profile;

namespace Aws
{

    namespace Crt
    {

        namespace Io
        {
            class ClientBootstrap;
        }

        namespace Auth
        {
            class Credentials;
        }

        namespace Imds
        {

            struct AWS_CRT_CPP_API ImdsClientConfig
            {
                ImdsClientConfig() : Bootstrap(nullptr) {}

                /**
                 * Connection bootstrap to use to create the http connection required to
                 * query resource from the Ec2 instance metadata service
                 *
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;

                /* Should add retry strategy support once that is available */
            };

            /**
             * https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instancedata-data-categories.html
             */
            struct AWS_CRT_CPP_API IamProfileView
            {
                DateTime lastUpdated;
                StringView instanceProfileArn;
                StringView instanceProfileId;
            };

            /**
             * A convenient class for you to persist data from IamProfileView, which has StringView members.
             */
            struct AWS_CRT_CPP_API IamProfile
            {
                IamProfile() {}
                IamProfile(const IamProfileView &other);

                IamProfile &operator=(const IamProfileView &other);

                DateTime lastUpdated;
                String instanceProfileArn;
                String instanceProfileId;
            };

            /**
             * Block of per-instance EC2-specific data
             *
             * https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instance-identity-documents.html
             */
            struct AWS_CRT_CPP_API InstanceInfoView
            {
                /* an array of StringView */
                Vector<StringView> marketplaceProductCodes;
                StringView availabilityZone;
                StringView privateIp;
                StringView version;
                StringView instanceId;
                /* an array of StringView */
                Vector<StringView> billingProducts;
                StringView instanceType;
                StringView accountId;
                StringView imageId;
                DateTime pendingTime;
                StringView architecture;
                StringView kernelId;
                StringView ramdiskId;
                StringView region;
            };

            /**
             * A convenient class for you to persist data from InstanceInfoView, which has StringView members.
             */
            struct AWS_CRT_CPP_API InstanceInfo
            {
                InstanceInfo() {}
                InstanceInfo(const InstanceInfoView &other);

                InstanceInfo &operator=(const InstanceInfoView &other);

                /* an array of StringView */
                Vector<String> marketplaceProductCodes;
                String availabilityZone;
                String privateIp;
                String version;
                String instanceId;
                /* an array of StringView */
                Vector<String> billingProducts;
                String instanceType;
                String accountId;
                String imageId;
                DateTime pendingTime;
                String architecture;
                String kernelId;
                String ramdiskId;
                String region;
            };

            using OnResourceAcquired = std::function<void(const StringView &resource, int errorCode, void *userData)>;
            using OnVectorResourceAcquired =
                std::function<void(const Vector<StringView> &resource, int errorCode, void *userData)>;
            using OnCredentialsAcquired =
                std::function<void(const Auth::Credentials &credentials, int errorCode, void *userData)>;
            using OnIamProfileAcquired =
                std::function<void(const IamProfileView &iamProfile, int errorCode, void *userData)>;
            using OnInstanceInfoAcquired =
                std::function<void(const InstanceInfoView &instanceInfo, int errorCode, void *userData)>;

            class AWS_CRT_CPP_API ImdsClient
            {
              public:
                ImdsClient(const ImdsClientConfig &config, Allocator *allocator = ApiAllocator()) noexcept;

                ~ImdsClient();

                ImdsClient(const ImdsClient &) = delete;
                ImdsClient(ImdsClient &&) = delete;
                ImdsClient &operator=(const ImdsClient &) = delete;
                ImdsClient &operator=(ImdsClient &&) = delete;

                aws_imds_client *GetUnderlyingHandle() { return m_client; }

                /**
                 * Queries a generic resource (string) from the ec2 instance metadata document
                 *
                 * @param resourcePath path of the resource to query
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetResource(const StringView &resourcePath, OnResourceAcquired callback, void *userData);

                /**
                 * Gets the ami id of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetAmiId(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the ami launch index of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetAmiLaunchIndex(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the ami manifest path of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetAmiManifestPath(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the list of ancestor ami ids of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetAncestorAmiIds(OnVectorResourceAcquired callback, void *userData);

                /**
                 * Gets the instance-action of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetInstanceAction(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the instance id of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetInstanceId(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the instance type of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetInstanceType(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the mac address of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetMacAddress(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the private ip address of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetPrivateIpAddress(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the availability zone of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetAvailabilityZone(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the product codes of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetProductCodes(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the public key of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetPublicKey(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the ramdisk id of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetRamDiskId(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the reservation id of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetReservationId(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the list of the security groups of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetSecurityGroups(OnVectorResourceAcquired callback, void *userData);

                /**
                 * Gets the list of block device mappings of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetBlockDeviceMapping(OnVectorResourceAcquired callback, void *userData);

                /**
                 * Gets the attached iam role of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetAttachedIamRole(OnResourceAcquired callback, void *userData);

                /**
                 * Gets temporary credentials based on the attached iam role of the ec2 instance
                 *
                 * @param iamRoleName iam role name to get temporary credentials through
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetCredentials(const StringView &iamRoleName, OnCredentialsAcquired callback, void *userData);

                /**
                 * Gets the iam profile information of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetIamProfile(OnIamProfileAcquired callback, void *userData);

                /**
                 * Gets the user data of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetUserData(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the signature of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetInstanceSignature(OnResourceAcquired callback, void *userData);

                /**
                 * Gets the instance information data block of the ec2 instance from the instance metadata document
                 *
                 * @param callback callback function to invoke on query success or failure
                 * @param userData opaque data to invoke the completion callback with
                 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
                 */
                int GetInstanceInfo(OnInstanceInfoAcquired callback, void *userData);

              private:
                static void s_onResourceAcquired(const aws_byte_buf *resource, int erroCode, void *userData);

                static void s_onVectorResourceAcquired(const aws_array_list *array, int errorCode, void *userData);

                static void s_onCredentialsAcquired(const aws_credentials *credentials, int errorCode, void *userData);

                static void s_onIamProfileAcquired(
                    const aws_imds_iam_profile *iamProfileInfo,
                    int errorCode,
                    void *userData);

                static void s_onInstanceInfoAcquired(
                    const aws_imds_instance_info *instanceInfo,
                    int error_code,
                    void *userData);

                aws_imds_client *m_client;
                Allocator *m_allocator;
            };

        } // namespace Imds
    } // namespace Crt
} // namespace Aws
