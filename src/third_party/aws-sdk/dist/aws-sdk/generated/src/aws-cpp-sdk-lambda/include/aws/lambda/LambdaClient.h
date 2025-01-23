/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/AWSClient.h>
#include <aws/core/client/AWSClientAsyncCRTP.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/lambda/LambdaServiceClientModel.h>

namespace Aws
{
namespace Lambda
{
  /**
   * <fullname>Lambda</fullname> <p> <b>Overview</b> </p> <p>Lambda is a compute
   * service that lets you run code without provisioning or managing servers. Lambda
   * runs your code on a high-availability compute infrastructure and performs all of
   * the administration of the compute resources, including server and operating
   * system maintenance, capacity provisioning and automatic scaling, code monitoring
   * and logging. With Lambda, you can run code for virtually any type of application
   * or backend service. For more information about the Lambda service, see <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/welcome.html">What is
   * Lambda</a> in the <b>Lambda Developer Guide</b>.</p> <p>The <i>Lambda API
   * Reference</i> provides information about each of the API methods, including
   * details about the parameters in each API request and response. </p> <p/> <p>You
   * can use Software Development Kits (SDKs), Integrated Development Environment
   * (IDE) Toolkits, and command line tools to access the API. For installation
   * instructions, see <a href="http://aws.amazon.com/tools/">Tools for Amazon Web
   * Services</a>. </p> <p>For a list of Region-specific endpoints that Lambda
   * supports, see <a
   * href="https://docs.aws.amazon.com/general/latest/gr/lambda-service.html">Lambda
   * endpoints and quotas </a> in the <i>Amazon Web Services General Reference.</i>.
   * </p> <p>When making the API calls, you will need to authenticate your request by
   * providing a signature. Lambda supports signature version 4. For more
   * information, see <a
   * href="https://docs.aws.amazon.com/general/latest/gr/signature-version-4.html">Signature
   * Version 4 signing process</a> in the <i>Amazon Web Services General
   * Reference.</i>. </p> <p> <b>CA certificates</b> </p> <p>Because Amazon Web
   * Services SDKs use the CA certificates from your computer, changes to the
   * certificates on the Amazon Web Services servers can cause connection failures
   * when you attempt to use an SDK. You can prevent these failures by keeping your
   * computer's CA certificates and operating system up-to-date. If you encounter
   * this issue in a corporate environment and do not manage your own computer, you
   * might need to ask an administrator to assist with the update process. The
   * following list shows minimum operating system and Java versions:</p> <ul> <li>
   * <p>Microsoft Windows versions that have updates from January 2005 or later
   * installed contain at least one of the required CAs in their trust list. </p>
   * </li> <li> <p>Mac OS X 10.4 with Java for Mac OS X 10.4 Release 5 (February
   * 2007), Mac OS X 10.5 (October 2007), and later versions contain at least one of
   * the required CAs in their trust list. </p> </li> <li> <p>Red Hat Enterprise
   * Linux 5 (March 2007), 6, and 7 and CentOS 5, 6, and 7 all contain at least one
   * of the required CAs in their default trusted CA list. </p> </li> <li> <p>Java
   * 1.4.2_12 (May 2006), 5 Update 2 (March 2005), and all later versions, including
   * Java 6 (December 2006), 7, and 8, contain at least one of the required CAs in
   * their default trusted CA list. </p> </li> </ul> <p>When accessing the Lambda
   * management console or Lambda API endpoints, whether through browsers or
   * programmatically, you will need to ensure your client machines support any of
   * the following CAs: </p> <ul> <li> <p>Amazon Root CA 1</p> </li> <li>
   * <p>Starfield Services Root Certificate Authority - G2</p> </li> <li>
   * <p>Starfield Class 2 Certification Authority</p> </li> </ul> <p>Root
   * certificates from the first two authorities are available from <a
   * href="https://www.amazontrust.com/repository/">Amazon trust services</a>, but
   * keeping your computer up-to-date is the more straightforward solution. To learn
   * more about ACM-provided certificates, see <a
   * href="http://aws.amazon.com/certificate-manager/faqs/#certificates">Amazon Web
   * Services Certificate Manager FAQs.</a> </p>
   */
  class AWS_LAMBDA_API LambdaClient : public Aws::Client::AWSJsonClient, public Aws::Client::ClientWithAsyncTemplateMethods<LambdaClient>
  {
    public:
      typedef Aws::Client::AWSJsonClient BASECLASS;
      static const char* GetServiceName();
      static const char* GetAllocationTag();

      typedef LambdaClientConfiguration ClientConfigurationType;
      typedef LambdaEndpointProvider EndpointProviderType;

       /**
        * Initializes client to use DefaultCredentialProviderChain, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        LambdaClient(const Aws::Lambda::LambdaClientConfiguration& clientConfiguration = Aws::Lambda::LambdaClientConfiguration(),
                     std::shared_ptr<LambdaEndpointProviderBase> endpointProvider = nullptr);

       /**
        * Initializes client to use SimpleAWSCredentialsProvider, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        LambdaClient(const Aws::Auth::AWSCredentials& credentials,
                     std::shared_ptr<LambdaEndpointProviderBase> endpointProvider = nullptr,
                     const Aws::Lambda::LambdaClientConfiguration& clientConfiguration = Aws::Lambda::LambdaClientConfiguration());

       /**
        * Initializes client to use specified credentials provider with specified client config. If http client factory is not supplied,
        * the default http client factory will be used
        */
        LambdaClient(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& credentialsProvider,
                     std::shared_ptr<LambdaEndpointProviderBase> endpointProvider = nullptr,
                     const Aws::Lambda::LambdaClientConfiguration& clientConfiguration = Aws::Lambda::LambdaClientConfiguration());


        /* Legacy constructors due deprecation */
       /**
        * Initializes client to use DefaultCredentialProviderChain, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        LambdaClient(const Aws::Client::ClientConfiguration& clientConfiguration);

       /**
        * Initializes client to use SimpleAWSCredentialsProvider, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        LambdaClient(const Aws::Auth::AWSCredentials& credentials,
                     const Aws::Client::ClientConfiguration& clientConfiguration);

       /**
        * Initializes client to use specified credentials provider with specified client config. If http client factory is not supplied,
        * the default http client factory will be used
        */
        LambdaClient(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& credentialsProvider,
                     const Aws::Client::ClientConfiguration& clientConfiguration);

        /* End of legacy constructors due deprecation */
        virtual ~LambdaClient();

        /**
         * <p>Adds permissions to the resource-based policy of a version of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>. Use this action to grant layer usage permission to other accounts.
         * You can grant permission to a single account, all accounts in an organization,
         * or all Amazon Web Services accounts. </p> <p>To revoke permission, call
         * <a>RemoveLayerVersionPermission</a> with the statement ID that you specified
         * when you added it.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/AddLayerVersionPermission">AWS
         * API Reference</a></p>
         */
        virtual Model::AddLayerVersionPermissionOutcome AddLayerVersionPermission(const Model::AddLayerVersionPermissionRequest& request) const;

        /**
         * A Callable wrapper for AddLayerVersionPermission that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename AddLayerVersionPermissionRequestT = Model::AddLayerVersionPermissionRequest>
        Model::AddLayerVersionPermissionOutcomeCallable AddLayerVersionPermissionCallable(const AddLayerVersionPermissionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::AddLayerVersionPermission, request);
        }

        /**
         * An Async wrapper for AddLayerVersionPermission that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename AddLayerVersionPermissionRequestT = Model::AddLayerVersionPermissionRequest>
        void AddLayerVersionPermissionAsync(const AddLayerVersionPermissionRequestT& request, const AddLayerVersionPermissionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::AddLayerVersionPermission, request, handler, context);
        }

        /**
         * <p>Grants a <a
         * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_elements_principal.html#Principal_specifying">principal</a>
         * permission to use a function. You can apply the policy at the function level, or
         * specify a qualifier to restrict access to a single version or alias. If you use
         * a qualifier, the invoker must use the full Amazon Resource Name (ARN) of that
         * version or alias to invoke the function. Note: Lambda does not support adding
         * policies to version $LATEST.</p> <p>To grant permission to another account,
         * specify the account ID as the <code>Principal</code>. To grant permission to an
         * organization defined in Organizations, specify the organization ID as the
         * <code>PrincipalOrgID</code>. For Amazon Web Services services, the principal is
         * a domain-style identifier that the service defines, such as
         * <code>s3.amazonaws.com</code> or <code>sns.amazonaws.com</code>. For Amazon Web
         * Services services, you can also specify the ARN of the associated resource as
         * the <code>SourceArn</code>. If you grant permission to a service principal
         * without specifying the source, other accounts could potentially configure
         * resources in their account to invoke your Lambda function.</p> <p>This operation
         * adds a statement to a resource-based permissions policy for the function. For
         * more information about function policies, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/access-control-resource-based.html">Using
         * resource-based policies for Lambda</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/AddPermission">AWS
         * API Reference</a></p>
         */
        virtual Model::AddPermissionOutcome AddPermission(const Model::AddPermissionRequest& request) const;

