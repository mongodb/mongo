/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/Runtime.h>
#include <aws/lambda/model/FunctionCode.h>
#include <aws/lambda/model/VpcConfig.h>
#include <aws/lambda/model/PackageType.h>
#include <aws/lambda/model/DeadLetterConfig.h>
#include <aws/lambda/model/Environment.h>
#include <aws/lambda/model/TracingConfig.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/ImageConfig.h>
#include <aws/lambda/model/EphemeralStorage.h>
#include <aws/lambda/model/SnapStart.h>
#include <aws/lambda/model/LoggingConfig.h>
#include <aws/lambda/model/FileSystemConfig.h>
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
  class CreateFunctionRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API CreateFunctionRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateFunction"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function.</p> <p class="title"> <b>Name
     * formats</b> </p> <ul> <li> <p> <b>Function name</b> –
     * <code>my-function</code>.</p> </li> <li> <p> <b>Function ARN</b> –
     * <code>arn:aws:lambda:us-west-2:123456789012:function:my-function</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> –
     * <code>123456789012:function:my-function</code>.</p> </li> </ul> <p>The length
     * constraint applies only to the full ARN. If you specify only the function name,
     * it is limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline CreateFunctionRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline CreateFunctionRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline CreateFunctionRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
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
    inline bool RuntimeHasBeenSet() const { return m_runtimeHasBeenSet; }
    inline void SetRuntime(const Runtime& value) { m_runtimeHasBeenSet = true; m_runtime = value; }
    inline void SetRuntime(Runtime&& value) { m_runtimeHasBeenSet = true; m_runtime = std::move(value); }
    inline CreateFunctionRequest& WithRuntime(const Runtime& value) { SetRuntime(value); return *this;}
    inline CreateFunctionRequest& WithRuntime(Runtime&& value) { SetRuntime(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the function's execution role.</p>
     */
    inline const Aws::String& GetRole() const{ return m_role; }
    inline bool RoleHasBeenSet() const { return m_roleHasBeenSet; }
    inline void SetRole(const Aws::String& value) { m_roleHasBeenSet = true; m_role = value; }
    inline void SetRole(Aws::String&& value) { m_roleHasBeenSet = true; m_role = std::move(value); }
    inline void SetRole(const char* value) { m_roleHasBeenSet = true; m_role.assign(value); }
    inline CreateFunctionRequest& WithRole(const Aws::String& value) { SetRole(value); return *this;}
    inline CreateFunctionRequest& WithRole(Aws::String&& value) { SetRole(std::move(value)); return *this;}
    inline CreateFunctionRequest& WithRole(const char* value) { SetRole(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the method within your code that Lambda calls to run your
     * function. Handler is required if the deployment package is a .zip file archive.
     * The format includes the file name. It can also include namespaces and other
     * qualifiers, depending on the runtime. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/foundation-progmodel.html">Lambda
     * programming model</a>.</p>
     */
    inline const Aws::String& GetHandler() const{ return m_handler; }
    inline bool HandlerHasBeenSet() const { return m_handlerHasBeenSet; }
    inline void SetHandler(const Aws::String& value) { m_handlerHasBeenSet = true; m_handler = value; }
    inline void SetHandler(Aws::String&& value) { m_handlerHasBeenSet = true; m_handler = std::move(value); }
    inline void SetHandler(const char* value) { m_handlerHasBeenSet = true; m_handler.assign(value); }
    inline CreateFunctionRequest& WithHandler(const Aws::String& value) { SetHandler(value); return *this;}
    inline CreateFunctionRequest& WithHandler(Aws::String&& value) { SetHandler(std::move(value)); return *this;}
    inline CreateFunctionRequest& WithHandler(const char* value) { SetHandler(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The code for the function.</p>
     */
    inline const FunctionCode& GetCode() const{ return m_code; }
    inline bool CodeHasBeenSet() const { return m_codeHasBeenSet; }
    inline void SetCode(const FunctionCode& value) { m_codeHasBeenSet = true; m_code = value; }
    inline void SetCode(FunctionCode&& value) { m_codeHasBeenSet = true; m_code = std::move(value); }
    inline CreateFunctionRequest& WithCode(const FunctionCode& value) { SetCode(value); return *this;}
    inline CreateFunctionRequest& WithCode(FunctionCode&& value) { SetCode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A description of the function.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline CreateFunctionRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline CreateFunctionRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline CreateFunctionRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The amount of time (in seconds) that Lambda allows a function to run before
     * stopping it. The default is 3 seconds. The maximum allowed value is 900 seconds.
     * For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/runtimes-context.html">Lambda
     * execution environment</a>.</p>
     */
    inline int GetTimeout() const{ return m_timeout; }
    inline bool TimeoutHasBeenSet() const { return m_timeoutHasBeenSet; }
    inline void SetTimeout(int value) { m_timeoutHasBeenSet = true; m_timeout = value; }
    inline CreateFunctionRequest& WithTimeout(int value) { SetTimeout(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The amount of <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-function-common.html#configuration-memory-console">memory
     * available to the function</a> at runtime. Increasing the function memory also
     * increases its CPU allocation. The default value is 128 MB. The value can be any
     * multiple of 1 MB.</p>
     */
    inline int GetMemorySize() const{ return m_memorySize; }
    inline bool MemorySizeHasBeenSet() const { return m_memorySizeHasBeenSet; }
    inline void SetMemorySize(int value) { m_memorySizeHasBeenSet = true; m_memorySize = value; }
    inline CreateFunctionRequest& WithMemorySize(int value) { SetMemorySize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Set to true to publish the first version of the function during creation.</p>
     */
    inline bool GetPublish() const{ return m_publish; }
    inline bool PublishHasBeenSet() const { return m_publishHasBeenSet; }
    inline void SetPublish(bool value) { m_publishHasBeenSet = true; m_publish = value; }
    inline CreateFunctionRequest& WithPublish(bool value) { SetPublish(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>For network connectivity to Amazon Web Services resources in a VPC, specify a
     * list of security groups and subnets in the VPC. When you connect a function to a
     * VPC, it can access resources and the internet only through that VPC. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-vpc.html">Configuring
     * a Lambda function to access resources in a VPC</a>.</p>
     */
    inline const VpcConfig& GetVpcConfig() const{ return m_vpcConfig; }
    inline bool VpcConfigHasBeenSet() const { return m_vpcConfigHasBeenSet; }
    inline void SetVpcConfig(const VpcConfig& value) { m_vpcConfigHasBeenSet = true; m_vpcConfig = value; }
    inline void SetVpcConfig(VpcConfig&& value) { m_vpcConfigHasBeenSet = true; m_vpcConfig = std::move(value); }
    inline CreateFunctionRequest& WithVpcConfig(const VpcConfig& value) { SetVpcConfig(value); return *this;}
    inline CreateFunctionRequest& WithVpcConfig(VpcConfig&& value) { SetVpcConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of deployment package. Set to <code>Image</code> for container image
     * and set to <code>Zip</code> for .zip file archive.</p>
     */
    inline const PackageType& GetPackageType() const{ return m_packageType; }
    inline bool PackageTypeHasBeenSet() const { return m_packageTypeHasBeenSet; }
    inline void SetPackageType(const PackageType& value) { m_packageTypeHasBeenSet = true; m_packageType = value; }
    inline void SetPackageType(PackageType&& value) { m_packageTypeHasBeenSet = true; m_packageType = std::move(value); }
    inline CreateFunctionRequest& WithPackageType(const PackageType& value) { SetPackageType(value); return *this;}
    inline CreateFunctionRequest& WithPackageType(PackageType&& value) { SetPackageType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A dead-letter queue configuration that specifies the queue or topic where
     * Lambda sends asynchronous events when they fail processing. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html#invocation-dlq">Dead-letter
     * queues</a>.</p>
     */
    inline const DeadLetterConfig& GetDeadLetterConfig() const{ return m_deadLetterConfig; }
    inline bool DeadLetterConfigHasBeenSet() const { return m_deadLetterConfigHasBeenSet; }
    inline void SetDeadLetterConfig(const DeadLetterConfig& value) { m_deadLetterConfigHasBeenSet = true; m_deadLetterConfig = value; }
    inline void SetDeadLetterConfig(DeadLetterConfig&& value) { m_deadLetterConfigHasBeenSet = true; m_deadLetterConfig = std::move(value); }
    inline CreateFunctionRequest& WithDeadLetterConfig(const DeadLetterConfig& value) { SetDeadLetterConfig(value); return *this;}
    inline CreateFunctionRequest& WithDeadLetterConfig(DeadLetterConfig&& value) { SetDeadLetterConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Environment variables that are accessible from function code during
     * execution.</p>
     */
    inline const Environment& GetEnvironment() const{ return m_environment; }
    inline bool EnvironmentHasBeenSet() const { return m_environmentHasBeenSet; }
    inline void SetEnvironment(const Environment& value) { m_environmentHasBeenSet = true; m_environment = value; }
    inline void SetEnvironment(Environment&& value) { m_environmentHasBeenSet = true; m_environment = std::move(value); }
    inline CreateFunctionRequest& WithEnvironment(const Environment& value) { SetEnvironment(value); return *this;}
    inline CreateFunctionRequest& WithEnvironment(Environment&& value) { SetEnvironment(std::move(value)); return *this;}
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
    inline bool KMSKeyArnHasBeenSet() const { return m_kMSKeyArnHasBeenSet; }
    inline void SetKMSKeyArn(const Aws::String& value) { m_kMSKeyArnHasBeenSet = true; m_kMSKeyArn = value; }
    inline void SetKMSKeyArn(Aws::String&& value) { m_kMSKeyArnHasBeenSet = true; m_kMSKeyArn = std::move(value); }
    inline void SetKMSKeyArn(const char* value) { m_kMSKeyArnHasBeenSet = true; m_kMSKeyArn.assign(value); }
    inline CreateFunctionRequest& WithKMSKeyArn(const Aws::String& value) { SetKMSKeyArn(value); return *this;}
    inline CreateFunctionRequest& WithKMSKeyArn(Aws::String&& value) { SetKMSKeyArn(std::move(value)); return *this;}
    inline CreateFunctionRequest& WithKMSKeyArn(const char* value) { SetKMSKeyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Set <code>Mode</code> to <code>Active</code> to sample and trace a subset of
     * incoming requests with <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/services-xray.html">X-Ray</a>.</p>
     */
    inline const TracingConfig& GetTracingConfig() const{ return m_tracingConfig; }
    inline bool TracingConfigHasBeenSet() const { return m_tracingConfigHasBeenSet; }
    inline void SetTracingConfig(const TracingConfig& value) { m_tracingConfigHasBeenSet = true; m_tracingConfig = value; }
    inline void SetTracingConfig(TracingConfig&& value) { m_tracingConfigHasBeenSet = true; m_tracingConfig = std::move(value); }
    inline CreateFunctionRequest& WithTracingConfig(const TracingConfig& value) { SetTracingConfig(value); return *this;}
    inline CreateFunctionRequest& WithTracingConfig(TracingConfig&& value) { SetTracingConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/tagging.html">tags</a> to
     * apply to the function.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Map<Aws::String, Aws::String>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Map<Aws::String, Aws::String>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline CreateFunctionRequest& WithTags(const Aws::Map<Aws::String, Aws::String>& value) { SetTags(value); return *this;}
    inline CreateFunctionRequest& WithTags(Aws::Map<Aws::String, Aws::String>&& value) { SetTags(std::move(value)); return *this;}
    inline CreateFunctionRequest& AddTags(const Aws::String& key, const Aws::String& value) { m_tagsHasBeenSet = true; m_tags.emplace(key, value); return *this; }
    inline CreateFunctionRequest& AddTags(Aws::String&& key, const Aws::String& value) { m_tagsHasBeenSet = true; m_tags.emplace(std::move(key), value); return *this; }
    inline CreateFunctionRequest& AddTags(const Aws::String& key, Aws::String&& value) { m_tagsHasBeenSet = true; m_tags.emplace(key, std::move(value)); return *this; }
    inline CreateFunctionRequest& AddTags(Aws::String&& key, Aws::String&& value) { m_tagsHasBeenSet = true; m_tags.emplace(std::move(key), std::move(value)); return *this; }
    inline CreateFunctionRequest& AddTags(const char* key, Aws::String&& value) { m_tagsHasBeenSet = true; m_tags.emplace(key, std::move(value)); return *this; }
    inline CreateFunctionRequest& AddTags(Aws::String&& key, const char* value) { m_tagsHasBeenSet = true; m_tags.emplace(std::move(key), value); return *this; }
    inline CreateFunctionRequest& AddTags(const char* key, const char* value) { m_tagsHasBeenSet = true; m_tags.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">function
     * layers</a> to add to the function's execution environment. Specify each layer by
     * its ARN, including the version.</p>
     */
    inline const Aws::Vector<Aws::String>& GetLayers() const{ return m_layers; }
    inline bool LayersHasBeenSet() const { return m_layersHasBeenSet; }
    inline void SetLayers(const Aws::Vector<Aws::String>& value) { m_layersHasBeenSet = true; m_layers = value; }
    inline void SetLayers(Aws::Vector<Aws::String>&& value) { m_layersHasBeenSet = true; m_layers = std::move(value); }
    inline CreateFunctionRequest& WithLayers(const Aws::Vector<Aws::String>& value) { SetLayers(value); return *this;}
    inline CreateFunctionRequest& WithLayers(Aws::Vector<Aws::String>&& value) { SetLayers(std::move(value)); return *this;}
    inline CreateFunctionRequest& AddLayers(const Aws::String& value) { m_layersHasBeenSet = true; m_layers.push_back(value); return *this; }
    inline CreateFunctionRequest& AddLayers(Aws::String&& value) { m_layersHasBeenSet = true; m_layers.push_back(std::move(value)); return *this; }
    inline CreateFunctionRequest& AddLayers(const char* value) { m_layersHasBeenSet = true; m_layers.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Connection settings for an Amazon EFS file system.</p>
     */
    inline const Aws::Vector<FileSystemConfig>& GetFileSystemConfigs() const{ return m_fileSystemConfigs; }
    inline bool FileSystemConfigsHasBeenSet() const { return m_fileSystemConfigsHasBeenSet; }
    inline void SetFileSystemConfigs(const Aws::Vector<FileSystemConfig>& value) { m_fileSystemConfigsHasBeenSet = true; m_fileSystemConfigs = value; }
    inline void SetFileSystemConfigs(Aws::Vector<FileSystemConfig>&& value) { m_fileSystemConfigsHasBeenSet = true; m_fileSystemConfigs = std::move(value); }
    inline CreateFunctionRequest& WithFileSystemConfigs(const Aws::Vector<FileSystemConfig>& value) { SetFileSystemConfigs(value); return *this;}
    inline CreateFunctionRequest& WithFileSystemConfigs(Aws::Vector<FileSystemConfig>&& value) { SetFileSystemConfigs(std::move(value)); return *this;}
    inline CreateFunctionRequest& AddFileSystemConfigs(const FileSystemConfig& value) { m_fileSystemConfigsHasBeenSet = true; m_fileSystemConfigs.push_back(value); return *this; }
    inline CreateFunctionRequest& AddFileSystemConfigs(FileSystemConfig&& value) { m_fileSystemConfigsHasBeenSet = true; m_fileSystemConfigs.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Container image <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/images-create.html#images-parms">configuration
     * values</a> that override the values in the container image Dockerfile.</p>
     */
    inline const ImageConfig& GetImageConfig() const{ return m_imageConfig; }
    inline bool ImageConfigHasBeenSet() const { return m_imageConfigHasBeenSet; }
    inline void SetImageConfig(const ImageConfig& value) { m_imageConfigHasBeenSet = true; m_imageConfig = value; }
    inline void SetImageConfig(ImageConfig&& value) { m_imageConfigHasBeenSet = true; m_imageConfig = std::move(value); }
    inline CreateFunctionRequest& WithImageConfig(const ImageConfig& value) { SetImageConfig(value); return *this;}
    inline CreateFunctionRequest& WithImageConfig(ImageConfig&& value) { SetImageConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>To enable code signing for this function, specify the ARN of a code-signing
     * configuration. A code-signing configuration includes a set of signing profiles,
     * which define the trusted publishers for this function.</p>
     */
    inline const Aws::String& GetCodeSigningConfigArn() const{ return m_codeSigningConfigArn; }
    inline bool CodeSigningConfigArnHasBeenSet() const { return m_codeSigningConfigArnHasBeenSet; }
    inline void SetCodeSigningConfigArn(const Aws::String& value) { m_codeSigningConfigArnHasBeenSet = true; m_codeSigningConfigArn = value; }
    inline void SetCodeSigningConfigArn(Aws::String&& value) { m_codeSigningConfigArnHasBeenSet = true; m_codeSigningConfigArn = std::move(value); }
    inline void SetCodeSigningConfigArn(const char* value) { m_codeSigningConfigArnHasBeenSet = true; m_codeSigningConfigArn.assign(value); }
    inline CreateFunctionRequest& WithCodeSigningConfigArn(const Aws::String& value) { SetCodeSigningConfigArn(value); return *this;}
    inline CreateFunctionRequest& WithCodeSigningConfigArn(Aws::String&& value) { SetCodeSigningConfigArn(std::move(value)); return *this;}
    inline CreateFunctionRequest& WithCodeSigningConfigArn(const char* value) { SetCodeSigningConfigArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The instruction set architecture that the function supports. Enter a string
     * array with one of the valid values (arm64 or x86_64). The default value is
     * <code>x86_64</code>.</p>
     */
    inline const Aws::Vector<Architecture>& GetArchitectures() const{ return m_architectures; }
    inline bool ArchitecturesHasBeenSet() const { return m_architecturesHasBeenSet; }
    inline void SetArchitectures(const Aws::Vector<Architecture>& value) { m_architecturesHasBeenSet = true; m_architectures = value; }
    inline void SetArchitectures(Aws::Vector<Architecture>&& value) { m_architecturesHasBeenSet = true; m_architectures = std::move(value); }
    inline CreateFunctionRequest& WithArchitectures(const Aws::Vector<Architecture>& value) { SetArchitectures(value); return *this;}
    inline CreateFunctionRequest& WithArchitectures(Aws::Vector<Architecture>&& value) { SetArchitectures(std::move(value)); return *this;}
    inline CreateFunctionRequest& AddArchitectures(const Architecture& value) { m_architecturesHasBeenSet = true; m_architectures.push_back(value); return *this; }
    inline CreateFunctionRequest& AddArchitectures(Architecture&& value) { m_architecturesHasBeenSet = true; m_architectures.push_back(std::move(value)); return *this; }
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
    inline bool EphemeralStorageHasBeenSet() const { return m_ephemeralStorageHasBeenSet; }
    inline void SetEphemeralStorage(const EphemeralStorage& value) { m_ephemeralStorageHasBeenSet = true; m_ephemeralStorage = value; }
    inline void SetEphemeralStorage(EphemeralStorage&& value) { m_ephemeralStorageHasBeenSet = true; m_ephemeralStorage = std::move(value); }
    inline CreateFunctionRequest& WithEphemeralStorage(const EphemeralStorage& value) { SetEphemeralStorage(value); return *this;}
    inline CreateFunctionRequest& WithEphemeralStorage(EphemeralStorage&& value) { SetEphemeralStorage(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/snapstart.html">SnapStart</a>
     * setting.</p>
     */
    inline const SnapStart& GetSnapStart() const{ return m_snapStart; }
    inline bool SnapStartHasBeenSet() const { return m_snapStartHasBeenSet; }
    inline void SetSnapStart(const SnapStart& value) { m_snapStartHasBeenSet = true; m_snapStart = value; }
    inline void SetSnapStart(SnapStart&& value) { m_snapStartHasBeenSet = true; m_snapStart = std::move(value); }
    inline CreateFunctionRequest& WithSnapStart(const SnapStart& value) { SetSnapStart(value); return *this;}
    inline CreateFunctionRequest& WithSnapStart(SnapStart&& value) { SetSnapStart(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function's Amazon CloudWatch Logs configuration settings.</p>
     */
    inline const LoggingConfig& GetLoggingConfig() const{ return m_loggingConfig; }
    inline bool LoggingConfigHasBeenSet() const { return m_loggingConfigHasBeenSet; }
    inline void SetLoggingConfig(const LoggingConfig& value) { m_loggingConfigHasBeenSet = true; m_loggingConfig = value; }
    inline void SetLoggingConfig(LoggingConfig&& value) { m_loggingConfigHasBeenSet = true; m_loggingConfig = std::move(value); }
    inline CreateFunctionRequest& WithLoggingConfig(const LoggingConfig& value) { SetLoggingConfig(value); return *this;}
    inline CreateFunctionRequest& WithLoggingConfig(LoggingConfig&& value) { SetLoggingConfig(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Runtime m_runtime;
    bool m_runtimeHasBeenSet = false;

    Aws::String m_role;
    bool m_roleHasBeenSet = false;

    Aws::String m_handler;
    bool m_handlerHasBeenSet = false;

    FunctionCode m_code;
    bool m_codeHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    int m_timeout;
    bool m_timeoutHasBeenSet = false;

    int m_memorySize;
    bool m_memorySizeHasBeenSet = false;

    bool m_publish;
    bool m_publishHasBeenSet = false;

    VpcConfig m_vpcConfig;
    bool m_vpcConfigHasBeenSet = false;

    PackageType m_packageType;
    bool m_packageTypeHasBeenSet = false;

    DeadLetterConfig m_deadLetterConfig;
    bool m_deadLetterConfigHasBeenSet = false;

    Environment m_environment;
    bool m_environmentHasBeenSet = false;

    Aws::String m_kMSKeyArn;
    bool m_kMSKeyArnHasBeenSet = false;

    TracingConfig m_tracingConfig;
    bool m_tracingConfigHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_tags;
    bool m_tagsHasBeenSet = false;

    Aws::Vector<Aws::String> m_layers;
    bool m_layersHasBeenSet = false;

    Aws::Vector<FileSystemConfig> m_fileSystemConfigs;
    bool m_fileSystemConfigsHasBeenSet = false;

    ImageConfig m_imageConfig;
    bool m_imageConfigHasBeenSet = false;

    Aws::String m_codeSigningConfigArn;
    bool m_codeSigningConfigArnHasBeenSet = false;

    Aws::Vector<Architecture> m_architectures;
    bool m_architecturesHasBeenSet = false;

    EphemeralStorage m_ephemeralStorage;
    bool m_ephemeralStorageHasBeenSet = false;

    SnapStart m_snapStart;
    bool m_snapStartHasBeenSet = false;

    LoggingConfig m_loggingConfig;
    bool m_loggingConfigHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
