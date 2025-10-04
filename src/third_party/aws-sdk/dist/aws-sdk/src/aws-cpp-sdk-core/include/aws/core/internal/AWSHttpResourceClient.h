/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/AWSErrorMarshaller.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/DateTime.h>
#include <memory>
#include <mutex>
namespace Aws
{
    namespace Http
    {
        class HttpClient;
        class HttpRequest;
        enum class HttpResponseCode;
    } // namespace Http

    namespace Internal
    {
        /**
         * Simple client for accessing the AWS remote data by Http.
         * Currently we use it to access EC2 Metadata and ECS Credential.
         */
        class AWS_CORE_API AWSHttpResourceClient
        {
        public:
            /**
             * Builds an AWSHttpResourceClient instance by using default http stack.
             */
            AWSHttpResourceClient(const char* logtag = "AWSHttpResourceClient");
            AWSHttpResourceClient(const Client::ClientConfiguration& clientConfiguration, const char* logtag = "AWSHttpResourceClient");

            AWSHttpResourceClient& operator =(const AWSHttpResourceClient& rhs) = delete;
            AWSHttpResourceClient(const AWSHttpResourceClient& rhs) = delete;
            AWSHttpResourceClient& operator =(const AWSHttpResourceClient&& rhs) = delete;
            AWSHttpResourceClient(const AWSHttpResourceClient&& rhs) = delete;

            virtual ~AWSHttpResourceClient();

            /**
             * Connects to an HTTP endpoint to read the specified resource and returns the text contents.
             * The resource URI = endpoint + resourcePath (e.g:http://domain/path/to/res)
             * @param endpoint The HTTP resource to connect to.
             * @param resourcePath A path appended to the endpoint to form the final URI.
             * @param authToken An optional authorization token that will be passed as the value of the HTTP
             * header 'Authorization'.
             * @return The response from the HTTP endpoint as a string.
             */
            virtual Aws::String GetResource(const char* endpoint, const char* resourcePath, const char* authToken) const;

            /**
             * Connects to an HTTP endpoint to read the specified resource and returns the text contents.
             * The resource URI = endpoint + resourcePath (e.g:http://domain/path/to/res)
             * @param endpoint The HTTP resource to connect to.
             * @param resourcePath A path appended to the endpoint to form the final URI.
             * @param authToken An optional authorization token that will be passed as the value of the HTTP
             * header 'Authorization'.
             * @return The response from the HTTP endpoint as a string, together with the http response code.
             */
            virtual AmazonWebServiceResult<Aws::String> GetResourceWithAWSWebServiceResult(const char *endpoint, const char *resourcePath, const char *authToken) const;

            /**
             * Above Function: Aws::String GetResource(const char* endpoint, const char* resourcePath, const char* authToken) const;
             * is limited to HTTP GET method and caller can't add wanted HTTP headers as well.
             * This overload gives caller the flexibility to manipulate the request, as well returns the HttpResponseCode of the last attempt.
             */
            virtual AmazonWebServiceResult<Aws::String> GetResourceWithAWSWebServiceResult(const std::shared_ptr<Http::HttpRequest> &httpRequest) const;

            /**
             * Set an error marshaller so as to marshall error type from http response body if any.
             * So that it can work with retry strategy to decide if a request should retry or not.
             */
            void SetErrorMarshaller(Aws::UniquePtr<Client::AWSErrorMarshaller> errorMarshaller) { m_errorMarshaller = std::move(errorMarshaller); }
            const Client::AWSErrorMarshaller* GetErrorMarshaller() const { return m_errorMarshaller.get(); }

        protected:
            Aws::String m_logtag;
            Aws::String m_userAgent;

        private:
            std::shared_ptr<Client::RetryStrategy> m_retryStrategy;
            std::shared_ptr<Http::HttpClient> m_httpClient;
            Aws::UniquePtr<Client::AWSErrorMarshaller> m_errorMarshaller;
        };