        /**
         * A Callable wrapper for AddPermission that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename AddPermissionRequestT = Model::AddPermissionRequest>
        Model::AddPermissionOutcomeCallable AddPermissionCallable(const AddPermissionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::AddPermission, request);
        }

        /**
         * An Async wrapper for AddPermission that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename AddPermissionRequestT = Model::AddPermissionRequest>
        void AddPermissionAsync(const AddPermissionRequestT& request, const AddPermissionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::AddPermission, request, handler, context);
        }

        /**
         * <p>Creates an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias</a>
         * for a Lambda function version. Use aliases to provide clients with a function
         * identifier that you can update to invoke a different version.</p> <p>You can
         * also map an alias to split invocation requests between two versions. Use the
         * <code>RoutingConfig</code> parameter to specify a second version and the
         * percentage of invocation requests that it receives.</p><p><h3>See Also:</h3>  
         * <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CreateAlias">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateAliasOutcome CreateAlias(const Model::CreateAliasRequest& request) const;

        /**
         * A Callable wrapper for CreateAlias that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateAliasRequestT = Model::CreateAliasRequest>
        Model::CreateAliasOutcomeCallable CreateAliasCallable(const CreateAliasRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::CreateAlias, request);
        }

        /**
         * An Async wrapper for CreateAlias that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateAliasRequestT = Model::CreateAliasRequest>
        void CreateAliasAsync(const CreateAliasRequestT& request, const CreateAliasResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::CreateAlias, request, handler, context);
        }

        /**
         * <p>Creates a code signing configuration. A <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-codesigning.html">code
         * signing configuration</a> defines a list of allowed signing profiles and defines
         * the code-signing validation policy (action to be taken if deployment validation
         * checks fail). </p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CreateCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateCodeSigningConfigOutcome CreateCodeSigningConfig(const Model::CreateCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for CreateCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateCodeSigningConfigRequestT = Model::CreateCodeSigningConfigRequest>
        Model::CreateCodeSigningConfigOutcomeCallable CreateCodeSigningConfigCallable(const CreateCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::CreateCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for CreateCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateCodeSigningConfigRequestT = Model::CreateCodeSigningConfigRequest>
        void CreateCodeSigningConfigAsync(const CreateCodeSigningConfigRequestT& request, const CreateCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::CreateCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Creates a mapping between an event source and an Lambda function. Lambda
         * reads items from the event source and invokes the function.</p> <p>For details
         * about how to configure different event sources, see the following topics. </p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-ddb.html#services-dynamodb-eventsourcemapping">
         * Amazon DynamoDB Streams</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-kinesis.html#services-kinesis-eventsourcemapping">
         * Amazon Kinesis</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-sqs.html#events-sqs-eventsource">
         * Amazon SQS</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-mq.html#services-mq-eventsourcemapping">
         * Amazon MQ and RabbitMQ</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-msk.html"> Amazon
         * MSK</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/kafka-smaa.html"> Apache
         * Kafka</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-documentdb.html"> Amazon
         * DocumentDB</a> </p> </li> </ul> <p>The following error handling options are
         * available only for DynamoDB and Kinesis event sources:</p> <ul> <li> <p>
         * <code>BisectBatchOnFunctionError</code> – If the function returns an error,
         * split the batch in two and retry.</p> </li> <li> <p>
         * <code>MaximumRecordAgeInSeconds</code> – Discard records older than the
         * specified age. The default value is infinite (-1). When set to infinite (-1),
         * failed records are retried until the record expires</p> </li> <li> <p>
         * <code>MaximumRetryAttempts</code> – Discard records after the specified number
         * of retries. The default value is infinite (-1). When set to infinite (-1),
         * failed records are retried until the record expires.</p> </li> <li> <p>
         * <code>ParallelizationFactor</code> – Process multiple batches from each shard
         * concurrently.</p> </li> </ul> <p>For stream sources (DynamoDB, Kinesis, Amazon
         * MSK, and self-managed Apache Kafka), the following option is also available:</p>
         * <ul> <li> <p> <code>DestinationConfig</code> – Send discarded records to an
         * Amazon SQS queue, Amazon SNS topic, or Amazon S3 bucket.</p> </li> </ul> <p>For
         * information about which configuration parameters apply to each event source, see
         * the following topics.</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-ddb.html#services-ddb-params">
         * Amazon DynamoDB Streams</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-kinesis.html#services-kinesis-params">
         * Amazon Kinesis</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-sqs.html#services-sqs-params">
         * Amazon SQS</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-mq.html#services-mq-params">
         * Amazon MQ and RabbitMQ</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-msk.html#services-msk-parms">
         * Amazon MSK</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-kafka.html#services-kafka-parms">
         * Apache Kafka</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-documentdb.html#docdb-configuration">
         * Amazon DocumentDB</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CreateEventSourceMapping">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateEventSourceMappingOutcome CreateEventSourceMapping(const Model::CreateEventSourceMappingRequest& request) const;

        /**
         * A Callable wrapper for CreateEventSourceMapping that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateEventSourceMappingRequestT = Model::CreateEventSourceMappingRequest>
        Model::CreateEventSourceMappingOutcomeCallable CreateEventSourceMappingCallable(const CreateEventSourceMappingRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::CreateEventSourceMapping, request);
        }

        /**
         * An Async wrapper for CreateEventSourceMapping that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateEventSourceMappingRequestT = Model::CreateEventSourceMappingRequest>
        void CreateEventSourceMappingAsync(const CreateEventSourceMappingRequestT& request, const CreateEventSourceMappingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::CreateEventSourceMapping, request, handler, context);
        }

        /**
         * <p>Creates a Lambda function. To create a function, you need a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/gettingstarted-package.html">deployment
         * package</a> and an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/intro-permission-model.html#lambda-intro-execution-role">execution
         * role</a>. The deployment package is a .zip file archive or container image that
         * contains your function code. The execution role grants the function permission
         * to use Amazon Web Services services, such as Amazon CloudWatch Logs for log
         * streaming and X-Ray for request tracing.</p> <p>If the deployment package is a
         * <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-images.html">container
         * image</a>, then you set the package type to <code>Image</code>. For a container
         * image, the code property must include the URI of a container image in the Amazon
         * ECR registry. You do not need to specify the handler and runtime properties.</p>
         * <p>If the deployment package is a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/gettingstarted-package.html#gettingstarted-package-zip">.zip
         * file archive</a>, then you set the package type to <code>Zip</code>. For a .zip
         * file archive, the code property specifies the location of the .zip file. You
         * must also specify the handler and runtime properties. The code in the deployment
         * package must be compatible with the target instruction set architecture of the
         * function (<code>x86-64</code> or <code>arm64</code>). If you do not specify the
         * architecture, then the default value is <code>x86-64</code>.</p> <p>When you
         * create a function, Lambda provisions an instance of the function and its
         * supporting resources. If your function connects to a VPC, this process can take
         * a minute or so. During this time, you can't invoke or modify the function. The
         * <code>State</code>, <code>StateReason</code>, and <code>StateReasonCode</code>
         * fields in the response from <a>GetFunctionConfiguration</a> indicate when the
         * function is ready to invoke. For more information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/functions-states.html">Lambda
         * function states</a>.</p> <p>A function has an unpublished version, and can have
         * published versions and aliases. The unpublished version changes when you update
         * your function's code and configuration. A published version is a snapshot of
         * your function code and configuration that can't be changed. An alias is a named
         * resource that maps to a version, and can be changed to map to a different
         * version. Use the <code>Publish</code> parameter to create version <code>1</code>
         * of your function from its initial configuration.</p> <p>The other parameters let
         * you configure version-specific and function-level settings. You can modify
         * version-specific settings later with <a>UpdateFunctionConfiguration</a>.
         * Function-level settings apply to both the unpublished and published versions of
         * the function, and include tags (<a>TagResource</a>) and per-function concurrency
         * limits (<a>PutFunctionConcurrency</a>).</p> <p>You can use code signing if your
         * deployment package is a .zip file archive. To enable code signing for this
         * function, specify the ARN of a code-signing configuration. When a user attempts
         * to deploy a code package with <a>UpdateFunctionCode</a>, Lambda checks that the
         * code package has a valid signature from a trusted publisher. The code-signing
         * configuration includes set of signing profiles, which define the trusted
         * publishers for this function.</p> <p>If another Amazon Web Services account or
         * an Amazon Web Services service invokes your function, use <a>AddPermission</a>
         * to grant permission by creating a resource-based Identity and Access Management
         * (IAM) policy. You can grant permissions at the function level, on a version, or
         * on an alias.</p> <p>To invoke your function directly, use <a>Invoke</a>. To
         * invoke your function in response to events in other Amazon Web Services
         * services, create an event source mapping (<a>CreateEventSourceMapping</a>), or
         * configure a function trigger in the other service. For more information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-invocation.html">Invoking
         * Lambda functions</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CreateFunction">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateFunctionOutcome CreateFunction(const Model::CreateFunctionRequest& request) const;

        /**
         * A Callable wrapper for CreateFunction that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateFunctionRequestT = Model::CreateFunctionRequest>
        Model::CreateFunctionOutcomeCallable CreateFunctionCallable(const CreateFunctionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::CreateFunction, request);
        }

        /**
         * An Async wrapper for CreateFunction that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateFunctionRequestT = Model::CreateFunctionRequest>
        void CreateFunctionAsync(const CreateFunctionRequestT& request, const CreateFunctionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::CreateFunction, request, handler, context);
        }

        /**
         * <p>Creates a Lambda function URL with the specified configuration parameters. A
         * function URL is a dedicated HTTP(S) endpoint that you can use to invoke your
         * function.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CreateFunctionUrlConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateFunctionUrlConfigOutcome CreateFunctionUrlConfig(const Model::CreateFunctionUrlConfigRequest& request) const;

        /**
         * A Callable wrapper for CreateFunctionUrlConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateFunctionUrlConfigRequestT = Model::CreateFunctionUrlConfigRequest>
        Model::CreateFunctionUrlConfigOutcomeCallable CreateFunctionUrlConfigCallable(const CreateFunctionUrlConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::CreateFunctionUrlConfig, request);
        }

        /**
         * An Async wrapper for CreateFunctionUrlConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateFunctionUrlConfigRequestT = Model::CreateFunctionUrlConfigRequest>
        void CreateFunctionUrlConfigAsync(const CreateFunctionUrlConfigRequestT& request, const CreateFunctionUrlConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::CreateFunctionUrlConfig, request, handler, context);
        }

        /**
         * <p>Deletes a Lambda function <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias</a>.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteAlias">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteAliasOutcome DeleteAlias(const Model::DeleteAliasRequest& request) const;

        /**
         * A Callable wrapper for DeleteAlias that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteAliasRequestT = Model::DeleteAliasRequest>
        Model::DeleteAliasOutcomeCallable DeleteAliasCallable(const DeleteAliasRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteAlias, request);
        }

        /**
         * An Async wrapper for DeleteAlias that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteAliasRequestT = Model::DeleteAliasRequest>
        void DeleteAliasAsync(const DeleteAliasRequestT& request, const DeleteAliasResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteAlias, request, handler, context);
        }

        /**
         * <p>Deletes the code signing configuration. You can delete the code signing
         * configuration only if no function is using it. </p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteCodeSigningConfigOutcome DeleteCodeSigningConfig(const Model::DeleteCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for DeleteCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteCodeSigningConfigRequestT = Model::DeleteCodeSigningConfigRequest>
        Model::DeleteCodeSigningConfigOutcomeCallable DeleteCodeSigningConfigCallable(const DeleteCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for DeleteCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteCodeSigningConfigRequestT = Model::DeleteCodeSigningConfigRequest>
        void DeleteCodeSigningConfigAsync(const DeleteCodeSigningConfigRequestT& request, const DeleteCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Deletes an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/intro-invocation-modes.html">event
         * source mapping</a>. You can get the identifier of a mapping from the output of
         * <a>ListEventSourceMappings</a>.</p> <p>When you delete an event source mapping,
         * it enters a <code>Deleting</code> state and might not be completely deleted for
         * several seconds.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteEventSourceMapping">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteEventSourceMappingOutcome DeleteEventSourceMapping(const Model::DeleteEventSourceMappingRequest& request) const;

        /**
         * A Callable wrapper for DeleteEventSourceMapping that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteEventSourceMappingRequestT = Model::DeleteEventSourceMappingRequest>
        Model::DeleteEventSourceMappingOutcomeCallable DeleteEventSourceMappingCallable(const DeleteEventSourceMappingRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteEventSourceMapping, request);
        }

        /**
         * An Async wrapper for DeleteEventSourceMapping that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteEventSourceMappingRequestT = Model::DeleteEventSourceMappingRequest>
        void DeleteEventSourceMappingAsync(const DeleteEventSourceMappingRequestT& request, const DeleteEventSourceMappingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteEventSourceMapping, request, handler, context);
        }

        /**
         * <p>Deletes a Lambda function. To delete a specific function version, use the
         * <code>Qualifier</code> parameter. Otherwise, all versions and aliases are
         * deleted. This doesn't require the user to have explicit permissions for
         * <a>DeleteAlias</a>.</p> <p>To delete Lambda event source mappings that invoke a
         * function, use <a>DeleteEventSourceMapping</a>. For Amazon Web Services services
         * and resources that invoke your function directly, delete the trigger in the
         * service where you originally configured it.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteFunction">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteFunctionOutcome DeleteFunction(const Model::DeleteFunctionRequest& request) const;

        /**
         * A Callable wrapper for DeleteFunction that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteFunctionRequestT = Model::DeleteFunctionRequest>
        Model::DeleteFunctionOutcomeCallable DeleteFunctionCallable(const DeleteFunctionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteFunction, request);
        }

        /**
         * An Async wrapper for DeleteFunction that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteFunctionRequestT = Model::DeleteFunctionRequest>
        void DeleteFunctionAsync(const DeleteFunctionRequestT& request, const DeleteFunctionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteFunction, request, handler, context);
        }

        /**
         * <p>Removes the code signing configuration from the function.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteFunctionCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteFunctionCodeSigningConfigOutcome DeleteFunctionCodeSigningConfig(const Model::DeleteFunctionCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for DeleteFunctionCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteFunctionCodeSigningConfigRequestT = Model::DeleteFunctionCodeSigningConfigRequest>
        Model::DeleteFunctionCodeSigningConfigOutcomeCallable DeleteFunctionCodeSigningConfigCallable(const DeleteFunctionCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteFunctionCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for DeleteFunctionCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteFunctionCodeSigningConfigRequestT = Model::DeleteFunctionCodeSigningConfigRequest>
        void DeleteFunctionCodeSigningConfigAsync(const DeleteFunctionCodeSigningConfigRequestT& request, const DeleteFunctionCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteFunctionCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Removes a concurrent execution limit from a function.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteFunctionConcurrency">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteFunctionConcurrencyOutcome DeleteFunctionConcurrency(const Model::DeleteFunctionConcurrencyRequest& request) const;

        /**
         * A Callable wrapper for DeleteFunctionConcurrency that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteFunctionConcurrencyRequestT = Model::DeleteFunctionConcurrencyRequest>
        Model::DeleteFunctionConcurrencyOutcomeCallable DeleteFunctionConcurrencyCallable(const DeleteFunctionConcurrencyRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteFunctionConcurrency, request);
        }

        /**
         * An Async wrapper for DeleteFunctionConcurrency that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteFunctionConcurrencyRequestT = Model::DeleteFunctionConcurrencyRequest>
        void DeleteFunctionConcurrencyAsync(const DeleteFunctionConcurrencyRequestT& request, const DeleteFunctionConcurrencyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteFunctionConcurrency, request, handler, context);
        }

        /**
         * <p>Deletes the configuration for asynchronous invocation for a function,
         * version, or alias.</p> <p>To configure options for asynchronous invocation, use
         * <a>PutFunctionEventInvokeConfig</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteFunctionEventInvokeConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteFunctionEventInvokeConfigOutcome DeleteFunctionEventInvokeConfig(const Model::DeleteFunctionEventInvokeConfigRequest& request) const;

        /**
         * A Callable wrapper for DeleteFunctionEventInvokeConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteFunctionEventInvokeConfigRequestT = Model::DeleteFunctionEventInvokeConfigRequest>
        Model::DeleteFunctionEventInvokeConfigOutcomeCallable DeleteFunctionEventInvokeConfigCallable(const DeleteFunctionEventInvokeConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteFunctionEventInvokeConfig, request);
        }

        /**
         * An Async wrapper for DeleteFunctionEventInvokeConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteFunctionEventInvokeConfigRequestT = Model::DeleteFunctionEventInvokeConfigRequest>
        void DeleteFunctionEventInvokeConfigAsync(const DeleteFunctionEventInvokeConfigRequestT& request, const DeleteFunctionEventInvokeConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteFunctionEventInvokeConfig, request, handler, context);
        }

        /**
         * <p>Deletes a Lambda function URL. When you delete a function URL, you can't
         * recover it. Creating a new function URL results in a different URL
         * address.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteFunctionUrlConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteFunctionUrlConfigOutcome DeleteFunctionUrlConfig(const Model::DeleteFunctionUrlConfigRequest& request) const;

        /**
         * A Callable wrapper for DeleteFunctionUrlConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteFunctionUrlConfigRequestT = Model::DeleteFunctionUrlConfigRequest>
        Model::DeleteFunctionUrlConfigOutcomeCallable DeleteFunctionUrlConfigCallable(const DeleteFunctionUrlConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteFunctionUrlConfig, request);
        }

        /**
         * An Async wrapper for DeleteFunctionUrlConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteFunctionUrlConfigRequestT = Model::DeleteFunctionUrlConfigRequest>
        void DeleteFunctionUrlConfigAsync(const DeleteFunctionUrlConfigRequestT& request, const DeleteFunctionUrlConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteFunctionUrlConfig, request, handler, context);
        }

        /**
         * <p>Deletes a version of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>. Deleted versions can no longer be viewed or added to functions. To
         * avoid breaking functions, a copy of the version remains in Lambda until no
         * functions refer to it.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteLayerVersion">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteLayerVersionOutcome DeleteLayerVersion(const Model::DeleteLayerVersionRequest& request) const;

        /**
         * A Callable wrapper for DeleteLayerVersion that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteLayerVersionRequestT = Model::DeleteLayerVersionRequest>
        Model::DeleteLayerVersionOutcomeCallable DeleteLayerVersionCallable(const DeleteLayerVersionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteLayerVersion, request);
        }

        /**
         * An Async wrapper for DeleteLayerVersion that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteLayerVersionRequestT = Model::DeleteLayerVersionRequest>
        void DeleteLayerVersionAsync(const DeleteLayerVersionRequestT& request, const DeleteLayerVersionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteLayerVersion, request, handler, context);
        }

        /**
         * <p>Deletes the provisioned concurrency configuration for a
         * function.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeleteProvisionedConcurrencyConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteProvisionedConcurrencyConfigOutcome DeleteProvisionedConcurrencyConfig(const Model::DeleteProvisionedConcurrencyConfigRequest& request) const;

        /**
         * A Callable wrapper for DeleteProvisionedConcurrencyConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteProvisionedConcurrencyConfigRequestT = Model::DeleteProvisionedConcurrencyConfigRequest>
        Model::DeleteProvisionedConcurrencyConfigOutcomeCallable DeleteProvisionedConcurrencyConfigCallable(const DeleteProvisionedConcurrencyConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::DeleteProvisionedConcurrencyConfig, request);
        }

        /**
         * An Async wrapper for DeleteProvisionedConcurrencyConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteProvisionedConcurrencyConfigRequestT = Model::DeleteProvisionedConcurrencyConfigRequest>
        void DeleteProvisionedConcurrencyConfigAsync(const DeleteProvisionedConcurrencyConfigRequestT& request, const DeleteProvisionedConcurrencyConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::DeleteProvisionedConcurrencyConfig, request, handler, context);
        }

        /**
         * <p>Retrieves details about your account's <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/limits.html">limits</a> and
         * usage in an Amazon Web Services Region.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetAccountSettings">AWS
         * API Reference</a></p>
         */
        virtual Model::GetAccountSettingsOutcome GetAccountSettings(const Model::GetAccountSettingsRequest& request = {}) const;

