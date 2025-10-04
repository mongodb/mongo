/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/aws_imds_client.h>
#include <aws/auth/credentials.h>
#include <aws/crt/Api.h>
#include <aws/crt/ImdsClient.h>
#include <aws/crt/auth/Credentials.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/io/Bootstrap.h>

namespace Aws
{
    namespace Crt
    {
        namespace Imds
        {
            IamProfile::IamProfile(const IamProfileView &other)
                : lastUpdated(other.lastUpdated),
                  instanceProfileArn(other.instanceProfileArn.data(), other.instanceProfileArn.size()),
                  instanceProfileId(other.instanceProfileId.data(), other.instanceProfileId.size())
            {
            }

            IamProfile &IamProfile::operator=(const IamProfileView &other)
            {
                lastUpdated = other.lastUpdated;
                instanceProfileArn = String(other.instanceProfileArn.data(), other.instanceProfileArn.size());
                instanceProfileId = String(other.instanceProfileId.data(), other.instanceProfileId.size());
                return *this;
            }

            InstanceInfo::InstanceInfo(const InstanceInfoView &other)
                : availabilityZone(other.availabilityZone.data(), other.availabilityZone.size()),
                  privateIp(other.privateIp.data(), other.privateIp.size()),
                  version(other.version.data(), other.version.size()),
                  instanceId(other.instanceId.data(), other.instanceId.size()),
                  instanceType(other.instanceType.data(), other.instanceType.size()),
                  accountId(other.accountId.data(), other.accountId.size()),
                  imageId(other.imageId.data(), other.imageId.size()), pendingTime(other.pendingTime),
                  architecture(other.architecture.data(), other.architecture.size()),
                  kernelId(other.kernelId.data(), other.kernelId.size()),
                  ramdiskId(other.ramdiskId.data(), other.ramdiskId.size()),
                  region(other.region.data(), other.region.size())
            {
                for (const auto &m : other.marketplaceProductCodes)
                {
                    marketplaceProductCodes.emplace_back(m.data(), m.size());
                }

                for (const auto &m : other.billingProducts)
                {
                    billingProducts.emplace_back(m.data(), m.size());
                }
            }

            InstanceInfo &InstanceInfo::operator=(const InstanceInfoView &other)
            {
                availabilityZone = {other.availabilityZone.data(), other.availabilityZone.size()};
                privateIp = {other.privateIp.data(), other.privateIp.size()};
                version = {other.version.data(), other.version.size()};
                instanceId = {other.instanceId.data(), other.instanceId.size()};
                instanceType = {other.instanceType.data(), other.instanceType.size()};
                accountId = {other.accountId.data(), other.accountId.size()};
                imageId = {other.imageId.data(), other.imageId.size()};
                pendingTime = other.pendingTime;
                architecture = {other.architecture.data(), other.architecture.size()};
                kernelId = {other.kernelId.data(), other.kernelId.size()};
                ramdiskId = {other.ramdiskId.data(), other.ramdiskId.size()};
                region = {other.region.data(), other.region.size()};

                for (const auto &m : other.marketplaceProductCodes)
                {
                    marketplaceProductCodes.emplace_back(m.data(), m.size());
                }

                for (const auto &m : other.billingProducts)
                {
                    billingProducts.emplace_back(m.data(), m.size());
                }
                return *this;
            }

            ImdsClient::ImdsClient(const ImdsClientConfig &config, Allocator *allocator) noexcept
            {
                struct aws_imds_client_options raw_config;
                AWS_ZERO_STRUCT(raw_config);
                if (config.Bootstrap != nullptr)
                {
                    raw_config.bootstrap = config.Bootstrap->GetUnderlyingHandle();
                }
                else
                {
                    raw_config.bootstrap = ApiHandle::GetOrCreateStaticDefaultClientBootstrap()->GetUnderlyingHandle();
                }

                m_client = aws_imds_client_new(allocator, &raw_config);
                m_allocator = allocator;
            }

            ImdsClient::~ImdsClient()
            {
                if (m_client)
                {
                    aws_imds_client_release(m_client);
                    m_client = nullptr;
                }
            }

            template <typename T> struct WrappedCallbackArgs
            {
                WrappedCallbackArgs(Allocator *allocator, T callback, void *userData)
                    : allocator(allocator), callback(callback), userData(userData)
                {
                }
                Allocator *allocator;
                T callback;
                void *userData;
            };

            void ImdsClient::s_onResourceAcquired(const aws_byte_buf *resource, int errorCode, void *userData)
            {
                WrappedCallbackArgs<OnResourceAcquired> *callbackArgs =
                    static_cast<WrappedCallbackArgs<OnResourceAcquired> *>(userData);
                callbackArgs->callback(
                    ByteCursorToStringView(aws_byte_cursor_from_buf(resource)), errorCode, callbackArgs->userData);
                Aws::Crt::Delete(callbackArgs, callbackArgs->allocator);
            }

            void ImdsClient::s_onVectorResourceAcquired(const aws_array_list *array, int errorCode, void *userData)
            {
                WrappedCallbackArgs<OnVectorResourceAcquired> *callbackArgs =
                    static_cast<WrappedCallbackArgs<OnVectorResourceAcquired> *>(userData);
                callbackArgs->callback(
                    ArrayListToVector<ByteCursor, StringView>(array, ByteCursorToStringView),
                    errorCode,
                    callbackArgs->userData);
                Aws::Crt::Delete(callbackArgs, callbackArgs->allocator);
            }