        /**
         * Derived class to support retrieving of EC2 Metadata
         */
        class AWS_CORE_API EC2MetadataClient : public AWSHttpResourceClient
        {
        public:
            /**
             * Build an instance with default EC2 Metadata service endpoint
             */
            EC2MetadataClient(const char* endpoint = "http://169.254.169.254");
            EC2MetadataClient(const Client::ClientConfiguration& clientConfiguration, const char* endpoint = "http://169.254.169.254");

            EC2MetadataClient& operator =(const EC2MetadataClient& rhs) = delete;
            EC2MetadataClient(const EC2MetadataClient& rhs) = delete;
            EC2MetadataClient& operator =(const EC2MetadataClient&& rhs) = delete;
            EC2MetadataClient(const EC2MetadataClient&& rhs) = delete;

            virtual ~EC2MetadataClient();

            using AWSHttpResourceClient::GetResource;

            /**
            * Connects to the metadata service to read the specified resource and
            * returns the text contents. The resource URI = m_endpoint + resourcePath.
            */
            virtual Aws::String GetResource(const char* resourcePath) const;

#if !defined(DISABLE_IMDSV1)
            /**
             * Connects to the Amazon EC2 Instance Metadata Service to retrieve the
             * default credential information (if any).
             */
            virtual Aws::String GetDefaultCredentials() const;
#endif
            /**
             * Connects to the Amazon EC2 Instance Metadata Service to retrieve the
             * credential information (if any) in a more secure way.
             */
            virtual Aws::String GetDefaultCredentialsSecurely() const;

            /**
             * connects to the Amazon EC2 Instance metadata Service to retrieve the region
             * the current EC2 instance is running in.
             */
            virtual Aws::String GetCurrentRegion() const;

            /**
             * Sets endpoint used to connect to the EC2 Instance metadata Service
             */
            virtual void SetEndpoint(const Aws::String& endpoint);

            /**
             * Gets endpoint used to connect to the EC2 Instance metadata Service
             */
            virtual Aws::String GetEndpoint() const;

        private:
            Aws::String m_endpoint;
            bool m_disableIMDS;
            mutable std::recursive_mutex m_tokenMutex;
            mutable Aws::String m_token;
            mutable bool m_tokenRequired;
            mutable Aws::String m_region;
            bool m_disableIMDSV1 = false;
        };

        void AWS_CORE_API InitEC2MetadataClient();
        void AWS_CORE_API CleanupEC2MetadataClient();
        std::shared_ptr<EC2MetadataClient> AWS_CORE_API GetEC2MetadataClient();

        /**
         * Derived class to support retrieving of ECS Credentials
         */
        class AWS_CORE_API ECSCredentialsClient : public AWSHttpResourceClient
        {
        public:
            /**
             * Build an instance with default ECS service endpoint
             * @param resourcePath The path part of the metadata URL
             * @param endpoint The URL authority to hit. Default is the IP address of the Task metadata service endpoint.
             */
            ECSCredentialsClient(const char* resourcePath, const char* endpoint = "http://169.254.170.2",
                    const char* authToken = "");
            ECSCredentialsClient(const Client::ClientConfiguration& clientConfiguration,
                                 const char* resourcePath, const char* endpoint = "http://169.254.170.2",
                                 const char* authToken = "");

            ECSCredentialsClient& operator =(ECSCredentialsClient& rhs) = delete;
            ECSCredentialsClient(const ECSCredentialsClient& rhs) = delete;
            ECSCredentialsClient& operator =(ECSCredentialsClient&& rhs) = delete;
            ECSCredentialsClient(const ECSCredentialsClient&& rhs) = delete;

            /**
             * Connects to the Amazon ECS service to retrieve the credential
             */
            virtual Aws::String GetECSCredentials() const
            {
                return GetResource(m_endpoint.c_str(),
                                   m_resourcePath.c_str(),
                                   m_token.empty() ? nullptr : m_token.c_str());
            }