        /**
         * A Callable wrapper for GetAccountSettings that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetAccountSettingsRequestT = Model::GetAccountSettingsRequest>
        Model::GetAccountSettingsOutcomeCallable GetAccountSettingsCallable(const GetAccountSettingsRequestT& request = {}) const
        {
            return SubmitCallable(&LambdaClient::GetAccountSettings, request);
        }

        /**
         * An Async wrapper for GetAccountSettings that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetAccountSettingsRequestT = Model::GetAccountSettingsRequest>
        void GetAccountSettingsAsync(const GetAccountSettingsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const GetAccountSettingsRequestT& request = {}) const
        {
            return SubmitAsync(&LambdaClient::GetAccountSettings, request, handler, context);
        }

        /**
         * <p>Returns details about a Lambda function <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias</a>.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetAlias">AWS API
         * Reference</a></p>
         */
        virtual Model::GetAliasOutcome GetAlias(const Model::GetAliasRequest& request) const;

        /**
         * A Callable wrapper for GetAlias that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetAliasRequestT = Model::GetAliasRequest>
        Model::GetAliasOutcomeCallable GetAliasCallable(const GetAliasRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetAlias, request);
        }

        /**
         * An Async wrapper for GetAlias that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetAliasRequestT = Model::GetAliasRequest>
        void GetAliasAsync(const GetAliasRequestT& request, const GetAliasResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetAlias, request, handler, context);
        }

        /**
         * <p>Returns information about the specified code signing
         * configuration.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetCodeSigningConfigOutcome GetCodeSigningConfig(const Model::GetCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for GetCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetCodeSigningConfigRequestT = Model::GetCodeSigningConfigRequest>
        Model::GetCodeSigningConfigOutcomeCallable GetCodeSigningConfigCallable(const GetCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for GetCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetCodeSigningConfigRequestT = Model::GetCodeSigningConfigRequest>
        void GetCodeSigningConfigAsync(const GetCodeSigningConfigRequestT& request, const GetCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Returns details about an event source mapping. You can get the identifier of
         * a mapping from the output of <a>ListEventSourceMappings</a>.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetEventSourceMapping">AWS
         * API Reference</a></p>
         */
        virtual Model::GetEventSourceMappingOutcome GetEventSourceMapping(const Model::GetEventSourceMappingRequest& request) const;

