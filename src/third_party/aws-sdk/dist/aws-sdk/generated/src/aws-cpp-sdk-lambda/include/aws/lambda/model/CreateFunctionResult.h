/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/Runtime.h>
#include <aws/lambda/model/VpcConfigResponse.h>
#include <aws/lambda/model/DeadLetterConfig.h>
#include <aws/lambda/model/EnvironmentResponse.h>
#include <aws/lambda/model/TracingConfigResponse.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/State.h>
#include <aws/lambda/model/StateReasonCode.h>
#include <aws/lambda/model/LastUpdateStatus.h>
#include <aws/lambda/model/LastUpdateStatusReasonCode.h>
#include <aws/lambda/model/PackageType.h>
#include <aws/lambda/model/ImageConfigResponse.h>
#include <aws/lambda/model/EphemeralStorage.h>
#include <aws/lambda/model/SnapStartResponse.h>
#include <aws/lambda/model/RuntimeVersionConfig.h>
#include <aws/lambda/model/LoggingConfig.h>
#include <aws/lambda/model/Layer.h>
#include <aws/lambda/model/FileSystemConfig.h>
#include <aws/lambda/model/Architecture.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{
  /**
   * <p>Details about a function's configuration.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/FunctionConfiguration">AWS
   * API Reference</a></p>
   */
  class CreateFunctionResult
  {
  public:
    AWS_LAMBDA_API CreateFunctionResult();
    AWS_LAMBDA_API CreateFunctionResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API CreateFunctionResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The name of the function.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline void SetFunctionName(const Aws::String& value) { m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionName.assign(value); }
    inline CreateFunctionResult& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline CreateFunctionResult& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline CreateFunctionResult& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's Amazon Resource Name (ARN).</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArn.assign(value); }
    inline CreateFunctionResult& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline CreateFunctionResult& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline CreateFunctionResult& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The identifier of the function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html">
     * runtime</a>. Runtime is required if the deployment package is a .zip file
     * archive. Specifying a runtime results in an error if you're deploying a function
     * using a container image.</p> <p>The following list includes deprecated runtimes.
     * Lambda blocks creating new functions and updating existing functions shortly
     * after each runtime is deprecated. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtime-deprecation-levels">Runtime
     * use after deprecation</a>.</p> <p>For a list of all currently supported
     * runtimes, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html#runtimes-supported">Supported
     * runtimes</a>.</p>
     */
    inline const Runtime& GetRuntime() const{ return m_runtime; }
    inline void SetRuntime(const Runtime& value) { m_runtime = value; }
    inline void SetRuntime(Runtime&& value) { m_runtime = std::move(value); }
    inline CreateFunctionResult& WithRuntime(const Runtime& value) { SetRuntime(value); return *this;}
    inline CreateFunctionResult& WithRuntime(Runtime&& value) { SetRuntime(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's execution role.</p>
     */
    inline const Aws::String& GetRole() const{ return m_role; }
    inline void SetRole(const Aws::String& value) { m_role = value; }
    inline void SetRole(Aws::String&& value) { m_role = std::move(value); }
    inline void SetRole(const char* value) { m_role.assign(value); }
    inline CreateFunctionResult& WithRole(const Aws::String& value) { SetRole(value); return *this;}
    inline CreateFunctionResult& WithRole(Aws::String&& value) { SetRole(std::move(value)); return *this;}
    inline CreateFunctionResult& WithRole(const char* value) { SetRole(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function that Lambda calls to begin running your function.</p>
     */
    inline const Aws::String& GetHandler() const{ return m_handler; }
    inline void SetHandler(const Aws::String& value) { m_handler = value; }
    inline void SetHandler(Aws::String&& value) { m_handler = std::move(value); }
    inline void SetHandler(const char* value) { m_handler.assign(value); }
    inline CreateFunctionResult& WithHandler(const Aws::String& value) { SetHandler(value); return *this;}
    inline CreateFunctionResult& WithHandler(Aws::String&& value) { SetHandler(std::move(value)); return *this;}
    inline CreateFunctionResult& WithHandler(const char* value) { SetHandler(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The size of the function's deployment package, in bytes.</p>
     */
    inline long long GetCodeSize() const{ return m_codeSize; }
    inline void SetCodeSize(long long value) { m_codeSize = value; }
    inline CreateFunctionResult& WithCodeSize(long long value) { SetCodeSize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's description.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline void SetDescription(const Aws::String& value) { m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_description.assign(value); }
    inline CreateFunctionResult& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CreateFunctionResult& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CreateFunctionResult& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The amount of time in seconds that Lambda allows a function to run before
     * stopping it.</p>
     */
    inline int GetTimeout() const{ return m_timeout; }
    inline void SetTimeout(int value) { m_timeout = value; }
    inline CreateFunctionResult& WithTimeout(int value) { SetTimeout(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The amount of memory available to the function at runtime.</p>
     */
    inline int GetMemorySize() const{ return m_memorySize; }
    inline void SetMemorySize(int value) { m_memorySize = value; }
    inline CreateFunctionResult& WithMemorySize(int value) { SetMemorySize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time that the function was last updated, in <a
     * href="https://www.w3.org/TR/NOTE-datetime">ISO-8601 format</a>
     * (YYYY-MM-DDThh:mm:ss.sTZD).</p>
     */
    inline const Aws::String& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::String& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::String&& value) { m_lastModified = std::move(value); }
    inline void SetLastModified(const char* value) { m_lastModified.assign(value); }
    inline CreateFunctionResult& WithLastModified(const Aws::String& value) { SetLastModified(value); return *this;}
    inline CreateFunctionResult& WithLastModified(Aws::String&& value) { SetLastModified(std::move(value)); return *this;}
    inline CreateFunctionResult& WithLastModified(const char* value) { SetLastModified(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The SHA256 hash of the function's deployment package.</p>
     */
    inline const Aws::String& GetCodeSha256() const{ return m_codeSha256; }
    inline void SetCodeSha256(const Aws::String& value) { m_codeSha256 = value; }
    inline void SetCodeSha256(Aws::String&& value) { m_codeSha256 = std::move(value); }
    inline void SetCodeSha256(const char* value) { m_codeSha256.assign(value); }
    inline CreateFunctionResult& WithCodeSha256(const Aws::String& value) { SetCodeSha256(value); return *this;}
    inline CreateFunctionResult& WithCodeSha256(Aws::String&& value) { SetCodeSha256(std::move(value)); return *this;}
    inline CreateFunctionResult& WithCodeSha256(const char* value) { SetCodeSha256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The version of the Lambda function.</p>
     */
    inline const Aws::String& GetVersion() const{ return m_version; }
    inline void SetVersion(const Aws::String& value) { m_version = value; }
    inline void SetVersion(Aws::String&& value) { m_version = std::move(value); }
    inline void SetVersion(const char* value) { m_version.assign(value); }
    inline CreateFunctionResult& WithVersion(const Aws::String& value) { SetVersion(value); return *this;}
    inline CreateFunctionResult& WithVersion(Aws::String&& value) { SetVersion(std::move(value)); return *this;}
    inline CreateFunctionResult& WithVersion(const char* value) { SetVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's networking configuration.</p>
     */
    inline const VpcConfigResponse& GetVpcConfig() const{ return m_vpcConfig; }
    inline void SetVpcConfig(const VpcConfigResponse& value) { m_vpcConfig = value; }
    inline void SetVpcConfig(VpcConfigResponse&& value) { m_vpcConfig = std::move(value); }
    inline CreateFunctionResult& WithVpcConfig(const VpcConfigResponse& value) { SetVpcConfig(value); return *this;}
    inline CreateFunctionResult& WithVpcConfig(VpcConfigResponse&& value) { SetVpcConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's dead letter queue.</p>
     */
    inline const DeadLetterConfig& GetDeadLetterConfig() const{ return m_deadLetterConfig; }
    inline void SetDeadLetterConfig(const DeadLetterConfig& value) { m_deadLetterConfig = value; }
    inline void SetDeadLetterConfig(DeadLetterConfig&& value) { m_deadLetterConfig = std::move(value); }
    inline CreateFunctionResult& WithDeadLetterConfig(const DeadLetterConfig& value) { SetDeadLetterConfig(value); return *this;}
    inline CreateFunctionResult& WithDeadLetterConfig(DeadLetterConfig&& value) { SetDeadLetterConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-envvars.html">environment
     * variables</a>. Omitted from CloudTrail logs.</p>
     */
    inline const EnvironmentResponse& GetEnvironment() const{ return m_environment; }
    inline void SetEnvironment(const EnvironmentResponse& value) { m_environment = value; }
    inline void SetEnvironment(EnvironmentResponse&& value) { m_environment = std::move(value); }
    inline CreateFunctionResult& WithEnvironment(const EnvironmentResponse& value) { SetEnvironment(value); return *this;}
    inline CreateFunctionResult& WithEnvironment(EnvironmentResponse&& value) { SetEnvironment(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the Key Management Service (KMS) customer managed key that's used
     * to encrypt the following resources:</p> <ul> <li> <p>The function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-envvars.html#configuration-envvars-encryption">environment
     * variables</a>.</p> </li> <li> <p>The function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/snapstart-security.html">Lambda
     * SnapStart</a> snapshots.</p> </li> <li> <p>When used with
     * <code>SourceKMSKeyArn</code>, the unzipped version of the .zip deployment
     * package that's used for function invocations. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/encrypt-zip-package.html#enable-zip-custom-encryption">
     * Specifying a customer managed key for Lambda</a>.</p> </li> <li> <p>The
     * optimized version of the container image that's used for function invocations.
     * Note that this is not the same key that's used to protect your container image
     * in the Amazon Elastic Container Registry (Amazon ECR). For more information, see
     * <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/images-create.html#images-lifecycle">Function
     * lifecycle</a>.</p> </li> </ul> <p>If you don't provide a customer managed key,
     * Lambda uses an <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-owned-cmk">Amazon
     * Web Services owned key</a> or an <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-managed-cmk">Amazon
     * Web Services managed key</a>.</p>
     */
    inline const Aws::String& GetKMSKeyArn() const{ return m_kMSKeyArn; }
    inline void SetKMSKeyArn(const Aws::String& value) { m_kMSKeyArn = value; }
    inline void SetKMSKeyArn(Aws::String&& value) { m_kMSKeyArn = std::move(value); }
    inline void SetKMSKeyArn(const char* value) { m_kMSKeyArn.assign(value); }
    inline CreateFunctionResult& WithKMSKeyArn(const Aws::String& value) { SetKMSKeyArn(value); return *this;}
    inline CreateFunctionResult& WithKMSKeyArn(Aws::String&& value) { SetKMSKeyArn(std::move(value)); return *this;}
    inline CreateFunctionResult& WithKMSKeyArn(const char* value) { SetKMSKeyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's X-Ray tracing configuration.</p>
     */
    inline const TracingConfigResponse& GetTracingConfig() const{ return m_tracingConfig; }
    inline void SetTracingConfig(const TracingConfigResponse& value) { m_tracingConfig = value; }
    inline void SetTracingConfig(TracingConfigResponse&& value) { m_tracingConfig = std::move(value); }
    inline CreateFunctionResult& WithTracingConfig(const TracingConfigResponse& value) { SetTracingConfig(value); return *this;}
    inline CreateFunctionResult& WithTracingConfig(TracingConfigResponse&& value) { SetTracingConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>For Lambda@Edge functions, the ARN of the main function.</p>
     */
    inline const Aws::String& GetMasterArn() const{ return m_masterArn; }
    inline void SetMasterArn(const Aws::String& value) { m_masterArn = value; }
    inline void SetMasterArn(Aws::String&& value) { m_masterArn = std::move(value); }
    inline void SetMasterArn(const char* value) { m_masterArn.assign(value); }
    inline CreateFunctionResult& WithMasterArn(const Aws::String& value) { SetMasterArn(value); return *this;}
    inline CreateFunctionResult& WithMasterArn(Aws::String&& value) { SetMasterArn(std::move(value)); return *this;}
    inline CreateFunctionResult& WithMasterArn(const char* value) { SetMasterArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The latest updated revision of the function or alias.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionId.assign(value); }
    inline CreateFunctionResult& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline CreateFunctionResult& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline CreateFunctionResult& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">layers</a>.</p>
     */
    inline const Aws::Vector<Layer>& GetLayers() const{ return m_layers; }
    inline void SetLayers(const Aws::Vector<Layer>& value) { m_layers = value; }
    inline void SetLayers(Aws::Vector<Layer>&& value) { m_layers = std::move(value); }
    inline CreateFunctionResult& WithLayers(const Aws::Vector<Layer>& value) { SetLayers(value); return *this;}
    inline CreateFunctionResult& WithLayers(Aws::Vector<Layer>&& value) { SetLayers(std::move(value)); return *this;}
    inline CreateFunctionResult& AddLayers(const Layer& value) { m_layers.push_back(value); return *this; }
    inline CreateFunctionResult& AddLayers(Layer&& value) { m_layers.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The current state of the function. When the state is <code>Inactive</code>,
     * you can reactivate the function by invoking it.</p>
     */
    inline const State& GetState() const{ return m_state; }
    inline void SetState(const State& value) { m_state = value; }
    inline void SetState(State&& value) { m_state = std::move(value); }
    inline CreateFunctionResult& WithState(const State& value) { SetState(value); return *this;}
    inline CreateFunctionResult& WithState(State&& value) { SetState(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The reason for the function's current state.</p>
     */
    inline const Aws::String& GetStateReason() const{ return m_stateReason; }
    inline void SetStateReason(const Aws::String& value) { m_stateReason = value; }
    inline void SetStateReason(Aws::String&& value) { m_stateReason = std::move(value); }
    inline void SetStateReason(const char* value) { m_stateReason.assign(value); }
    inline CreateFunctionResult& WithStateReason(const Aws::String& value) { SetStateReason(value); return *this;}
    inline CreateFunctionResult& WithStateReason(Aws::String&& value) { SetStateReason(std::move(value)); return *this;}
    inline CreateFunctionResult& WithStateReason(const char* value) { SetStateReason(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The reason code for the function's current state. When the code is
     * <code>Creating</code>, you can't invoke or modify the function.</p>
     */
    inline const StateReasonCode& GetStateReasonCode() const{ return m_stateReasonCode; }
    inline void SetStateReasonCode(const StateReasonCode& value) { m_stateReasonCode = value; }
    inline void SetStateReasonCode(StateReasonCode&& value) { m_stateReasonCode = std::move(value); }
    inline CreateFunctionResult& WithStateReasonCode(const StateReasonCode& value) { SetStateReasonCode(value); return *this;}
    inline CreateFunctionResult& WithStateReasonCode(StateReasonCode&& value) { SetStateReasonCode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The status of the last update that was performed on the function. This is
     * first set to <code>Successful</code> after function creation completes.</p>
     */
    inline const LastUpdateStatus& GetLastUpdateStatus() const{ return m_lastUpdateStatus; }
    inline void SetLastUpdateStatus(const LastUpdateStatus& value) { m_lastUpdateStatus = value; }
    inline void SetLastUpdateStatus(LastUpdateStatus&& value) { m_lastUpdateStatus = std::move(value); }
    inline CreateFunctionResult& WithLastUpdateStatus(const LastUpdateStatus& value) { SetLastUpdateStatus(value); return *this;}
    inline CreateFunctionResult& WithLastUpdateStatus(LastUpdateStatus&& value) { SetLastUpdateStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The reason for the last update that was performed on the function.</p>
     */
    inline const Aws::String& GetLastUpdateStatusReason() const{ return m_lastUpdateStatusReason; }
    inline void SetLastUpdateStatusReason(const Aws::String& value) { m_lastUpdateStatusReason = value; }
    inline void SetLastUpdateStatusReason(Aws::String&& value) { m_lastUpdateStatusReason = std::move(value); }
    inline void SetLastUpdateStatusReason(const char* value) { m_lastUpdateStatusReason.assign(value); }
    inline CreateFunctionResult& WithLastUpdateStatusReason(const Aws::String& value) { SetLastUpdateStatusReason(value); return *this;}
    inline CreateFunctionResult& WithLastUpdateStatusReason(Aws::String&& value) { SetLastUpdateStatusReason(std::move(value)); return *this;}
    inline CreateFunctionResult& WithLastUpdateStatusReason(const char* value) { SetLastUpdateStatusReason(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The reason code for the last update that was performed on the function.</p>
     */
    inline const LastUpdateStatusReasonCode& GetLastUpdateStatusReasonCode() const{ return m_lastUpdateStatusReasonCode; }
    inline void SetLastUpdateStatusReasonCode(const LastUpdateStatusReasonCode& value) { m_lastUpdateStatusReasonCode = value; }
    inline void SetLastUpdateStatusReasonCode(LastUpdateStatusReasonCode&& value) { m_lastUpdateStatusReasonCode = std::move(value); }
    inline CreateFunctionResult& WithLastUpdateStatusReasonCode(const LastUpdateStatusReasonCode& value) { SetLastUpdateStatusReasonCode(value); return *this;}
    inline CreateFunctionResult& WithLastUpdateStatusReasonCode(LastUpdateStatusReasonCode&& value) { SetLastUpdateStatusReasonCode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Connection settings for an <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-filesystem.html">Amazon
     * EFS file system</a>.</p>
     */
    inline const Aws::Vector<FileSystemConfig>& GetFileSystemConfigs() const{ return m_fileSystemConfigs; }
    inline void SetFileSystemConfigs(const Aws::Vector<FileSystemConfig>& value) { m_fileSystemConfigs = value; }
    inline void SetFileSystemConfigs(Aws::Vector<FileSystemConfig>&& value) { m_fileSystemConfigs = std::move(value); }
    inline CreateFunctionResult& WithFileSystemConfigs(const Aws::Vector<FileSystemConfig>& value) { SetFileSystemConfigs(value); return *this;}
    inline CreateFunctionResult& WithFileSystemConfigs(Aws::Vector<FileSystemConfig>&& value) { SetFileSystemConfigs(std::move(value)); return *this;}
    inline CreateFunctionResult& AddFileSystemConfigs(const FileSystemConfig& value) { m_fileSystemConfigs.push_back(value); return *this; }
    inline CreateFunctionResult& AddFileSystemConfigs(FileSystemConfig&& value) { m_fileSystemConfigs.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The type of deployment package. Set to <code>Image</code> for container image
     * and set <code>Zip</code> for .zip file archive.</p>
     */
    inline const PackageType& GetPackageType() const{ return m_packageType; }
    inline void SetPackageType(const PackageType& value) { m_packageType = value; }
    inline void SetPackageType(PackageType&& value) { m_packageType = std::move(value); }
    inline CreateFunctionResult& WithPackageType(const PackageType& value) { SetPackageType(value); return *this;}
    inline CreateFunctionResult& WithPackageType(PackageType&& value) { SetPackageType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's image configuration values.</p>
     */
    inline const ImageConfigResponse& GetImageConfigResponse() const{ return m_imageConfigResponse; }
    inline void SetImageConfigResponse(const ImageConfigResponse& value) { m_imageConfigResponse = value; }
    inline void SetImageConfigResponse(ImageConfigResponse&& value) { m_imageConfigResponse = std::move(value); }
    inline CreateFunctionResult& WithImageConfigResponse(const ImageConfigResponse& value) { SetImageConfigResponse(value); return *this;}
    inline CreateFunctionResult& WithImageConfigResponse(ImageConfigResponse&& value) { SetImageConfigResponse(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the signing profile version.</p>
     */
    inline const Aws::String& GetSigningProfileVersionArn() const{ return m_signingProfileVersionArn; }
    inline void SetSigningProfileVersionArn(const Aws::String& value) { m_signingProfileVersionArn = value; }
    inline void SetSigningProfileVersionArn(Aws::String&& value) { m_signingProfileVersionArn = std::move(value); }
    inline void SetSigningProfileVersionArn(const char* value) { m_signingProfileVersionArn.assign(value); }
    inline CreateFunctionResult& WithSigningProfileVersionArn(const Aws::String& value) { SetSigningProfileVersionArn(value); return *this;}
    inline CreateFunctionResult& WithSigningProfileVersionArn(Aws::String&& value) { SetSigningProfileVersionArn(std::move(value)); return *this;}
    inline CreateFunctionResult& WithSigningProfileVersionArn(const char* value) { SetSigningProfileVersionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the signing job.</p>
     */
    inline const Aws::String& GetSigningJobArn() const{ return m_signingJobArn; }
    inline void SetSigningJobArn(const Aws::String& value) { m_signingJobArn = value; }
    inline void SetSigningJobArn(Aws::String&& value) { m_signingJobArn = std::move(value); }
    inline void SetSigningJobArn(const char* value) { m_signingJobArn.assign(value); }
    inline CreateFunctionResult& WithSigningJobArn(const Aws::String& value) { SetSigningJobArn(value); return *this;}
    inline CreateFunctionResult& WithSigningJobArn(Aws::String&& value) { SetSigningJobArn(std::move(value)); return *this;}
    inline CreateFunctionResult& WithSigningJobArn(const char* value) { SetSigningJobArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The instruction set architecture that the function supports. Architecture is
     * a string array with one of the valid values. The default architecture value is
     * <code>x86_64</code>.</p>
     */
    inline const Aws::Vector<Architecture>& GetArchitectures() const{ return m_architectures; }
    inline void SetArchitectures(const Aws::Vector<Architecture>& value) { m_architectures = value; }
    inline void SetArchitectures(Aws::Vector<Architecture>&& value) { m_architectures = std::move(value); }
    inline CreateFunctionResult& WithArchitectures(const Aws::Vector<Architecture>& value) { SetArchitectures(value); return *this;}
    inline CreateFunctionResult& WithArchitectures(Aws::Vector<Architecture>&& value) { SetArchitectures(std::move(value)); return *this;}
    inline CreateFunctionResult& AddArchitectures(const Architecture& value) { m_architectures.push_back(value); return *this; }
    inline CreateFunctionResult& AddArchitectures(Architecture&& value) { m_architectures.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The size of the function's <code>/tmp</code> directory in MB. The default
     * value is 512, but can be any whole number between 512 and 10,240 MB. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-function-common.html#configuration-ephemeral-storage">Configuring
     * ephemeral storage (console)</a>.</p>
     */
    inline const EphemeralStorage& GetEphemeralStorage() const{ return m_ephemeralStorage; }
    inline void SetEphemeralStorage(const EphemeralStorage& value) { m_ephemeralStorage = value; }
    inline void SetEphemeralStorage(EphemeralStorage&& value) { m_ephemeralStorage = std::move(value); }
    inline CreateFunctionResult& WithEphemeralStorage(const EphemeralStorage& value) { SetEphemeralStorage(value); return *this;}
    inline CreateFunctionResult& WithEphemeralStorage(EphemeralStorage&& value) { SetEphemeralStorage(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Set <code>ApplyOn</code> to <code>PublishedVersions</code> to create a
     * snapshot of the initialized execution environment when you publish a function
     * version. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/snapstart.html">Improving
     * startup performance with Lambda SnapStart</a>.</p>
     */
    inline const SnapStartResponse& GetSnapStart() const{ return m_snapStart; }
    inline void SetSnapStart(const SnapStartResponse& value) { m_snapStart = value; }
    inline void SetSnapStart(SnapStartResponse&& value) { m_snapStart = std::move(value); }
    inline CreateFunctionResult& WithSnapStart(const SnapStartResponse& value) { SetSnapStart(value); return *this;}
    inline CreateFunctionResult& WithSnapStart(SnapStartResponse&& value) { SetSnapStart(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the runtime and any errors that occured.</p>
     */
    inline const RuntimeVersionConfig& GetRuntimeVersionConfig() const{ return m_runtimeVersionConfig; }
    inline void SetRuntimeVersionConfig(const RuntimeVersionConfig& value) { m_runtimeVersionConfig = value; }
    inline void SetRuntimeVersionConfig(RuntimeVersionConfig&& value) { m_runtimeVersionConfig = std::move(value); }
    inline CreateFunctionResult& WithRuntimeVersionConfig(const RuntimeVersionConfig& value) { SetRuntimeVersionConfig(value); return *this;}
    inline CreateFunctionResult& WithRuntimeVersionConfig(RuntimeVersionConfig&& value) { SetRuntimeVersionConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's Amazon CloudWatch Logs configuration settings.</p>
     */
    inline const LoggingConfig& GetLoggingConfig() const{ return m_loggingConfig; }
    inline void SetLoggingConfig(const LoggingConfig& value) { m_loggingConfig = value; }
    inline void SetLoggingConfig(LoggingConfig&& value) { m_loggingConfig = std::move(value); }
    inline CreateFunctionResult& WithLoggingConfig(const LoggingConfig& value) { SetLoggingConfig(value); return *this;}
    inline CreateFunctionResult& WithLoggingConfig(LoggingConfig&& value) { SetLoggingConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline CreateFunctionResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline CreateFunctionResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline CreateFunctionResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_functionName;

    Aws::String m_functionArn;

    Runtime m_runtime;

    Aws::String m_role;

    Aws::String m_handler;

    long long m_codeSize;

    Aws::String m_description;

    int m_timeout;

    int m_memorySize;

    Aws::String m_lastModified;

    Aws::String m_codeSha256;

    Aws::String m_version;

    VpcConfigResponse m_vpcConfig;

    DeadLetterConfig m_deadLetterConfig;

    EnvironmentResponse m_environment;

    Aws::String m_kMSKeyArn;

    TracingConfigResponse m_tracingConfig;

    Aws::String m_masterArn;

    Aws::String m_revisionId;

    Aws::Vector<Layer> m_layers;

    State m_state;

    Aws::String m_stateReason;

    StateReasonCode m_stateReasonCode;

    LastUpdateStatus m_lastUpdateStatus;

    Aws::String m_lastUpdateStatusReason;

    LastUpdateStatusReasonCode m_lastUpdateStatusReasonCode;

    Aws::Vector<FileSystemConfig> m_fileSystemConfigs;

    PackageType m_packageType;

    ImageConfigResponse m_imageConfigResponse;

    Aws::String m_signingProfileVersionArn;

    Aws::String m_signingJobArn;

    Aws::Vector<Architecture> m_architectures;

    EphemeralStorage m_ephemeralStorage;

    SnapStartResponse m_snapStart;

    RuntimeVersionConfig m_runtimeVersionConfig;

    LoggingConfig m_loggingConfig;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