            inline void SetToken(Aws::String token)
            {
                m_token = std::move(token);
            }

        private:
            Aws::String m_resourcePath;
            Aws::String m_endpoint;
            Aws::String m_token;
        };

        /**
         * To support retrieving credentials from STS.
         * Note that STS accepts request with protocol of queryxml. Calling GetResource() will trigger
         * a query request using AWSHttpResourceClient under the hood.
         */
        class AWS_CORE_API STSCredentialsClient : public AWSHttpResourceClient
        {
        public:
            /**
             * Initializes the provider to retrieve credentials from STS when it expires.
             */
            STSCredentialsClient(const Client::ClientConfiguration& clientConfiguration);

            STSCredentialsClient& operator =(STSCredentialsClient& rhs) = delete;
            STSCredentialsClient(const STSCredentialsClient& rhs) = delete;
            STSCredentialsClient& operator =(STSCredentialsClient&& rhs) = delete;
            STSCredentialsClient(const STSCredentialsClient&& rhs) = delete;

            // If you want to make an AssumeRoleWithWebIdentity call to sts. use these classes to pass data to and get info from STSCredentialsClient client.
            // If you want to make an AssumeRole call to sts, define the request/result members class/struct like this.
            struct STSAssumeRoleWithWebIdentityRequest
            {
                Aws::String roleSessionName;
                Aws::String roleArn;
                Aws::String webIdentityToken;
            };

            struct STSAssumeRoleWithWebIdentityResult
            {
                Aws::Auth::AWSCredentials creds;
            };

            STSAssumeRoleWithWebIdentityResult GetAssumeRoleWithWebIdentityCredentials(const STSAssumeRoleWithWebIdentityRequest& request);

        private:
            Aws::String m_endpoint;
        };

        /**
         * To support retrieving credentials from SSO.
         */
         class AWS_CORE_API SSOCredentialsClient : public AWSHttpResourceClient
         {
         public:
             SSOCredentialsClient(const Client::ClientConfiguration& clientConfiguration);
             SSOCredentialsClient(const Client::ClientConfiguration& clientConfiguration, Aws::Http::Scheme scheme, const Aws::String& region);

             SSOCredentialsClient& operator =(SSOCredentialsClient& rhs) = delete;
             SSOCredentialsClient(const SSOCredentialsClient& rhs) = delete;
             SSOCredentialsClient& operator =(SSOCredentialsClient&& rhs) = delete;
             SSOCredentialsClient(SSOCredentialsClient&& rhs) = delete;

             struct SSOGetRoleCredentialsRequest
             {
                 Aws::String m_ssoAccountId;
                 Aws::String m_ssoRoleName;
                 Aws::String m_accessToken;
             };

             struct SSOGetRoleCredentialsResult
             {
                 Aws::Auth::AWSCredentials creds;
             };

             SSOGetRoleCredentialsResult GetSSOCredentials(const SSOGetRoleCredentialsRequest& request);

             struct SSOCreateTokenRequest
             {
                 Aws::String clientId;
                 Aws::String clientSecret;
                 Aws::String grantType;
                 Aws::String refreshToken;
             };

             struct SSOCreateTokenResult
             {
                 Aws::String accessToken;
                 size_t expiresIn = 0; //seconds
                 Aws::String idToken;
                 Aws::String refreshToken;
                 Aws::String clientId;
                 Aws::String tokenType;
             };

             SSOCreateTokenResult CreateToken(const SSOCreateTokenRequest& request);
         private:
             Aws::String buildEndpoint(Aws::Http::Scheme scheme,
                 const Aws::String& region,
                 const Aws::String& domain,
                 const Aws::String& endpoint);
             Aws::String m_endpoint;
             Aws::String m_oidcEndpoint;
         };
    } // namespace Internal
} // namespace Aws