        /**
         * A Callable wrapper for GetEventSourceMapping that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetEventSourceMappingRequestT = Model::GetEventSourceMappingRequest>
        Model::GetEventSourceMappingOutcomeCallable GetEventSourceMappingCallable(const GetEventSourceMappingRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetEventSourceMapping, request);
        }

        /**
         * An Async wrapper for GetEventSourceMapping that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetEventSourceMappingRequestT = Model::GetEventSourceMappingRequest>
        void GetEventSourceMappingAsync(const GetEventSourceMappingRequestT& request, const GetEventSourceMappingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetEventSourceMapping, request, handler, context);
        }

        /**
         * <p>Returns information about the function or function version, with a link to
         * download the deployment package that's valid for 10 minutes. If you specify a
         * function version, only details that are specific to that version are
         * returned.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunction">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionOutcome GetFunction(const Model::GetFunctionRequest& request) const;

        /**
         * A Callable wrapper for GetFunction that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionRequestT = Model::GetFunctionRequest>
        Model::GetFunctionOutcomeCallable GetFunctionCallable(const GetFunctionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunction, request);
        }

        /**
         * An Async wrapper for GetFunction that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionRequestT = Model::GetFunctionRequest>
        void GetFunctionAsync(const GetFunctionRequestT& request, const GetFunctionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunction, request, handler, context);
        }

        /**
         * <p>Returns the code signing configuration for the specified
         * function.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunctionCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionCodeSigningConfigOutcome GetFunctionCodeSigningConfig(const Model::GetFunctionCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for GetFunctionCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionCodeSigningConfigRequestT = Model::GetFunctionCodeSigningConfigRequest>
        Model::GetFunctionCodeSigningConfigOutcomeCallable GetFunctionCodeSigningConfigCallable(const GetFunctionCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunctionCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for GetFunctionCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionCodeSigningConfigRequestT = Model::GetFunctionCodeSigningConfigRequest>
        void GetFunctionCodeSigningConfigAsync(const GetFunctionCodeSigningConfigRequestT& request, const GetFunctionCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunctionCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Returns details about the reserved concurrency configuration for a function.
         * To set a concurrency limit for a function, use
         * <a>PutFunctionConcurrency</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunctionConcurrency">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionConcurrencyOutcome GetFunctionConcurrency(const Model::GetFunctionConcurrencyRequest& request) const;

        /**
         * A Callable wrapper for GetFunctionConcurrency that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionConcurrencyRequestT = Model::GetFunctionConcurrencyRequest>
        Model::GetFunctionConcurrencyOutcomeCallable GetFunctionConcurrencyCallable(const GetFunctionConcurrencyRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunctionConcurrency, request);
        }

        /**
         * An Async wrapper for GetFunctionConcurrency that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionConcurrencyRequestT = Model::GetFunctionConcurrencyRequest>
        void GetFunctionConcurrencyAsync(const GetFunctionConcurrencyRequestT& request, const GetFunctionConcurrencyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunctionConcurrency, request, handler, context);
        }

        /**
         * <p>Returns the version-specific settings of a Lambda function or version. The
         * output includes only options that can vary between versions of a function. To
         * modify these settings, use <a>UpdateFunctionConfiguration</a>.</p> <p>To get all
         * of a function's details, including function-level settings, use
         * <a>GetFunction</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunctionConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionConfigurationOutcome GetFunctionConfiguration(const Model::GetFunctionConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetFunctionConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionConfigurationRequestT = Model::GetFunctionConfigurationRequest>
        Model::GetFunctionConfigurationOutcomeCallable GetFunctionConfigurationCallable(const GetFunctionConfigurationRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunctionConfiguration, request);
        }

        /**
         * An Async wrapper for GetFunctionConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionConfigurationRequestT = Model::GetFunctionConfigurationRequest>
        void GetFunctionConfigurationAsync(const GetFunctionConfigurationRequestT& request, const GetFunctionConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunctionConfiguration, request, handler, context);
        }

        /**
         * <p>Retrieves the configuration for asynchronous invocation for a function,
         * version, or alias.</p> <p>To configure options for asynchronous invocation, use
         * <a>PutFunctionEventInvokeConfig</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunctionEventInvokeConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionEventInvokeConfigOutcome GetFunctionEventInvokeConfig(const Model::GetFunctionEventInvokeConfigRequest& request) const;

        /**
         * A Callable wrapper for GetFunctionEventInvokeConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionEventInvokeConfigRequestT = Model::GetFunctionEventInvokeConfigRequest>
        Model::GetFunctionEventInvokeConfigOutcomeCallable GetFunctionEventInvokeConfigCallable(const GetFunctionEventInvokeConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunctionEventInvokeConfig, request);
        }

        /**
         * An Async wrapper for GetFunctionEventInvokeConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionEventInvokeConfigRequestT = Model::GetFunctionEventInvokeConfigRequest>
        void GetFunctionEventInvokeConfigAsync(const GetFunctionEventInvokeConfigRequestT& request, const GetFunctionEventInvokeConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunctionEventInvokeConfig, request, handler, context);
        }

        /**
         * <p>Returns your function's <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-recursion.html">recursive
         * loop detection</a> configuration. </p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunctionRecursionConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionRecursionConfigOutcome GetFunctionRecursionConfig(const Model::GetFunctionRecursionConfigRequest& request) const;

        /**
         * A Callable wrapper for GetFunctionRecursionConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionRecursionConfigRequestT = Model::GetFunctionRecursionConfigRequest>
        Model::GetFunctionRecursionConfigOutcomeCallable GetFunctionRecursionConfigCallable(const GetFunctionRecursionConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunctionRecursionConfig, request);
        }

        /**
         * An Async wrapper for GetFunctionRecursionConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionRecursionConfigRequestT = Model::GetFunctionRecursionConfigRequest>
        void GetFunctionRecursionConfigAsync(const GetFunctionRecursionConfigRequestT& request, const GetFunctionRecursionConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunctionRecursionConfig, request, handler, context);
        }

        /**
         * <p>Returns details about a Lambda function URL.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetFunctionUrlConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetFunctionUrlConfigOutcome GetFunctionUrlConfig(const Model::GetFunctionUrlConfigRequest& request) const;

        /**
         * A Callable wrapper for GetFunctionUrlConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetFunctionUrlConfigRequestT = Model::GetFunctionUrlConfigRequest>
        Model::GetFunctionUrlConfigOutcomeCallable GetFunctionUrlConfigCallable(const GetFunctionUrlConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetFunctionUrlConfig, request);
        }

        /**
         * An Async wrapper for GetFunctionUrlConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetFunctionUrlConfigRequestT = Model::GetFunctionUrlConfigRequest>
        void GetFunctionUrlConfigAsync(const GetFunctionUrlConfigRequestT& request, const GetFunctionUrlConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetFunctionUrlConfig, request, handler, context);
        }

        /**
         * <p>Returns information about a version of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>, with a link to download the layer archive that's valid for 10
         * minutes.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetLayerVersion">AWS
         * API Reference</a></p>
         */
        virtual Model::GetLayerVersionOutcome GetLayerVersion(const Model::GetLayerVersionRequest& request) const;

        /**
         * A Callable wrapper for GetLayerVersion that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetLayerVersionRequestT = Model::GetLayerVersionRequest>
        Model::GetLayerVersionOutcomeCallable GetLayerVersionCallable(const GetLayerVersionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetLayerVersion, request);
        }

        /**
         * An Async wrapper for GetLayerVersion that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetLayerVersionRequestT = Model::GetLayerVersionRequest>
        void GetLayerVersionAsync(const GetLayerVersionRequestT& request, const GetLayerVersionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetLayerVersion, request, handler, context);
        }

        /**
         * <p>Returns information about a version of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>, with a link to download the layer archive that's valid for 10
         * minutes.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetLayerVersionByArn">AWS
         * API Reference</a></p>
         */
        virtual Model::GetLayerVersionByArnOutcome GetLayerVersionByArn(const Model::GetLayerVersionByArnRequest& request) const;

        /**
         * A Callable wrapper for GetLayerVersionByArn that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetLayerVersionByArnRequestT = Model::GetLayerVersionByArnRequest>
        Model::GetLayerVersionByArnOutcomeCallable GetLayerVersionByArnCallable(const GetLayerVersionByArnRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetLayerVersionByArn, request);
        }

        /**
         * An Async wrapper for GetLayerVersionByArn that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetLayerVersionByArnRequestT = Model::GetLayerVersionByArnRequest>
        void GetLayerVersionByArnAsync(const GetLayerVersionByArnRequestT& request, const GetLayerVersionByArnResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetLayerVersionByArn, request, handler, context);
        }

        /**
         * <p>Returns the permission policy for a version of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>. For more information, see
         * <a>AddLayerVersionPermission</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetLayerVersionPolicy">AWS
         * API Reference</a></p>
         */
        virtual Model::GetLayerVersionPolicyOutcome GetLayerVersionPolicy(const Model::GetLayerVersionPolicyRequest& request) const;

        /**
         * A Callable wrapper for GetLayerVersionPolicy that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetLayerVersionPolicyRequestT = Model::GetLayerVersionPolicyRequest>
        Model::GetLayerVersionPolicyOutcomeCallable GetLayerVersionPolicyCallable(const GetLayerVersionPolicyRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetLayerVersionPolicy, request);
        }

        /**
         * An Async wrapper for GetLayerVersionPolicy that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetLayerVersionPolicyRequestT = Model::GetLayerVersionPolicyRequest>
        void GetLayerVersionPolicyAsync(const GetLayerVersionPolicyRequestT& request, const GetLayerVersionPolicyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetLayerVersionPolicy, request, handler, context);
        }

        /**
         * <p>Returns the <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/access-control-resource-based.html">resource-based
         * IAM policy</a> for a function, version, or alias.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetPolicy">AWS
         * API Reference</a></p>
         */
        virtual Model::GetPolicyOutcome GetPolicy(const Model::GetPolicyRequest& request) const;