            void ImdsClient::s_onCredentialsAcquired(const aws_credentials *credentials, int errorCode, void *userData)
            {
                WrappedCallbackArgs<OnCredentialsAcquired> *callbackArgs =
                    static_cast<WrappedCallbackArgs<OnCredentialsAcquired> *>(userData);
                auto credentialsPtr = Aws::Crt::MakeShared<Auth::Credentials>(callbackArgs->allocator, credentials);
                callbackArgs->callback(credentials, errorCode, callbackArgs->userData);
                Aws::Crt::Delete(callbackArgs, callbackArgs->allocator);
            }

            void ImdsClient::s_onIamProfileAcquired(
                const aws_imds_iam_profile *iamProfileInfo,
                int errorCode,
                void *userData)
            {
                WrappedCallbackArgs<OnIamProfileAcquired> *callbackArgs =
                    static_cast<WrappedCallbackArgs<OnIamProfileAcquired> *>(userData);
                IamProfileView iamProfile;
                iamProfile.lastUpdated = aws_date_time_as_epoch_secs(&(iamProfileInfo->last_updated));
                iamProfile.instanceProfileArn = ByteCursorToStringView(iamProfileInfo->instance_profile_arn);
                iamProfile.instanceProfileId = ByteCursorToStringView(iamProfileInfo->instance_profile_id);
                callbackArgs->callback(iamProfile, errorCode, callbackArgs->userData);
                Aws::Crt::Delete(callbackArgs, callbackArgs->allocator);
            }

            void ImdsClient::s_onInstanceInfoAcquired(
                const aws_imds_instance_info *instanceInfo,
                int errorCode,
                void *userData)
            {
                WrappedCallbackArgs<OnInstanceInfoAcquired> *callbackArgs =
                    static_cast<WrappedCallbackArgs<OnInstanceInfoAcquired> *>(userData);
                InstanceInfoView info;
                info.marketplaceProductCodes = ArrayListToVector<ByteCursor, StringView>(
                    &(instanceInfo->marketplace_product_codes), ByteCursorToStringView);
                info.availabilityZone = ByteCursorToStringView(instanceInfo->availability_zone);
                info.privateIp = ByteCursorToStringView(instanceInfo->private_ip);
                info.version = ByteCursorToStringView(instanceInfo->version);
                info.instanceId = ByteCursorToStringView(instanceInfo->instance_id);
                info.billingProducts = ArrayListToVector<ByteCursor, StringView>(
                    &(instanceInfo->billing_products), ByteCursorToStringView);
                info.instanceType = ByteCursorToStringView(instanceInfo->instance_type);
                info.accountId = ByteCursorToStringView(instanceInfo->account_id);
                info.imageId = ByteCursorToStringView(instanceInfo->image_id);
                info.pendingTime = aws_date_time_as_epoch_secs(&(instanceInfo->pending_time));
                info.architecture = ByteCursorToStringView(instanceInfo->architecture);
                info.kernelId = ByteCursorToStringView(instanceInfo->kernel_id);
                info.ramdiskId = ByteCursorToStringView(instanceInfo->ramdisk_id);
                info.region = ByteCursorToStringView(instanceInfo->region);
                callbackArgs->callback(info, errorCode, callbackArgs->userData);
                Aws::Crt::Delete(callbackArgs, callbackArgs->allocator);
            }

            int ImdsClient::GetResource(const StringView &resourcePath, OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }

                return aws_imds_client_get_resource_async(
                    m_client, StringViewToByteCursor(resourcePath), s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetAmiId(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_ami_id(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetAmiLaunchIndex(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_ami_launch_index(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetAmiManifestPath(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_ami_manifest_path(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetAncestorAmiIds(OnVectorResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnVectorResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_ancestor_ami_ids(m_client, s_onVectorResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetInstanceAction(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_instance_action(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetInstanceId(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_instance_id(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetInstanceType(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_instance_type(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetMacAddress(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_mac_address(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetPrivateIpAddress(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_private_ip_address(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetAvailabilityZone(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_availability_zone(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetProductCodes(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_product_codes(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetPublicKey(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_public_key(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetRamDiskId(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_ramdisk_id(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetReservationId(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_reservation_id(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetSecurityGroups(OnVectorResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnVectorResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_security_groups(m_client, s_onVectorResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetBlockDeviceMapping(OnVectorResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnVectorResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_block_device_mapping(
                    m_client, s_onVectorResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetAttachedIamRole(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_attached_iam_role(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetCredentials(
                const StringView &iamRoleName,
                OnCredentialsAcquired callback,
                void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnCredentialsAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_credentials(
                    m_client, StringViewToByteCursor(iamRoleName), s_onCredentialsAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetIamProfile(OnIamProfileAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnIamProfileAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_iam_profile(m_client, s_onIamProfileAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetUserData(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_user_data(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetInstanceSignature(OnResourceAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnResourceAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_instance_signature(m_client, s_onResourceAcquired, wrappedCallbackArgs);
            }

            int ImdsClient::GetInstanceInfo(OnInstanceInfoAcquired callback, void *userData)
            {
                auto wrappedCallbackArgs = Aws::Crt::New<WrappedCallbackArgs<OnInstanceInfoAcquired>>(
                    m_allocator, m_allocator, callback, userData);
                if (wrappedCallbackArgs == nullptr)
                {
                    return AWS_OP_ERR;
                }
                return aws_imds_client_get_instance_info(m_client, s_onInstanceInfoAcquired, wrappedCallbackArgs);
            }
        } // namespace Imds
    } // namespace Crt

} // namespace Aws