        /**
         * A Callable wrapper for GetPolicy that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetPolicyRequestT = Model::GetPolicyRequest>
        Model::GetPolicyOutcomeCallable GetPolicyCallable(const GetPolicyRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetPolicy, request);
        }

        /**
         * An Async wrapper for GetPolicy that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetPolicyRequestT = Model::GetPolicyRequest>
        void GetPolicyAsync(const GetPolicyRequestT& request, const GetPolicyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetPolicy, request, handler, context);
        }

        /**
         * <p>Retrieves the provisioned concurrency configuration for a function's alias or
         * version.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetProvisionedConcurrencyConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetProvisionedConcurrencyConfigOutcome GetProvisionedConcurrencyConfig(const Model::GetProvisionedConcurrencyConfigRequest& request) const;

        /**
         * A Callable wrapper for GetProvisionedConcurrencyConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetProvisionedConcurrencyConfigRequestT = Model::GetProvisionedConcurrencyConfigRequest>
        Model::GetProvisionedConcurrencyConfigOutcomeCallable GetProvisionedConcurrencyConfigCallable(const GetProvisionedConcurrencyConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetProvisionedConcurrencyConfig, request);
        }

        /**
         * An Async wrapper for GetProvisionedConcurrencyConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetProvisionedConcurrencyConfigRequestT = Model::GetProvisionedConcurrencyConfigRequest>
        void GetProvisionedConcurrencyConfigAsync(const GetProvisionedConcurrencyConfigRequestT& request, const GetProvisionedConcurrencyConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetProvisionedConcurrencyConfig, request, handler, context);
        }

        /**
         * <p>Retrieves the runtime management configuration for a function's version. If
         * the runtime update mode is <b>Manual</b>, this includes the ARN of the runtime
         * version and the runtime update mode. If the runtime update mode is <b>Auto</b>
         * or <b>Function update</b>, this includes the runtime update mode and
         * <code>null</code> is returned for the ARN. For more information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/runtimes-update.html">Runtime
         * updates</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/GetRuntimeManagementConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::GetRuntimeManagementConfigOutcome GetRuntimeManagementConfig(const Model::GetRuntimeManagementConfigRequest& request) const;

        /**
         * A Callable wrapper for GetRuntimeManagementConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetRuntimeManagementConfigRequestT = Model::GetRuntimeManagementConfigRequest>
        Model::GetRuntimeManagementConfigOutcomeCallable GetRuntimeManagementConfigCallable(const GetRuntimeManagementConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::GetRuntimeManagementConfig, request);
        }

        /**
         * An Async wrapper for GetRuntimeManagementConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetRuntimeManagementConfigRequestT = Model::GetRuntimeManagementConfigRequest>
        void GetRuntimeManagementConfigAsync(const GetRuntimeManagementConfigRequestT& request, const GetRuntimeManagementConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::GetRuntimeManagementConfig, request, handler, context);
        }

        /**
         * <p>Invokes a Lambda function. You can invoke a function synchronously (and wait
         * for the response), or asynchronously. By default, Lambda invokes your function
         * synchronously (i.e. the<code>InvocationType</code> is
         * <code>RequestResponse</code>). To invoke a function asynchronously, set
         * <code>InvocationType</code> to <code>Event</code>. Lambda passes the
         * <code>ClientContext</code> object to your function for synchronous invocations
         * only.</p> <p>For <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-sync.html">synchronous
         * invocation</a>, details about the function response, including errors, are
         * included in the response body and headers. For either invocation type, you can
         * find more information in the <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/monitoring-functions.html">execution
         * log</a> and <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-x-ray.html">trace</a>.</p>
         * <p>When an error occurs, your function may be invoked multiple times. Retry
         * behavior varies by error type, client, event source, and invocation type. For
         * example, if you invoke a function asynchronously and it returns an error, Lambda
         * executes the function up to two more times. For more information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-retries.html">Error
         * handling and automatic retries in Lambda</a>.</p> <p>For <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html">asynchronous
         * invocation</a>, Lambda adds events to a queue before sending them to your
         * function. If your function does not have enough capacity to keep up with the
         * queue, events may be lost. Occasionally, your function may receive the same
         * event multiple times, even if no error occurs. To retain events that were not
         * processed, configure your function with a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html#invocation-dlq">dead-letter
         * queue</a>.</p> <p>The status code in the API response doesn't reflect function
         * errors. Error codes are reserved for errors that prevent your function from
         * executing, such as permissions errors, <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/gettingstarted-limits.html">quota</a>
         * errors, or issues with your function's code and configuration. For example,
         * Lambda returns <code>TooManyRequestsException</code> if running the function
         * would cause you to exceed a concurrency limit at either the account level
         * (<code>ConcurrentInvocationLimitExceeded</code>) or function level
         * (<code>ReservedFunctionConcurrentInvocationLimitExceeded</code>).</p> <p>For
         * functions with a long timeout, your client might disconnect during synchronous
         * invocation while it waits for a response. Configure your HTTP client, SDK,
         * firewall, proxy, or operating system to allow for long connections with timeout
         * or keep-alive settings.</p> <p>This operation requires permission for the <a
         * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/list_awslambda.html">lambda:InvokeFunction</a>
         * action. For details on how to set up permissions for cross-account invocations,
         * see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/access-control-resource-based.html#permissions-resource-xaccountinvoke">Granting
         * function access to other accounts</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/Invoke">AWS API
         * Reference</a></p>
         */
        virtual Model::InvokeOutcome Invoke(const Model::InvokeRequest& request) const;

        /**
         * A Callable wrapper for Invoke that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename InvokeRequestT = Model::InvokeRequest>
        Model::InvokeOutcomeCallable InvokeCallable(const InvokeRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::Invoke, request);
        }

        /**
         * An Async wrapper for Invoke that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename InvokeRequestT = Model::InvokeRequest>
        void InvokeAsync(const InvokeRequestT& request, const InvokeResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::Invoke, request, handler, context);
        }

        /**
         * <p>Configure your Lambda functions to stream response payloads back to clients.
         * For more information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-response-streaming.html">Configuring
         * a Lambda function to stream responses</a>.</p> <p>This operation requires
         * permission for the <a
         * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/list_awslambda.html">lambda:InvokeFunction</a>
         * action. For details on how to set up permissions for cross-account invocations,
         * see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/access-control-resource-based.html#permissions-resource-xaccountinvoke">Granting
         * function access to other accounts</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/InvokeWithResponseStream">AWS
         * API Reference</a></p>
         */
        virtual Model::InvokeWithResponseStreamOutcome InvokeWithResponseStream(Model::InvokeWithResponseStreamRequest& request) const;

        /**
         * A Callable wrapper for InvokeWithResponseStream that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename InvokeWithResponseStreamRequestT = Model::InvokeWithResponseStreamRequest>
        Model::InvokeWithResponseStreamOutcomeCallable InvokeWithResponseStreamCallable(InvokeWithResponseStreamRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::InvokeWithResponseStream, request);
        }

        /**
         * An Async wrapper for InvokeWithResponseStream that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename InvokeWithResponseStreamRequestT = Model::InvokeWithResponseStreamRequest>
        void InvokeWithResponseStreamAsync(InvokeWithResponseStreamRequestT& request, const InvokeWithResponseStreamResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::InvokeWithResponseStream, request, handler, context);
        }

        /**
         * <p>Returns a list of <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">aliases</a>
         * for a Lambda function.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListAliases">AWS
         * API Reference</a></p>
         */
        virtual Model::ListAliasesOutcome ListAliases(const Model::ListAliasesRequest& request) const;

        /**
         * A Callable wrapper for ListAliases that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListAliasesRequestT = Model::ListAliasesRequest>
        Model::ListAliasesOutcomeCallable ListAliasesCallable(const ListAliasesRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListAliases, request);
        }

        /**
         * An Async wrapper for ListAliases that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListAliasesRequestT = Model::ListAliasesRequest>
        void ListAliasesAsync(const ListAliasesRequestT& request, const ListAliasesResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListAliases, request, handler, context);
        }

        /**
         * <p>Returns a list of <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuring-codesigning.html">code
         * signing configurations</a>. A request returns up to 10,000 configurations per
         * call. You can use the <code>MaxItems</code> parameter to return fewer
         * configurations per call. </p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListCodeSigningConfigs">AWS
         * API Reference</a></p>
         */
        virtual Model::ListCodeSigningConfigsOutcome ListCodeSigningConfigs(const Model::ListCodeSigningConfigsRequest& request = {}) const;

        /**
         * A Callable wrapper for ListCodeSigningConfigs that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListCodeSigningConfigsRequestT = Model::ListCodeSigningConfigsRequest>
        Model::ListCodeSigningConfigsOutcomeCallable ListCodeSigningConfigsCallable(const ListCodeSigningConfigsRequestT& request = {}) const
        {
            return SubmitCallable(&LambdaClient::ListCodeSigningConfigs, request);
        }

        /**
         * An Async wrapper for ListCodeSigningConfigs that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListCodeSigningConfigsRequestT = Model::ListCodeSigningConfigsRequest>
        void ListCodeSigningConfigsAsync(const ListCodeSigningConfigsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const ListCodeSigningConfigsRequestT& request = {}) const
        {
            return SubmitAsync(&LambdaClient::ListCodeSigningConfigs, request, handler, context);
        }

        /**
         * <p>Lists event source mappings. Specify an <code>EventSourceArn</code> to show
         * only event source mappings for a single event source.</p><p><h3>See Also:</h3>  
         * <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListEventSourceMappings">AWS
         * API Reference</a></p>
         */
        virtual Model::ListEventSourceMappingsOutcome ListEventSourceMappings(const Model::ListEventSourceMappingsRequest& request = {}) const;

        /**
         * A Callable wrapper for ListEventSourceMappings that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListEventSourceMappingsRequestT = Model::ListEventSourceMappingsRequest>
        Model::ListEventSourceMappingsOutcomeCallable ListEventSourceMappingsCallable(const ListEventSourceMappingsRequestT& request = {}) const
        {
            return SubmitCallable(&LambdaClient::ListEventSourceMappings, request);
        }

        /**
         * An Async wrapper for ListEventSourceMappings that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListEventSourceMappingsRequestT = Model::ListEventSourceMappingsRequest>
        void ListEventSourceMappingsAsync(const ListEventSourceMappingsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const ListEventSourceMappingsRequestT& request = {}) const
        {
            return SubmitAsync(&LambdaClient::ListEventSourceMappings, request, handler, context);
        }

        /**
         * <p>Retrieves a list of configurations for asynchronous invocation for a
         * function.</p> <p>To configure options for asynchronous invocation, use
         * <a>PutFunctionEventInvokeConfig</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListFunctionEventInvokeConfigs">AWS
         * API Reference</a></p>
         */
        virtual Model::ListFunctionEventInvokeConfigsOutcome ListFunctionEventInvokeConfigs(const Model::ListFunctionEventInvokeConfigsRequest& request) const;

        /**
         * A Callable wrapper for ListFunctionEventInvokeConfigs that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListFunctionEventInvokeConfigsRequestT = Model::ListFunctionEventInvokeConfigsRequest>
        Model::ListFunctionEventInvokeConfigsOutcomeCallable ListFunctionEventInvokeConfigsCallable(const ListFunctionEventInvokeConfigsRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListFunctionEventInvokeConfigs, request);
        }

        /**
         * An Async wrapper for ListFunctionEventInvokeConfigs that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListFunctionEventInvokeConfigsRequestT = Model::ListFunctionEventInvokeConfigsRequest>
        void ListFunctionEventInvokeConfigsAsync(const ListFunctionEventInvokeConfigsRequestT& request, const ListFunctionEventInvokeConfigsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListFunctionEventInvokeConfigs, request, handler, context);
        }

        /**
         * <p>Returns a list of Lambda function URLs for the specified
         * function.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListFunctionUrlConfigs">AWS
         * API Reference</a></p>
         */
        virtual Model::ListFunctionUrlConfigsOutcome ListFunctionUrlConfigs(const Model::ListFunctionUrlConfigsRequest& request) const;

        /**
         * A Callable wrapper for ListFunctionUrlConfigs that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListFunctionUrlConfigsRequestT = Model::ListFunctionUrlConfigsRequest>
        Model::ListFunctionUrlConfigsOutcomeCallable ListFunctionUrlConfigsCallable(const ListFunctionUrlConfigsRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListFunctionUrlConfigs, request);
        }

        /**
         * An Async wrapper for ListFunctionUrlConfigs that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListFunctionUrlConfigsRequestT = Model::ListFunctionUrlConfigsRequest>
        void ListFunctionUrlConfigsAsync(const ListFunctionUrlConfigsRequestT& request, const ListFunctionUrlConfigsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListFunctionUrlConfigs, request, handler, context);
        }

        /**
         * <p>Returns a list of Lambda functions, with the version-specific configuration
         * of each. Lambda returns up to 50 functions per call.</p> <p>Set
         * <code>FunctionVersion</code> to <code>ALL</code> to include all published
         * versions of each function in addition to the unpublished version.</p> 
         * <p>The <code>ListFunctions</code> operation returns a subset of the
         * <a>FunctionConfiguration</a> fields. To get the additional fields (State,
         * StateReasonCode, StateReason, LastUpdateStatus, LastUpdateStatusReason,
         * LastUpdateStatusReasonCode, RuntimeVersionConfig) for a function or version, use
         * <a>GetFunction</a>.</p> <p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListFunctions">AWS
         * API Reference</a></p>
         */
        virtual Model::ListFunctionsOutcome ListFunctions(const Model::ListFunctionsRequest& request = {}) const;

        /**
         * A Callable wrapper for ListFunctions that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListFunctionsRequestT = Model::ListFunctionsRequest>
        Model::ListFunctionsOutcomeCallable ListFunctionsCallable(const ListFunctionsRequestT& request = {}) const
        {
            return SubmitCallable(&LambdaClient::ListFunctions, request);
        }

        /**
         * An Async wrapper for ListFunctions that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListFunctionsRequestT = Model::ListFunctionsRequest>
        void ListFunctionsAsync(const ListFunctionsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const ListFunctionsRequestT& request = {}) const
        {
            return SubmitAsync(&LambdaClient::ListFunctions, request, handler, context);
        }

        /**
         * <p>List the functions that use the specified code signing configuration. You can
         * use this method prior to deleting a code signing configuration, to verify that
         * no functions are using it.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListFunctionsByCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::ListFunctionsByCodeSigningConfigOutcome ListFunctionsByCodeSigningConfig(const Model::ListFunctionsByCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for ListFunctionsByCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListFunctionsByCodeSigningConfigRequestT = Model::ListFunctionsByCodeSigningConfigRequest>
        Model::ListFunctionsByCodeSigningConfigOutcomeCallable ListFunctionsByCodeSigningConfigCallable(const ListFunctionsByCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListFunctionsByCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for ListFunctionsByCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListFunctionsByCodeSigningConfigRequestT = Model::ListFunctionsByCodeSigningConfigRequest>
        void ListFunctionsByCodeSigningConfigAsync(const ListFunctionsByCodeSigningConfigRequestT& request, const ListFunctionsByCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListFunctionsByCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Lists the versions of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>. Versions that have been deleted aren't listed. Specify a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html">runtime
         * identifier</a> to list only versions that indicate that they're compatible with
         * that runtime. Specify a compatible architecture to include only layer versions
         * that are compatible with that architecture.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListLayerVersions">AWS
         * API Reference</a></p>
         */
        virtual Model::ListLayerVersionsOutcome ListLayerVersions(const Model::ListLayerVersionsRequest& request) const;

        /**
         * A Callable wrapper for ListLayerVersions that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListLayerVersionsRequestT = Model::ListLayerVersionsRequest>
        Model::ListLayerVersionsOutcomeCallable ListLayerVersionsCallable(const ListLayerVersionsRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListLayerVersions, request);
        }

        /**
         * An Async wrapper for ListLayerVersions that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListLayerVersionsRequestT = Model::ListLayerVersionsRequest>
        void ListLayerVersionsAsync(const ListLayerVersionsRequestT& request, const ListLayerVersionsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListLayerVersions, request, handler, context);
        }

        /**
         * <p>Lists <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-layers.html">Lambda
         * layers</a> and shows information about the latest version of each. Specify a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-runtimes.html">runtime
         * identifier</a> to list only layers that indicate that they're compatible with
         * that runtime. Specify a compatible architecture to include only layers that are
         * compatible with that <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/foundation-arch.html">instruction
         * set architecture</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListLayers">AWS
         * API Reference</a></p>
         */
        virtual Model::ListLayersOutcome ListLayers(const Model::ListLayersRequest& request = {}) const;

        /**
         * A Callable wrapper for ListLayers that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListLayersRequestT = Model::ListLayersRequest>
        Model::ListLayersOutcomeCallable ListLayersCallable(const ListLayersRequestT& request = {}) const
        {
            return SubmitCallable(&LambdaClient::ListLayers, request);
        }

        /**
         * An Async wrapper for ListLayers that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListLayersRequestT = Model::ListLayersRequest>
        void ListLayersAsync(const ListLayersResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const ListLayersRequestT& request = {}) const
        {
            return SubmitAsync(&LambdaClient::ListLayers, request, handler, context);
        }

        /**
         * <p>Retrieves a list of provisioned concurrency configurations for a
         * function.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListProvisionedConcurrencyConfigs">AWS
         * API Reference</a></p>
         */
        virtual Model::ListProvisionedConcurrencyConfigsOutcome ListProvisionedConcurrencyConfigs(const Model::ListProvisionedConcurrencyConfigsRequest& request) const;

        /**
         * A Callable wrapper for ListProvisionedConcurrencyConfigs that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListProvisionedConcurrencyConfigsRequestT = Model::ListProvisionedConcurrencyConfigsRequest>
        Model::ListProvisionedConcurrencyConfigsOutcomeCallable ListProvisionedConcurrencyConfigsCallable(const ListProvisionedConcurrencyConfigsRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListProvisionedConcurrencyConfigs, request);
        }

        /**
         * An Async wrapper for ListProvisionedConcurrencyConfigs that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListProvisionedConcurrencyConfigsRequestT = Model::ListProvisionedConcurrencyConfigsRequest>
        void ListProvisionedConcurrencyConfigsAsync(const ListProvisionedConcurrencyConfigsRequestT& request, const ListProvisionedConcurrencyConfigsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListProvisionedConcurrencyConfigs, request, handler, context);
        }

        /**
         * <p>Returns a function, event source mapping, or code signing configuration's <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/tagging.html">tags</a>. You
         * can also view function tags with <a>GetFunction</a>.</p><p><h3>See Also:</h3>  
         * <a href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListTags">AWS
         * API Reference</a></p>
         */
        virtual Model::ListTagsOutcome ListTags(const Model::ListTagsRequest& request) const;

        /**
         * A Callable wrapper for ListTags that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListTagsRequestT = Model::ListTagsRequest>
        Model::ListTagsOutcomeCallable ListTagsCallable(const ListTagsRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListTags, request);
        }

        /**
         * An Async wrapper for ListTags that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListTagsRequestT = Model::ListTagsRequest>
        void ListTagsAsync(const ListTagsRequestT& request, const ListTagsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListTags, request, handler, context);
        }

        /**
         * <p>Returns a list of <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/versioning-aliases.html">versions</a>,
         * with the version-specific configuration of each. Lambda returns up to 50
         * versions per call.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListVersionsByFunction">AWS
         * API Reference</a></p>
         */
        virtual Model::ListVersionsByFunctionOutcome ListVersionsByFunction(const Model::ListVersionsByFunctionRequest& request) const;

        /**
         * A Callable wrapper for ListVersionsByFunction that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListVersionsByFunctionRequestT = Model::ListVersionsByFunctionRequest>
        Model::ListVersionsByFunctionOutcomeCallable ListVersionsByFunctionCallable(const ListVersionsByFunctionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::ListVersionsByFunction, request);
        }

        /**
         * An Async wrapper for ListVersionsByFunction that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListVersionsByFunctionRequestT = Model::ListVersionsByFunctionRequest>
        void ListVersionsByFunctionAsync(const ListVersionsByFunctionRequestT& request, const ListVersionsByFunctionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::ListVersionsByFunction, request, handler, context);
        }

        /**
         * <p>Creates an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a> from a ZIP archive. Each time you call
         * <code>PublishLayerVersion</code> with the same layer name, a new version is
         * created.</p> <p>Add layers to your function with <a>CreateFunction</a> or
         * <a>UpdateFunctionConfiguration</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PublishLayerVersion">AWS
         * API Reference</a></p>
         */
        virtual Model::PublishLayerVersionOutcome PublishLayerVersion(const Model::PublishLayerVersionRequest& request) const;

        /**
         * A Callable wrapper for PublishLayerVersion that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PublishLayerVersionRequestT = Model::PublishLayerVersionRequest>
        Model::PublishLayerVersionOutcomeCallable PublishLayerVersionCallable(const PublishLayerVersionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PublishLayerVersion, request);
        }

        /**
         * An Async wrapper for PublishLayerVersion that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PublishLayerVersionRequestT = Model::PublishLayerVersionRequest>
        void PublishLayerVersionAsync(const PublishLayerVersionRequestT& request, const PublishLayerVersionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PublishLayerVersion, request, handler, context);
        }

        /**
         * <p>Creates a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/versioning-aliases.html">version</a>
         * from the current code and configuration of a function. Use versions to create a
         * snapshot of your function code and configuration that doesn't change.</p>
         * <p>Lambda doesn't publish a version if the function's configuration and code
         * haven't changed since the last version. Use <a>UpdateFunctionCode</a> or
         * <a>UpdateFunctionConfiguration</a> to update the function before publishing a
         * version.</p> <p>Clients can invoke versions directly or with an alias. To create
         * an alias, use <a>CreateAlias</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PublishVersion">AWS
         * API Reference</a></p>
         */
        virtual Model::PublishVersionOutcome PublishVersion(const Model::PublishVersionRequest& request) const;

        /**
         * A Callable wrapper for PublishVersion that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PublishVersionRequestT = Model::PublishVersionRequest>
        Model::PublishVersionOutcomeCallable PublishVersionCallable(const PublishVersionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PublishVersion, request);
        }

        /**
         * An Async wrapper for PublishVersion that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PublishVersionRequestT = Model::PublishVersionRequest>
        void PublishVersionAsync(const PublishVersionRequestT& request, const PublishVersionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PublishVersion, request, handler, context);
        }

        /**
         * <p>Update the code signing configuration for the function. Changes to the code
         * signing configuration take effect the next time a user tries to deploy a code
         * package to the function. </p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PutFunctionCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::PutFunctionCodeSigningConfigOutcome PutFunctionCodeSigningConfig(const Model::PutFunctionCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for PutFunctionCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutFunctionCodeSigningConfigRequestT = Model::PutFunctionCodeSigningConfigRequest>
        Model::PutFunctionCodeSigningConfigOutcomeCallable PutFunctionCodeSigningConfigCallable(const PutFunctionCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PutFunctionCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for PutFunctionCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutFunctionCodeSigningConfigRequestT = Model::PutFunctionCodeSigningConfigRequest>
        void PutFunctionCodeSigningConfigAsync(const PutFunctionCodeSigningConfigRequestT& request, const PutFunctionCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PutFunctionCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Sets the maximum number of simultaneous executions for a function, and
         * reserves capacity for that concurrency level.</p> <p>Concurrency settings apply
         * to the function as a whole, including all published versions and the unpublished
         * version. Reserving concurrency both ensures that your function has capacity to
         * process the specified number of events simultaneously, and prevents it from
         * scaling beyond that level. Use <a>GetFunction</a> to see the current setting for
         * a function.</p> <p>Use <a>GetAccountSettings</a> to see your Regional
         * concurrency limit. You can reserve concurrency for as many functions as you
         * like, as long as you leave at least 100 simultaneous executions unreserved for
         * functions that aren't configured with a per-function limit. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-scaling.html">Lambda
         * function scaling</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PutFunctionConcurrency">AWS
         * API Reference</a></p>
         */
        virtual Model::PutFunctionConcurrencyOutcome PutFunctionConcurrency(const Model::PutFunctionConcurrencyRequest& request) const;

        /**
         * A Callable wrapper for PutFunctionConcurrency that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutFunctionConcurrencyRequestT = Model::PutFunctionConcurrencyRequest>
        Model::PutFunctionConcurrencyOutcomeCallable PutFunctionConcurrencyCallable(const PutFunctionConcurrencyRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PutFunctionConcurrency, request);
        }

        /**
         * An Async wrapper for PutFunctionConcurrency that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutFunctionConcurrencyRequestT = Model::PutFunctionConcurrencyRequest>
        void PutFunctionConcurrencyAsync(const PutFunctionConcurrencyRequestT& request, const PutFunctionConcurrencyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PutFunctionConcurrency, request, handler, context);
        }

        /**
         * <p>Configures options for <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html">asynchronous
         * invocation</a> on a function, version, or alias. If a configuration already
         * exists for a function, version, or alias, this operation overwrites it. If you
         * exclude any settings, they are removed. To set one option without affecting
         * existing settings for other options, use
         * <a>UpdateFunctionEventInvokeConfig</a>.</p> <p>By default, Lambda retries an
         * asynchronous invocation twice if the function returns an error. It retains
         * events in a queue for up to six hours. When an event fails all processing
         * attempts or stays in the asynchronous invocation queue for too long, Lambda
         * discards it. To retain discarded events, configure a dead-letter queue with
         * <a>UpdateFunctionConfiguration</a>.</p> <p>To send an invocation record to a
         * queue, topic, S3 bucket, function, or event bus, specify a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html#invocation-async-destinations">destination</a>.
         * You can configure separate destinations for successful invocations (on-success)
         * and events that fail all processing attempts (on-failure). You can configure
         * destinations in addition to or instead of a dead-letter queue.</p>  <p>S3
         * buckets are supported only for on-failure destinations. To retain records of
         * successful invocations, use another destination type.</p> <p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PutFunctionEventInvokeConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::PutFunctionEventInvokeConfigOutcome PutFunctionEventInvokeConfig(const Model::PutFunctionEventInvokeConfigRequest& request) const;

        /**
         * A Callable wrapper for PutFunctionEventInvokeConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutFunctionEventInvokeConfigRequestT = Model::PutFunctionEventInvokeConfigRequest>
        Model::PutFunctionEventInvokeConfigOutcomeCallable PutFunctionEventInvokeConfigCallable(const PutFunctionEventInvokeConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PutFunctionEventInvokeConfig, request);
        }

        /**
         * An Async wrapper for PutFunctionEventInvokeConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutFunctionEventInvokeConfigRequestT = Model::PutFunctionEventInvokeConfigRequest>
        void PutFunctionEventInvokeConfigAsync(const PutFunctionEventInvokeConfigRequestT& request, const PutFunctionEventInvokeConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PutFunctionEventInvokeConfig, request, handler, context);
        }

        /**
         * <p>Sets your function's <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-recursion.html">recursive
         * loop detection</a> configuration.</p> <p>When you configure a Lambda function to
         * output to the same service or resource that invokes the function, it's possible
         * to create an infinite recursive loop. For example, a Lambda function might write
         * a message to an Amazon Simple Queue Service (Amazon SQS) queue, which then
         * invokes the same function. This invocation causes the function to write another
         * message to the queue, which in turn invokes the function again.</p> <p>Lambda
         * can detect certain types of recursive loops shortly after they occur. When
         * Lambda detects a recursive loop and your function's recursive loop detection
         * configuration is set to <code>Terminate</code>, it stops your function being
         * invoked and notifies you.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PutFunctionRecursionConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::PutFunctionRecursionConfigOutcome PutFunctionRecursionConfig(const Model::PutFunctionRecursionConfigRequest& request) const;

        /**
         * A Callable wrapper for PutFunctionRecursionConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutFunctionRecursionConfigRequestT = Model::PutFunctionRecursionConfigRequest>
        Model::PutFunctionRecursionConfigOutcomeCallable PutFunctionRecursionConfigCallable(const PutFunctionRecursionConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PutFunctionRecursionConfig, request);
        }

        /**
         * An Async wrapper for PutFunctionRecursionConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutFunctionRecursionConfigRequestT = Model::PutFunctionRecursionConfigRequest>
        void PutFunctionRecursionConfigAsync(const PutFunctionRecursionConfigRequestT& request, const PutFunctionRecursionConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PutFunctionRecursionConfig, request, handler, context);
        }

        /**
         * <p>Adds a provisioned concurrency configuration to a function's alias or
         * version.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PutProvisionedConcurrencyConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::PutProvisionedConcurrencyConfigOutcome PutProvisionedConcurrencyConfig(const Model::PutProvisionedConcurrencyConfigRequest& request) const;

        /**
         * A Callable wrapper for PutProvisionedConcurrencyConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutProvisionedConcurrencyConfigRequestT = Model::PutProvisionedConcurrencyConfigRequest>
        Model::PutProvisionedConcurrencyConfigOutcomeCallable PutProvisionedConcurrencyConfigCallable(const PutProvisionedConcurrencyConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PutProvisionedConcurrencyConfig, request);
        }

        /**
         * An Async wrapper for PutProvisionedConcurrencyConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutProvisionedConcurrencyConfigRequestT = Model::PutProvisionedConcurrencyConfigRequest>
        void PutProvisionedConcurrencyConfigAsync(const PutProvisionedConcurrencyConfigRequestT& request, const PutProvisionedConcurrencyConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PutProvisionedConcurrencyConfig, request, handler, context);
        }

        /**
         * <p>Sets the runtime management configuration for a function's version. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/runtimes-update.html">Runtime
         * updates</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/PutRuntimeManagementConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::PutRuntimeManagementConfigOutcome PutRuntimeManagementConfig(const Model::PutRuntimeManagementConfigRequest& request) const;

        /**
         * A Callable wrapper for PutRuntimeManagementConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutRuntimeManagementConfigRequestT = Model::PutRuntimeManagementConfigRequest>
        Model::PutRuntimeManagementConfigOutcomeCallable PutRuntimeManagementConfigCallable(const PutRuntimeManagementConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::PutRuntimeManagementConfig, request);
        }

        /**
         * An Async wrapper for PutRuntimeManagementConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutRuntimeManagementConfigRequestT = Model::PutRuntimeManagementConfigRequest>
        void PutRuntimeManagementConfigAsync(const PutRuntimeManagementConfigRequestT& request, const PutRuntimeManagementConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::PutRuntimeManagementConfig, request, handler, context);
        }

        /**
         * <p>Removes a statement from the permissions policy for a version of an <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
         * layer</a>. For more information, see
         * <a>AddLayerVersionPermission</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/RemoveLayerVersionPermission">AWS
         * API Reference</a></p>
         */
        virtual Model::RemoveLayerVersionPermissionOutcome RemoveLayerVersionPermission(const Model::RemoveLayerVersionPermissionRequest& request) const;

        /**
         * A Callable wrapper for RemoveLayerVersionPermission that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename RemoveLayerVersionPermissionRequestT = Model::RemoveLayerVersionPermissionRequest>
        Model::RemoveLayerVersionPermissionOutcomeCallable RemoveLayerVersionPermissionCallable(const RemoveLayerVersionPermissionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::RemoveLayerVersionPermission, request);
        }

        /**
         * An Async wrapper for RemoveLayerVersionPermission that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename RemoveLayerVersionPermissionRequestT = Model::RemoveLayerVersionPermissionRequest>
        void RemoveLayerVersionPermissionAsync(const RemoveLayerVersionPermissionRequestT& request, const RemoveLayerVersionPermissionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::RemoveLayerVersionPermission, request, handler, context);
        }

        /**
         * <p>Revokes function-use permission from an Amazon Web Services service or
         * another Amazon Web Services account. You can get the ID of the statement from
         * the output of <a>GetPolicy</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/RemovePermission">AWS
         * API Reference</a></p>
         */
        virtual Model::RemovePermissionOutcome RemovePermission(const Model::RemovePermissionRequest& request) const;

        /**
         * A Callable wrapper for RemovePermission that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename RemovePermissionRequestT = Model::RemovePermissionRequest>
        Model::RemovePermissionOutcomeCallable RemovePermissionCallable(const RemovePermissionRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::RemovePermission, request);
        }

        /**
         * An Async wrapper for RemovePermission that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename RemovePermissionRequestT = Model::RemovePermissionRequest>
        void RemovePermissionAsync(const RemovePermissionRequestT& request, const RemovePermissionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::RemovePermission, request, handler, context);
        }

        /**
         * <p>Adds <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/tagging.html">tags</a> to a
         * function, event source mapping, or code signing configuration.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/TagResource">AWS
         * API Reference</a></p>
         */
        virtual Model::TagResourceOutcome TagResource(const Model::TagResourceRequest& request) const;

        /**
         * A Callable wrapper for TagResource that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename TagResourceRequestT = Model::TagResourceRequest>
        Model::TagResourceOutcomeCallable TagResourceCallable(const TagResourceRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::TagResource, request);
        }

        /**
         * An Async wrapper for TagResource that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename TagResourceRequestT = Model::TagResourceRequest>
        void TagResourceAsync(const TagResourceRequestT& request, const TagResourceResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::TagResource, request, handler, context);
        }

        /**
         * <p>Removes <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/tagging.html">tags</a> from a
         * function, event source mapping, or code signing configuration.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UntagResource">AWS
         * API Reference</a></p>
         */
        virtual Model::UntagResourceOutcome UntagResource(const Model::UntagResourceRequest& request) const;

        /**
         * A Callable wrapper for UntagResource that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UntagResourceRequestT = Model::UntagResourceRequest>
        Model::UntagResourceOutcomeCallable UntagResourceCallable(const UntagResourceRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UntagResource, request);
        }

        /**
         * An Async wrapper for UntagResource that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UntagResourceRequestT = Model::UntagResourceRequest>
        void UntagResourceAsync(const UntagResourceRequestT& request, const UntagResourceResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UntagResource, request, handler, context);
        }

        /**
         * <p>Updates the configuration of a Lambda function <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias</a>.</p><p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateAlias">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateAliasOutcome UpdateAlias(const Model::UpdateAliasRequest& request) const;

        /**
         * A Callable wrapper for UpdateAlias that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateAliasRequestT = Model::UpdateAliasRequest>
        Model::UpdateAliasOutcomeCallable UpdateAliasCallable(const UpdateAliasRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateAlias, request);
        }

        /**
         * An Async wrapper for UpdateAlias that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateAliasRequestT = Model::UpdateAliasRequest>
        void UpdateAliasAsync(const UpdateAliasRequestT& request, const UpdateAliasResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateAlias, request, handler, context);
        }

        /**
         * <p>Update the code signing configuration. Changes to the code signing
         * configuration take effect the next time a user tries to deploy a code package to
         * the function. </p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateCodeSigningConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateCodeSigningConfigOutcome UpdateCodeSigningConfig(const Model::UpdateCodeSigningConfigRequest& request) const;

        /**
         * A Callable wrapper for UpdateCodeSigningConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateCodeSigningConfigRequestT = Model::UpdateCodeSigningConfigRequest>
        Model::UpdateCodeSigningConfigOutcomeCallable UpdateCodeSigningConfigCallable(const UpdateCodeSigningConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateCodeSigningConfig, request);
        }

        /**
         * An Async wrapper for UpdateCodeSigningConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateCodeSigningConfigRequestT = Model::UpdateCodeSigningConfigRequest>
        void UpdateCodeSigningConfigAsync(const UpdateCodeSigningConfigRequestT& request, const UpdateCodeSigningConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateCodeSigningConfig, request, handler, context);
        }

        /**
         * <p>Updates an event source mapping. You can change the function that Lambda
         * invokes, or pause invocation and resume later from the same location.</p> <p>For
         * details about how to configure different event sources, see the following
         * topics. </p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-ddb.html#services-dynamodb-eventsourcemapping">
         * Amazon DynamoDB Streams</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-kinesis.html#services-kinesis-eventsourcemapping">
         * Amazon Kinesis</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-sqs.html#events-sqs-eventsource">
         * Amazon SQS</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-mq.html#services-mq-eventsourcemapping">
         * Amazon MQ and RabbitMQ</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-msk.html"> Amazon
         * MSK</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/kafka-smaa.html"> Apache
         * Kafka</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-documentdb.html"> Amazon
         * DocumentDB</a> </p> </li> </ul> <p>The following error handling options are
         * available only for DynamoDB and Kinesis event sources:</p> <ul> <li> <p>
         * <code>BisectBatchOnFunctionError</code> – If the function returns an error,
         * split the batch in two and retry.</p> </li> <li> <p>
         * <code>MaximumRecordAgeInSeconds</code> – Discard records older than the
         * specified age. The default value is infinite (-1). When set to infinite (-1),
         * failed records are retried until the record expires</p> </li> <li> <p>
         * <code>MaximumRetryAttempts</code> – Discard records after the specified number
         * of retries. The default value is infinite (-1). When set to infinite (-1),
         * failed records are retried until the record expires.</p> </li> <li> <p>
         * <code>ParallelizationFactor</code> – Process multiple batches from each shard
         * concurrently.</p> </li> </ul> <p>For stream sources (DynamoDB, Kinesis, Amazon
         * MSK, and self-managed Apache Kafka), the following option is also available:</p>
         * <ul> <li> <p> <code>DestinationConfig</code> – Send discarded records to an
         * Amazon SQS queue, Amazon SNS topic, or Amazon S3 bucket.</p> </li> </ul> <p>For
         * information about which configuration parameters apply to each event source, see
         * the following topics.</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-ddb.html#services-ddb-params">
         * Amazon DynamoDB Streams</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-kinesis.html#services-kinesis-params">
         * Amazon Kinesis</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-sqs.html#services-sqs-params">
         * Amazon SQS</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-mq.html#services-mq-params">
         * Amazon MQ and RabbitMQ</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-msk.html#services-msk-parms">
         * Amazon MSK</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-kafka.html#services-kafka-parms">
         * Apache Kafka</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/with-documentdb.html#docdb-configuration">
         * Amazon DocumentDB</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateEventSourceMapping">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateEventSourceMappingOutcome UpdateEventSourceMapping(const Model::UpdateEventSourceMappingRequest& request) const;

        /**
         * A Callable wrapper for UpdateEventSourceMapping that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateEventSourceMappingRequestT = Model::UpdateEventSourceMappingRequest>
        Model::UpdateEventSourceMappingOutcomeCallable UpdateEventSourceMappingCallable(const UpdateEventSourceMappingRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateEventSourceMapping, request);
        }

        /**
         * An Async wrapper for UpdateEventSourceMapping that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateEventSourceMappingRequestT = Model::UpdateEventSourceMappingRequest>
        void UpdateEventSourceMappingAsync(const UpdateEventSourceMappingRequestT& request, const UpdateEventSourceMappingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateEventSourceMapping, request, handler, context);
        }

        /**
         * <p>Updates a Lambda function's code. If code signing is enabled for the
         * function, the code package must be signed by a trusted publisher. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-codesigning.html">Configuring
         * code signing for Lambda</a>.</p> <p>If the function's package type is
         * <code>Image</code>, then you must specify the code package in
         * <code>ImageUri</code> as the URI of a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-images.html">container
         * image</a> in the Amazon ECR registry.</p> <p>If the function's package type is
         * <code>Zip</code>, then you must specify the deployment package as a <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/gettingstarted-package.html#gettingstarted-package-zip">.zip
         * file archive</a>. Enter the Amazon S3 bucket and key of the code .zip file
         * location. You can also provide the function code inline using the
         * <code>ZipFile</code> field.</p> <p>The code in the deployment package must be
         * compatible with the target instruction set architecture of the function
         * (<code>x86-64</code> or <code>arm64</code>).</p> <p>The function's code is
         * locked when you publish a version. You can't modify the code of a published
         * version, only the unpublished version.</p>  <p>For a function defined as a
         * container image, Lambda resolves the image tag to an image digest. In Amazon
         * ECR, if you update the image tag to a new image, Lambda does not automatically
         * update the function.</p> <p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateFunctionCode">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateFunctionCodeOutcome UpdateFunctionCode(const Model::UpdateFunctionCodeRequest& request) const;

        /**
         * A Callable wrapper for UpdateFunctionCode that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateFunctionCodeRequestT = Model::UpdateFunctionCodeRequest>
        Model::UpdateFunctionCodeOutcomeCallable UpdateFunctionCodeCallable(const UpdateFunctionCodeRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateFunctionCode, request);
        }

        /**
         * An Async wrapper for UpdateFunctionCode that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateFunctionCodeRequestT = Model::UpdateFunctionCodeRequest>
        void UpdateFunctionCodeAsync(const UpdateFunctionCodeRequestT& request, const UpdateFunctionCodeResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateFunctionCode, request, handler, context);
        }

        /**
         * <p>Modify the version-specific settings of a Lambda function.</p> <p>When you
         * update a function, Lambda provisions an instance of the function and its
         * supporting resources. If your function connects to a VPC, this process can take
         * a minute. During this time, you can't modify the function, but you can still
         * invoke it. The <code>LastUpdateStatus</code>,
         * <code>LastUpdateStatusReason</code>, and <code>LastUpdateStatusReasonCode</code>
         * fields in the response from <a>GetFunctionConfiguration</a> indicate when the
         * update is complete and the function is processing events with the new
         * configuration. For more information, see <a
         * href="https://docs.aws.amazon.com/lambda/latest/dg/functions-states.html">Lambda
         * function states</a>.</p> <p>These settings can vary between versions of a
         * function and are locked when you publish a version. You can't modify the
         * configuration of a published version, only the unpublished version.</p> <p>To
         * configure function concurrency, use <a>PutFunctionConcurrency</a>. To grant
         * invoke permissions to an Amazon Web Services account or Amazon Web Services
         * service, use <a>AddPermission</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateFunctionConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateFunctionConfigurationOutcome UpdateFunctionConfiguration(const Model::UpdateFunctionConfigurationRequest& request) const;

        /**
         * A Callable wrapper for UpdateFunctionConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateFunctionConfigurationRequestT = Model::UpdateFunctionConfigurationRequest>
        Model::UpdateFunctionConfigurationOutcomeCallable UpdateFunctionConfigurationCallable(const UpdateFunctionConfigurationRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateFunctionConfiguration, request);
        }

        /**
         * An Async wrapper for UpdateFunctionConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateFunctionConfigurationRequestT = Model::UpdateFunctionConfigurationRequest>
        void UpdateFunctionConfigurationAsync(const UpdateFunctionConfigurationRequestT& request, const UpdateFunctionConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateFunctionConfiguration, request, handler, context);
        }

        /**
         * <p>Updates the configuration for asynchronous invocation for a function,
         * version, or alias.</p> <p>To configure options for asynchronous invocation, use
         * <a>PutFunctionEventInvokeConfig</a>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateFunctionEventInvokeConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateFunctionEventInvokeConfigOutcome UpdateFunctionEventInvokeConfig(const Model::UpdateFunctionEventInvokeConfigRequest& request) const;

        /**
         * A Callable wrapper for UpdateFunctionEventInvokeConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateFunctionEventInvokeConfigRequestT = Model::UpdateFunctionEventInvokeConfigRequest>
        Model::UpdateFunctionEventInvokeConfigOutcomeCallable UpdateFunctionEventInvokeConfigCallable(const UpdateFunctionEventInvokeConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateFunctionEventInvokeConfig, request);
        }

        /**
         * An Async wrapper for UpdateFunctionEventInvokeConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateFunctionEventInvokeConfigRequestT = Model::UpdateFunctionEventInvokeConfigRequest>
        void UpdateFunctionEventInvokeConfigAsync(const UpdateFunctionEventInvokeConfigRequestT& request, const UpdateFunctionEventInvokeConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateFunctionEventInvokeConfig, request, handler, context);
        }

        /**
         * <p>Updates the configuration for a Lambda function URL.</p><p><h3>See Also:</h3>
         * <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/UpdateFunctionUrlConfig">AWS
         * API Reference</a></p>
         */
        virtual Model::UpdateFunctionUrlConfigOutcome UpdateFunctionUrlConfig(const Model::UpdateFunctionUrlConfigRequest& request) const;

        /**
         * A Callable wrapper for UpdateFunctionUrlConfig that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UpdateFunctionUrlConfigRequestT = Model::UpdateFunctionUrlConfigRequest>
        Model::UpdateFunctionUrlConfigOutcomeCallable UpdateFunctionUrlConfigCallable(const UpdateFunctionUrlConfigRequestT& request) const
        {
            return SubmitCallable(&LambdaClient::UpdateFunctionUrlConfig, request);
        }

        /**
         * An Async wrapper for UpdateFunctionUrlConfig that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UpdateFunctionUrlConfigRequestT = Model::UpdateFunctionUrlConfigRequest>
        void UpdateFunctionUrlConfigAsync(const UpdateFunctionUrlConfigRequestT& request, const UpdateFunctionUrlConfigResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&LambdaClient::UpdateFunctionUrlConfig, request, handler, context);
        }


      void OverrideEndpoint(const Aws::String& endpoint);
      std::shared_ptr<LambdaEndpointProviderBase>& accessEndpointProvider();
    private:
      friend class Aws::Client::ClientWithAsyncTemplateMethods<LambdaClient>;
      void init(const LambdaClientConfiguration& clientConfiguration);

      LambdaClientConfiguration m_clientConfiguration;
      std::shared_ptr<LambdaEndpointProviderBase> m_endpointProvider;
  };

} // namespace Lambda
} // namespace Aws
