/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/AWSClient.h>
#include <aws/core/client/AWSClientAsyncCRTP.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/DNS.h>
#include <aws/s3/S3ServiceClientModel.h>

// TODO: temporary fix for naming conflicts on Windows.
#ifdef _WIN32
#ifdef GetObject
#undef GetObject
#endif
#endif

namespace Aws
{
  namespace S3
  {
    namespace SSEHeaders
    {
        static const char SERVER_SIDE_ENCRYPTION[] = "x-amz-server-side-encryption";
        static const char SERVER_SIDE_ENCRYPTION_AWS_KMS_KEY_ID[] = "x-amz-server-side-encryption-aws-kms-key-id";
        static const char SERVER_SIDE_ENCRYPTION_CUSTOMER_ALGORITHM[] = "x-amz-server-side-encryption-customer-algorithm";
        static const char SERVER_SIDE_ENCRYPTION_CUSTOMER_KEY[] = "x-amz-server-side-encryption-customer-key";
        static const char SERVER_SIDE_ENCRYPTION_CUSTOMER_KEY_MD5[] = "x-amz-server-side-encryption-customer-key-MD5";
    } // SS3Headers

    //max expiration for presigned urls in s3 is 7 days.
    static const unsigned MAX_EXPIRATION_SECONDS = 7 * 24 * 60 * 60;

    /**
     * <p/>
     */
    class AWS_S3_API S3Client : public Aws::Client::AWSXMLClient, public Aws::Client::ClientWithAsyncTemplateMethods<S3Client>
    {
    public:
        typedef Aws::Client::AWSXMLClient BASECLASS;
        static const char* GetServiceName();
        static const char* GetAllocationTag();

      typedef S3ClientConfiguration ClientConfigurationType;
      typedef S3EndpointProvider EndpointProviderType;

        /**
         * Copy constructor for a S3Client. Copies all members that do not reference self.
         * Recreates member that reference self.
         * @param rhs the source object of the copy.
         */
        S3Client(const S3Client &rhs);

        /**
         * Assignment operator for a S3Client. Copies all members that do not reference self.
         * Recreates member that reference self.
         * @param rhs the source object of the copy.
         * @return the copied client.
         */
        S3Client& operator=(const S3Client &rhs);

        /**
         * Copy move constructor for a S3Client. Copies all members that do not reference self.
         * Recreates member that reference self.
         * @param rhs the source object of the copy.
         */
        S3Client(S3Client &&rhs) noexcept;

        /**
         * Assignment move operator for a S3Client. Copies all members that do not reference self.
         * Recreates member that reference self.
         * @param rhs the source object of the copy.
         * @return the copied client.
         */
        S3Client& operator=(S3Client &&rhs) noexcept;
       /**
        * Initializes client to use DefaultCredentialProviderChain, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        S3Client(const Aws::S3::S3ClientConfiguration& clientConfiguration = Aws::S3::S3ClientConfiguration(),
                 std::shared_ptr<S3EndpointProviderBase> endpointProvider = nullptr);

       /**
        * Initializes client to use SimpleAWSCredentialsProvider, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        S3Client(const Aws::Auth::AWSCredentials& credentials,
                 std::shared_ptr<S3EndpointProviderBase> endpointProvider = nullptr,
                 const Aws::S3::S3ClientConfiguration& clientConfiguration = Aws::S3::S3ClientConfiguration());

       /**
        * Initializes client to use specified credentials provider with specified client config. If http client factory is not supplied,
        * the default http client factory will be used
        */
        S3Client(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& credentialsProvider,
                 std::shared_ptr<S3EndpointProviderBase> endpointProvider = nullptr,
                 const Aws::S3::S3ClientConfiguration& clientConfiguration = Aws::S3::S3ClientConfiguration());


        /* Legacy constructors due deprecation */
       /**
        * Initializes client to use DefaultCredentialProviderChain, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        S3Client(const Aws::Client::ClientConfiguration& clientConfiguration,
                 Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signPayloads,
                 bool useVirtualAddressing,
                 Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION USEast1RegionalEndPointOption = Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET);

       /**
        * Initializes client to use SimpleAWSCredentialsProvider, with default http client factory, and optional client config. If client config
        * is not specified, it will be initialized to default values.
        */
        S3Client(const Aws::Auth::AWSCredentials& credentials,
                 const Aws::Client::ClientConfiguration& clientConfiguration,
                 Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signPayloads,
                 bool useVirtualAddressing,
                 Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION USEast1RegionalEndPointOption = Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET);

       /**
        * Initializes client to use specified credentials provider with specified client config. If http client factory is not supplied,
        * the default http client factory will be used
        */
        S3Client(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider>& credentialsProvider,
                 const Aws::Client::ClientConfiguration& clientConfiguration,
                 Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signPayloads,
                 bool useVirtualAddressing,
                 Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION USEast1RegionalEndPointOption = Aws::S3::US_EAST_1_REGIONAL_ENDPOINT_OPTION::NOT_SET);

        /* End of legacy constructors due deprecation */
        virtual ~S3Client();

        /**
         * <p>This operation aborts a multipart upload. After a multipart upload is
         * aborted, no additional parts can be uploaded using that upload ID. The storage
         * consumed by any previously uploaded parts will be freed. However, if any part
         * uploads are currently in progress, those part uploads might or might not
         * succeed. As a result, it might be necessary to abort a given multipart upload
         * multiple times in order to completely free all storage consumed by all parts.
         * </p> <p>To verify that all parts have been removed and prevent getting charged
         * for the part storage, you should call the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * API operation and ensure that the parts list is empty.</p>  <ul> <li> <p>
         * <b>Directory buckets</b> - If multipart uploads in a directory bucket are in
         * progress, you can't delete the bucket until all the in-progress multipart
         * uploads are aborted or completed. To delete these in-progress multipart uploads,
         * use the <code>ListMultipartUploads</code> operation to list the in-progress
         * multipart uploads in the bucket and use the <code>AbortMultipartUpload</code>
         * operation to abort all the in-progress multipart uploads. </p> </li> <li> <p>
         * <b>Directory buckets</b> - For directory buckets, you must make requests for
         * this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General
         * purpose bucket permissions</b> - For information about permissions required to
         * use the multipart upload, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuAndPermissions.html">Multipart
         * Upload and Permissions</a> in the <i>Amazon S3 User Guide</i>.</p> </li> <li>
         * <p> <b>Directory bucket permissions</b> - To grant access to this API operation
         * on a directory bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>AbortMultipartUpload</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListMultipartUploads.html">ListMultipartUploads</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AbortMultipartUpload">AWS
         * API Reference</a></p>
         */
        virtual Model::AbortMultipartUploadOutcome AbortMultipartUpload(const Model::AbortMultipartUploadRequest& request) const;

        /**
         * A Callable wrapper for AbortMultipartUpload that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename AbortMultipartUploadRequestT = Model::AbortMultipartUploadRequest>
        Model::AbortMultipartUploadOutcomeCallable AbortMultipartUploadCallable(const AbortMultipartUploadRequestT& request) const
        {
            return SubmitCallable(&S3Client::AbortMultipartUpload, request);
        }

        /**
         * An Async wrapper for AbortMultipartUpload that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename AbortMultipartUploadRequestT = Model::AbortMultipartUploadRequest>
        void AbortMultipartUploadAsync(const AbortMultipartUploadRequestT& request, const AbortMultipartUploadResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::AbortMultipartUpload, request, handler, context);
        }

        /**
         * <p>Completes a multipart upload by assembling previously uploaded parts.</p>
         * <p>You first initiate the multipart upload and then upload all parts using the
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * operation or the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>
         * operation. After successfully uploading all relevant parts of an upload, you
         * call this <code>CompleteMultipartUpload</code> operation to complete the upload.
         * Upon receiving this request, Amazon S3 concatenates all the parts in ascending
         * order by part number to create a new object. In the CompleteMultipartUpload
         * request, you must provide the parts list and ensure that the parts list is
         * complete. The CompleteMultipartUpload API operation concatenates the parts that
         * you provide in the list. For each part in the list, you must provide the
         * <code>PartNumber</code> value and the <code>ETag</code> value that are returned
         * after that part was uploaded.</p> <p>The processing of a CompleteMultipartUpload
         * request could take several minutes to finalize. After Amazon S3 begins
         * processing the request, it sends an HTTP response header that specifies a
         * <code>200 OK</code> response. While processing is in progress, Amazon S3
         * periodically sends white space characters to keep the connection from timing
         * out. A request could fail after the initial <code>200 OK</code> response has
         * been sent. This means that a <code>200 OK</code> response can contain either a
         * success or an error. The error response might be embedded in the <code>200
         * OK</code> response. If you call this API operation directly, make sure to design
         * your application to parse the contents of the response and handle it
         * appropriately. If you use Amazon Web Services SDKs, SDKs handle this condition.
         * The SDKs detect the embedded error and apply error handling per your
         * configuration settings (including automatically retrying the request as
         * appropriate). If the condition persists, the SDKs throw an exception (or, for
         * the SDKs that don't use exceptions, they return an error). </p> <p>Note that if
         * <code>CompleteMultipartUpload</code> fails, applications should be prepared to
         * retry any failed requests (including 500 error responses). For more information,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ErrorBestPractices.html">Amazon
         * S3 Error Best Practices</a>.</p>  <p>You can't use
         * <code>Content-Type: application/x-www-form-urlencoded</code> for the
         * CompleteMultipartUpload requests. Also, if you don't provide a
         * <code>Content-Type</code> header, <code>CompleteMultipartUpload</code> can still
         * return a <code>200 OK</code> response.</p>  <p>For more information
         * about multipart uploads, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/uploadobjusingmpu.html">Uploading
         * Objects Using Multipart Upload</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p> <b>Directory buckets</b> - For directory buckets, you must make
         * requests for this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - For information about permissions required to use the
         * multipart upload API, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuAndPermissions.html">Multipart
         * Upload and Permissions</a> in the <i>Amazon S3 User Guide</i>.</p> <p>If you
         * provide an <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_Checksum.html">additional
         * checksum value</a> in your <code>MultipartUpload</code> requests and the object
         * is encrypted with Key Management Service, you must have permission to use the
         * <code>kms:Decrypt</code> action for the <code>CompleteMultipartUpload</code>
         * request to succeed.</p> </li> <li> <p> <b>Directory bucket permissions</b> - To
         * grant access to this API operation on a directory bucket, we recommend that you
         * use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> <p>If the object is encrypted with SSE-KMS,
         * you must also have the <code>kms:GenerateDataKey</code> and
         * <code>kms:Decrypt</code> permissions in IAM identity-based policies and KMS key
         * policies for the KMS key.</p> </li> </ul> </dd> <dt>Special errors</dt> <dd>
         * <ul> <li> <p>Error Code: <code>EntityTooSmall</code> </p> <ul> <li>
         * <p>Description: Your proposed upload is smaller than the minimum allowed object
         * size. Each part must be at least 5 MB in size, except the last part.</p> </li>
         * <li> <p>HTTP Status Code: 400 Bad Request</p> </li> </ul> </li> <li> <p>Error
         * Code: <code>InvalidPart</code> </p> <ul> <li> <p>Description: One or more of the
         * specified parts could not be found. The part might not have been uploaded, or
         * the specified ETag might not have matched the uploaded part's ETag.</p> </li>
         * <li> <p>HTTP Status Code: 400 Bad Request</p> </li> </ul> </li> <li> <p>Error
         * Code: <code>InvalidPartOrder</code> </p> <ul> <li> <p>Description: The list of
         * parts was not in ascending order. The parts list must be specified in order by
         * part number.</p> </li> <li> <p>HTTP Status Code: 400 Bad Request</p> </li> </ul>
         * </li> <li> <p>Error Code: <code>NoSuchUpload</code> </p> <ul> <li>
         * <p>Description: The specified multipart upload does not exist. The upload ID
         * might be invalid, or the multipart upload might have been aborted or
         * completed.</p> </li> <li> <p>HTTP Status Code: 404 Not Found</p> </li> </ul>
         * </li> </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets
         * </b> - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>CompleteMultipartUpload</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListMultipartUploads.html">ListMultipartUploads</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CompleteMultipartUpload">AWS
         * API Reference</a></p>
         */
        virtual Model::CompleteMultipartUploadOutcome CompleteMultipartUpload(const Model::CompleteMultipartUploadRequest& request) const;

        /**
         * A Callable wrapper for CompleteMultipartUpload that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CompleteMultipartUploadRequestT = Model::CompleteMultipartUploadRequest>
        Model::CompleteMultipartUploadOutcomeCallable CompleteMultipartUploadCallable(const CompleteMultipartUploadRequestT& request) const
        {
            return SubmitCallable(&S3Client::CompleteMultipartUpload, request);
        }

        /**
         * An Async wrapper for CompleteMultipartUpload that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CompleteMultipartUploadRequestT = Model::CompleteMultipartUploadRequest>
        void CompleteMultipartUploadAsync(const CompleteMultipartUploadRequestT& request, const CompleteMultipartUploadResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::CompleteMultipartUpload, request, handler, context);
        }

        /**
         * <p>Creates a copy of an object that is already stored in Amazon S3.</p> 
         * <p>You can store individual objects of up to 5 TB in Amazon S3. You create a
         * copy of your object up to 5 GB in size in a single atomic action using this API.
         * However, to copy an object greater than 5 GB, you must use the multipart upload
         * Upload Part - Copy (UploadPartCopy) API. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/CopyingObjctsUsingRESTMPUapi.html">Copy
         * Object Using the REST Multipart Upload API</a>.</p>  <p>You can copy
         * individual objects between general purpose buckets, between directory buckets,
         * and between general purpose buckets and directory buckets.</p>  <ul> <li>
         * <p>Amazon S3 supports copy operations using Multi-Region Access Points only as a
         * destination when using the Multi-Region Access Point ARN. </p> </li> <li> <p>
         * <b>Directory buckets </b> - For directory buckets, you must make requests for
         * this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> <li> <p>VPC endpoints don't support cross-Region requests (including
         * copies). If you're using VPC endpoints, your source and destination buckets
         * should be in the same Amazon Web Services Region as your VPC endpoint.</p> </li>
         * </ul>  <p>Both the Region that you want to copy the object from and the
         * Region that you want to copy the object to must be enabled for your account. For
         * more information about how to enable a Region for your account, see <a
         * href="https://docs.aws.amazon.com/accounts/latest/reference/manage-acct-regions.html#manage-acct-regions-enable-standalone">Enable
         * or disable a Region for standalone accounts</a> in the <i>Amazon Web Services
         * Account Management Guide</i>.</p>  <p>Amazon S3 transfer acceleration
         * does not support cross-Region copies. If you request a cross-Region copy using a
         * transfer acceleration endpoint, you get a <code>400 Bad Request</code> error.
         * For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/transfer-acceleration.html">Transfer
         * Acceleration</a>.</p>  <dl> <dt>Authentication and
         * authorization</dt> <dd> <p>All <code>CopyObject</code> requests must be
         * authenticated and signed by using IAM credentials (access key ID and secret
         * access key for the IAM identities). All headers with the <code>x-amz-</code>
         * prefix, including <code>x-amz-copy-source</code>, must be signed. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/RESTAuthentication.html">REST
         * Authentication</a>.</p> <p> <b>Directory buckets</b> - You must use the IAM
         * credentials to authenticate and authorize your access to the
         * <code>CopyObject</code> API operation, instead of using the temporary security
         * credentials through the <code>CreateSession</code> API operation.</p> <p>Amazon
         * Web Services CLI or SDKs handles authentication and authorization on your
         * behalf.</p> </dd> <dt>Permissions</dt> <dd> <p>You must have <i>read</i> access
         * to the source object and <i>write</i> access to the destination bucket.</p> <ul>
         * <li> <p> <b>General purpose bucket permissions</b> - You must have permissions
         * in an IAM policy based on the source and destination bucket types in a
         * <code>CopyObject</code> operation.</p> <ul> <li> <p>If the source object is in a
         * general purpose bucket, you must have <b> <code>s3:GetObject</code> </b>
         * permission to read the source object that is being copied. </p> </li> <li> <p>If
         * the destination bucket is a general purpose bucket, you must have <b>
         * <code>s3:PutObject</code> </b> permission to write the object copy to the
         * destination bucket. </p> </li> </ul> </li> <li> <p> <b>Directory bucket
         * permissions</b> - You must have permissions in a bucket policy or an IAM
         * identity-based policy based on the source and destination bucket types in a
         * <code>CopyObject</code> operation.</p> <ul> <li> <p>If the source object that
         * you want to copy is in a directory bucket, you must have the <b>
         * <code>s3express:CreateSession</code> </b> permission in the <code>Action</code>
         * element of a policy to read the object. By default, the session is in the
         * <code>ReadWrite</code> mode. If you want to restrict the access, you can
         * explicitly set the <code>s3express:SessionMode</code> condition key to
         * <code>ReadOnly</code> on the copy source bucket.</p> </li> <li> <p>If the copy
         * destination is a directory bucket, you must have the <b>
         * <code>s3express:CreateSession</code> </b> permission in the <code>Action</code>
         * element of a policy to write the object to the destination. The
         * <code>s3express:SessionMode</code> condition key can't be set to
         * <code>ReadOnly</code> on the copy destination bucket. </p> </li> </ul> <p>If the
         * object is encrypted with SSE-KMS, you must also have the
         * <code>kms:GenerateDataKey</code> and <code>kms:Decrypt</code> permissions in IAM
         * identity-based policies and KMS key policies for the KMS key.</p> <p>For example
         * policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-example-bucket-policies.html">Example
         * bucket policies for S3 Express One Zone</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-identity-policies.html">Amazon
         * Web Services Identity and Access Management (IAM) identity-based policies for S3
         * Express One Zone</a> in the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd>
         * <dt>Response and special errors</dt> <dd> <p>When the request is an HTTP 1.1
         * request, the response is chunk encoded. When the request is not an HTTP 1.1
         * request, the response would not contain the <code>Content-Length</code>. You
         * always need to read the entire response body to check if the copy succeeds. </p>
         * <ul> <li> <p>If the copy is successful, you receive a response with information
         * about the copied object.</p> </li> <li> <p>A copy request might return an error
         * when Amazon S3 receives the copy request or while Amazon S3 is copying the
         * files. A <code>200 OK</code> response can contain either a success or an
         * error.</p> <ul> <li> <p>If the error occurs before the copy action starts, you
         * receive a standard Amazon S3 error.</p> </li> <li> <p>If the error occurs during
         * the copy operation, the error response is embedded in the <code>200 OK</code>
         * response. For example, in a cross-region copy, you may encounter throttling and
         * receive a <code>200 OK</code> response. For more information, see <a
         * href="https://repost.aws/knowledge-center/s3-resolve-200-internalerror">Resolve
         * the Error 200 response when copying objects to Amazon S3</a>. The <code>200
         * OK</code> status code means the copy was accepted, but it doesn't mean the copy
         * is complete. Another example is when you disconnect from Amazon S3 before the
         * copy is complete, Amazon S3 might cancel the copy and you may receive a
         * <code>200 OK</code> response. You must stay connected to Amazon S3 until the
         * entire response is successfully received and processed.</p> <p>If you call this
         * API operation directly, make sure to design your application to parse the
         * content of the response and handle it appropriately. If you use Amazon Web
         * Services SDKs, SDKs handle this condition. The SDKs detect the embedded error
         * and apply error handling per your configuration settings (including
         * automatically retrying the request as appropriate). If the condition persists,
         * the SDKs throw an exception (or, for the SDKs that don't use exceptions, they
         * return an error).</p> </li> </ul> </li> </ul> </dd> <dt>Charge</dt> <dd> <p>The
         * copy request charge is based on the storage class and Region that you specify
         * for the destination object. The request can also result in a data retrieval
         * charge for the source if the source storage class bills for data retrieval. If
         * the copy source is in a different region, the data transfer is billed to the
         * copy source account. For pricing information, see <a
         * href="http://aws.amazon.com/s3/pricing/">Amazon S3 pricing</a>.</p> </dd>
         * <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP
         * Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>CopyObject</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CopyObject">AWS API
         * Reference</a></p>
         */
        virtual Model::CopyObjectOutcome CopyObject(const Model::CopyObjectRequest& request) const;

        /**
         * A Callable wrapper for CopyObject that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        virtual Model::CopyObjectOutcomeCallable CopyObjectCallable(const Model::CopyObjectRequest& request) const;

        /**
         * An Async wrapper for CopyObject that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        virtual void CopyObjectAsync(const Model::CopyObjectRequest& request, const CopyObjectResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const;

        /**
         *  <p>This action creates an Amazon S3 bucket. To create an Amazon S3 on
         * Outposts bucket, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_control_CreateBucket.html">
         * <code>CreateBucket</code> </a>.</p>  <p>Creates a new S3 bucket. To
         * create a bucket, you must set up Amazon S3 and have a valid Amazon Web Services
         * Access Key ID to authenticate requests. Anonymous requests are never allowed to
         * create buckets. By creating the bucket, you become the bucket owner.</p>
         * <p>There are two types of buckets: general purpose buckets and directory
         * buckets. For more information about these bucket types, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/creating-buckets-s3.html">Creating,
         * configuring, and working with Amazon S3 buckets</a> in the <i>Amazon S3 User
         * Guide</i>.</p>  <ul> <li> <p> <b>General purpose buckets</b> - If you send
         * your <code>CreateBucket</code> request to the <code>s3.amazonaws.com</code>
         * global endpoint, the request goes to the <code>us-east-1</code> Region. So the
         * signature calculations in Signature Version 4 must use <code>us-east-1</code> as
         * the Region, even if the location constraint in the request specifies another
         * Region where the bucket is to be created. If you create a bucket in a Region
         * other than US East (N. Virginia), your application must be able to handle 307
         * redirect. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/VirtualHosting.html">Virtual
         * hosting of buckets</a> in the <i>Amazon S3 User Guide</i>.</p> </li> <li> <p>
         * <b>Directory buckets </b> - For directory buckets, you must make requests for
         * this API operation to the Regional endpoint. These endpoints support path-style
         * requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General
         * purpose bucket permissions</b> - In addition to the <code>s3:CreateBucket</code>
         * permission, the following permissions are required in a policy when your
         * <code>CreateBucket</code> request includes specific headers: </p> <ul> <li> <p>
         * <b>Access control lists (ACLs)</b> - In your <code>CreateBucket</code> request,
         * if you specify an access control list (ACL) and set it to
         * <code>public-read</code>, <code>public-read-write</code>,
         * <code>authenticated-read</code>, or if you explicitly specify any other custom
         * ACLs, both <code>s3:CreateBucket</code> and <code>s3:PutBucketAcl</code>
         * permissions are required. In your <code>CreateBucket</code> request, if you set
         * the ACL to <code>private</code>, or if you don't specify any ACLs, only the
         * <code>s3:CreateBucket</code> permission is required. </p> </li> <li> <p>
         * <b>Object Lock</b> - In your <code>CreateBucket</code> request, if you set
         * <code>x-amz-bucket-object-lock-enabled</code> to true, the
         * <code>s3:PutBucketObjectLockConfiguration</code> and
         * <code>s3:PutBucketVersioning</code> permissions are required.</p> </li> <li> <p>
         * <b>S3 Object Ownership</b> - If your <code>CreateBucket</code> request includes
         * the <code>x-amz-object-ownership</code> header, then the
         * <code>s3:PutBucketOwnershipControls</code> permission is required.</p>
         *  <p> To set an ACL on a bucket as part of a <code>CreateBucket</code>
         * request, you must explicitly set S3 Object Ownership for the bucket to a
         * different value than the default, <code>BucketOwnerEnforced</code>.
         * Additionally, if your desired bucket ACL grants public access, you must first
         * create the bucket (without the bucket ACL) and then explicitly disable Block
         * Public Access on the bucket before using <code>PutBucketAcl</code> to set the
         * ACL. If you try to create a bucket with a public ACL, the request will fail.
         * </p> <p> For the majority of modern use cases in S3, we recommend that you keep
         * all Block Public Access settings enabled and keep ACLs disabled. If you would
         * like to share data with users outside of your account, you can use bucket
         * policies as needed. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html">Controlling
         * ownership of objects and disabling ACLs for your bucket </a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/access-control-block-public-access.html">Blocking
         * public access to your Amazon S3 storage </a> in the <i>Amazon S3 User Guide</i>.
         * </p>  </li> <li> <p> <b>S3 Block Public Access</b> - If your
         * specific use case requires granting public access to your S3 resources, you can
         * disable Block Public Access. Specifically, you can create a new bucket with
         * Block Public Access enabled, then separately call the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeletePublicAccessBlock.html">
         * <code>DeletePublicAccessBlock</code> </a> API. To use this operation, you must
         * have the <code>s3:PutBucketPublicAccessBlock</code> permission. For more
         * information about S3 Block Public Access, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/access-control-block-public-access.html">Blocking
         * public access to your Amazon S3 storage </a> in the <i>Amazon S3 User Guide</i>.
         * </p> </li> </ul> </li> <li> <p> <b>Directory bucket permissions</b> - You must
         * have the <code>s3express:CreateBucket</code> permission in an IAM identity-based
         * policy instead of a bucket policy. Cross-account access to this API operation
         * isn't supported. This operation can only be performed by the Amazon Web Services
         * account that owns the resource. For more information about directory bucket
         * policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p>  <p>The permissions for ACLs,
         * Object Lock, S3 Object Ownership, and S3 Block Public Access are not supported
         * for directory buckets. For directory buckets, all Block Public Access settings
         * are enabled at the bucket level and S3 Object Ownership is set to Bucket owner
         * enforced (ACLs disabled). These settings can't be modified. </p> <p>For more
         * information about permissions for creating and working with directory buckets,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-buckets-overview.html">Directory
         * buckets</a> in the <i>Amazon S3 User Guide</i>. For more information about
         * supported S3 features for directory buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-one-zone.html#s3-express-features">Features
         * of S3 Express One Zone</a> in the <i>Amazon S3 User Guide</i>.</p> 
         * </li> </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets
         * </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following operations are related to <code>CreateBucket</code>:</p> <ul>
         * <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucket.html">DeleteBucket</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CreateBucket">AWS API
         * Reference</a></p>
         */
        virtual Model::CreateBucketOutcome CreateBucket(const Model::CreateBucketRequest& request) const;

        /**
         * A Callable wrapper for CreateBucket that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateBucketRequestT = Model::CreateBucketRequest>
        Model::CreateBucketOutcomeCallable CreateBucketCallable(const CreateBucketRequestT& request) const
        {
            return SubmitCallable(&S3Client::CreateBucket, request);
        }

        /**
         * An Async wrapper for CreateBucket that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateBucketRequestT = Model::CreateBucketRequest>
        void CreateBucketAsync(const CreateBucketRequestT& request, const CreateBucketResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::CreateBucket, request, handler, context);
        }

        /**
         * <p>Creates a metadata table configuration for a general purpose bucket. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/metadata-tables-overview.html">Accelerating
         * data discovery with S3 Metadata</a> in the <i>Amazon S3 User Guide</i>. </p>
         * <dl> <dt>Permissions</dt> <dd> <p>To use this operation, you must have the
         * following permissions. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/metadata-tables-permissions.html">Setting
         * up permissions for configuring metadata tables</a> in the <i>Amazon S3 User
         * Guide</i>.</p> <p>If you also want to integrate your table bucket with Amazon
         * Web Services analytics services so that you can query your metadata table, you
         * need additional permissions. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-tables-integrating-aws.html">
         * Integrating Amazon S3 Tables with Amazon Web Services analytics services</a> in
         * the <i>Amazon S3 User Guide</i>.</p> <ul> <li> <p>
         * <code>s3:CreateBucketMetadataTableConfiguration</code> </p> </li> <li> <p>
         * <code>s3tables:CreateNamespace</code> </p> </li> <li> <p>
         * <code>s3tables:GetTable</code> </p> </li> <li> <p>
         * <code>s3tables:CreateTable</code> </p> </li> <li> <p>
         * <code>s3tables:PutTablePolicy</code> </p> </li> </ul> </dd> </dl> <p>The
         * following operations are related to
         * <code>CreateBucketMetadataTableConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketMetadataTableConfiguration.html">DeleteBucketMetadataTableConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketMetadataTableConfiguration.html">GetBucketMetadataTableConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CreateBucketMetadataTableConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateBucketMetadataTableConfigurationOutcome CreateBucketMetadataTableConfiguration(const Model::CreateBucketMetadataTableConfigurationRequest& request) const;

        /**
         * A Callable wrapper for CreateBucketMetadataTableConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateBucketMetadataTableConfigurationRequestT = Model::CreateBucketMetadataTableConfigurationRequest>
        Model::CreateBucketMetadataTableConfigurationOutcomeCallable CreateBucketMetadataTableConfigurationCallable(const CreateBucketMetadataTableConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::CreateBucketMetadataTableConfiguration, request);
        }

        /**
         * An Async wrapper for CreateBucketMetadataTableConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateBucketMetadataTableConfigurationRequestT = Model::CreateBucketMetadataTableConfigurationRequest>
        void CreateBucketMetadataTableConfigurationAsync(const CreateBucketMetadataTableConfigurationRequestT& request, const CreateBucketMetadataTableConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::CreateBucketMetadataTableConfiguration, request, handler, context);
        }

        /**
         * <p>This action initiates a multipart upload and returns an upload ID. This
         * upload ID is used to associate all of the parts in the specific multipart
         * upload. You specify this upload ID in each of your subsequent upload part
         * requests (see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>).
         * You also include this upload ID in the final request to either complete or abort
         * the multipart upload request. For more information about multipart uploads, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html">Multipart
         * Upload Overview</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>After you
         * initiate a multipart upload and upload one or more parts, to stop being charged
         * for storing the uploaded parts, you must either complete or abort the multipart
         * upload. Amazon S3 frees up the space used to store the parts and stops charging
         * you for storing them only after you either complete or abort a multipart upload.
         * </p>  <p>If you have configured a lifecycle rule to abort incomplete
         * multipart uploads, the created multipart upload must be completed within the
         * number of days specified in the bucket lifecycle configuration. Otherwise, the
         * incomplete multipart upload becomes eligible for an abort action and Amazon S3
         * aborts the multipart upload. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html#mpu-abort-incomplete-mpu-lifecycle-config">Aborting
         * Incomplete Multipart Uploads Using a Bucket Lifecycle Configuration</a>.</p>
         *  <ul> <li> <p> <b>Directory buckets </b> - S3 Lifecycle is not supported
         * by directory buckets.</p> </li> <li> <p> <b>Directory buckets </b> - For
         * directory buckets, you must make requests for this API operation to the Zonal
         * endpoint. These endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <dl> <dt>Request signing</dt> <dd> <p>For request signing,
         * multipart upload is just a series of regular requests. You initiate a multipart
         * upload, send one or more requests to upload parts, and then complete the
         * multipart upload process. You sign each request individually. There is nothing
         * special about signing multipart upload requests. For more information about
         * signing, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html">Authenticating
         * Requests (Amazon Web Services Signature Version 4)</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </dd> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose
         * bucket permissions</b> - To perform a multipart upload with encryption using an
         * Key Management Service (KMS) KMS key, the requester must have permission to the
         * <code>kms:Decrypt</code> and <code>kms:GenerateDataKey</code> actions on the
         * key. The requester must also have permissions for the
         * <code>kms:GenerateDataKey</code> action for the
         * <code>CreateMultipartUpload</code> API. Then, the requester needs permissions
         * for the <code>kms:Decrypt</code> action on the <code>UploadPart</code> and
         * <code>UploadPartCopy</code> APIs. These permissions are required because Amazon
         * S3 must decrypt and read data from the encrypted file parts before it completes
         * the multipart upload. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html#mpuAndPermissions">Multipart
         * upload API and permissions</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/UsingKMSEncryption.html">Protecting
         * data using server-side encryption with Amazon Web Services KMS</a> in the
         * <i>Amazon S3 User Guide</i>.</p> </li> <li> <p> <b>Directory bucket
         * permissions</b> - To grant access to this API operation on a directory bucket,
         * we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>Encryption</dt> <dd>
         * <ul> <li> <p> <b>General purpose buckets</b> - Server-side encryption is for
         * data encryption at rest. Amazon S3 encrypts your data as it writes it to disks
         * in its data centers and decrypts it when you access it. Amazon S3 automatically
         * encrypts all new objects that are uploaded to an S3 bucket. When doing a
         * multipart upload, if you don't specify encryption information in your request,
         * the encryption setting of the uploaded parts is set to the default encryption
         * configuration of the destination bucket. By default, all buckets have a base
         * level of encryption configuration that uses server-side encryption with Amazon
         * S3 managed keys (SSE-S3). If the destination bucket has a default encryption
         * configuration that uses server-side encryption with an Key Management Service
         * (KMS) key (SSE-KMS), or a customer-provided encryption key (SSE-C), Amazon S3
         * uses the corresponding KMS key, or a customer-provided key to encrypt the
         * uploaded parts. When you perform a CreateMultipartUpload operation, if you want
         * to use a different type of encryption setting for the uploaded parts, you can
         * request that Amazon S3 encrypts the object with a different encryption key (such
         * as an Amazon S3 managed key, a KMS key, or a customer-provided key). When the
         * encryption setting in your request is different from the default encryption
         * configuration of the destination bucket, the encryption setting in your request
         * takes precedence. If you choose to provide your own encryption key, the request
         * headers you provide in <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>
         * requests must match the headers you used in the
         * <code>CreateMultipartUpload</code> request.</p> <ul> <li> <p>Use KMS keys
         * (SSE-KMS) that include the Amazon Web Services managed key (<code>aws/s3</code>)
         * and KMS customer managed keys stored in Key Management Service (KMS) – If you
         * want Amazon Web Services to manage the keys used to encrypt data, specify the
         * following headers in the request.</p> <ul> <li> <p>
         * <code>x-amz-server-side-encryption</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-aws-kms-key-id</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-context</code> </p> </li> </ul>  <ul>
         * <li> <p>If you specify <code>x-amz-server-side-encryption:aws:kms</code>, but
         * don't provide <code>x-amz-server-side-encryption-aws-kms-key-id</code>, Amazon
         * S3 uses the Amazon Web Services managed key (<code>aws/s3</code> key) in KMS to
         * protect the data.</p> </li> <li> <p>To perform a multipart upload with
         * encryption by using an Amazon Web Services KMS key, the requester must have
         * permission to the <code>kms:Decrypt</code> and <code>kms:GenerateDataKey*</code>
         * actions on the key. These permissions are required because Amazon S3 must
         * decrypt and read data from the encrypted file parts before it completes the
         * multipart upload. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html#mpuAndPermissions">Multipart
         * upload API and permissions</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/UsingKMSEncryption.html">Protecting
         * data using server-side encryption with Amazon Web Services KMS</a> in the
         * <i>Amazon S3 User Guide</i>.</p> </li> <li> <p>If your Identity and Access
         * Management (IAM) user or role is in the same Amazon Web Services account as the
         * KMS key, then you must have these permissions on the key policy. If your IAM
         * user or role is in a different account from the key, then you must have the
         * permissions on both the key policy and your IAM user or role.</p> </li> <li>
         * <p>All <code>GET</code> and <code>PUT</code> requests for an object protected by
         * KMS fail if you don't make them by using Secure Sockets Layer (SSL), Transport
         * Layer Security (TLS), or Signature Version 4. For information about configuring
         * any of the officially supported Amazon Web Services SDKs and Amazon Web Services
         * CLI, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/UsingAWSSDK.html#specify-signature-version">Specifying
         * the Signature Version in Request Authentication</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </li> </ul>  <p>For more information about server-side
         * encryption with KMS keys (SSE-KMS), see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/UsingKMSEncryption.html">Protecting
         * Data Using Server-Side Encryption with KMS keys</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </li> <li> <p>Use customer-provided encryption keys (SSE-C) – If
         * you want to manage your own encryption keys, provide all the following headers
         * in the request.</p> <ul> <li> <p>
         * <code>x-amz-server-side-encryption-customer-algorithm</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-customer-key</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-customer-key-MD5</code> </p> </li> </ul>
         * <p>For more information about server-side encryption with customer-provided
         * encryption keys (SSE-C), see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/ServerSideEncryptionCustomerKeys.html">
         * Protecting data using server-side encryption with customer-provided encryption
         * keys (SSE-C)</a> in the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </li> <li>
         * <p> <b>Directory buckets</b> - For directory buckets, there are only two
         * supported options for server-side encryption: server-side encryption with Amazon
         * S3 managed keys (SSE-S3) (<code>AES256</code>) and server-side encryption with
         * KMS keys (SSE-KMS) (<code>aws:kms</code>). We recommend that the bucket's
         * default encryption uses the desired encryption configuration and you don't
         * override the bucket default encryption in your <code>CreateSession</code>
         * requests or <code>PUT</code> object requests. Then, new objects are
         * automatically encrypted with the desired encryption settings. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-serv-side-encryption.html">Protecting
         * data with server-side encryption</a> in the <i>Amazon S3 User Guide</i>. For
         * more information about the encryption overriding behaviors in directory buckets,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-specifying-kms-encryption.html">Specifying
         * server-side encryption with KMS for new object uploads</a>.</p> <p>In the Zonal
         * endpoint API calls (except <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>)
         * using the REST API, the encryption request headers must match the encryption
         * settings that are specified in the <code>CreateSession</code> request. You can't
         * override the values of the encryption settings
         * (<code>x-amz-server-side-encryption</code>,
         * <code>x-amz-server-side-encryption-aws-kms-key-id</code>,
         * <code>x-amz-server-side-encryption-context</code>, and
         * <code>x-amz-server-side-encryption-bucket-key-enabled</code>) that are specified
         * in the <code>CreateSession</code> request. You don't need to explicitly specify
         * these encryption settings values in Zonal endpoint API calls, and Amazon S3 will
         * use the encryption settings values from the <code>CreateSession</code> request
         * to protect new objects in the directory bucket. </p>  <p>When you use the
         * CLI or the Amazon Web Services SDKs, for <code>CreateSession</code>, the session
         * token refreshes automatically to avoid service interruptions when a session
         * expires. The CLI or the Amazon Web Services SDKs use the bucket's default
         * encryption configuration for the <code>CreateSession</code> request. It's not
         * supported to override the encryption settings values in the
         * <code>CreateSession</code> request. So in the Zonal endpoint API calls (except
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>),
         * the encryption request headers must match the default encryption configuration
         * of the directory bucket. </p>   <p>For directory buckets, when you
         * perform a <code>CreateMultipartUpload</code> operation and an
         * <code>UploadPartCopy</code> operation, the request headers you provide in the
         * <code>CreateMultipartUpload</code> request must match the default encryption
         * configuration of the destination bucket. </p>  </li> </ul> </dd> <dt>HTTP
         * Host header syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host
         * header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>CreateMultipartUpload</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListMultipartUploads.html">ListMultipartUploads</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CreateMultipartUpload">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateMultipartUploadOutcome CreateMultipartUpload(const Model::CreateMultipartUploadRequest& request) const;

        /**
         * A Callable wrapper for CreateMultipartUpload that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateMultipartUploadRequestT = Model::CreateMultipartUploadRequest>
        Model::CreateMultipartUploadOutcomeCallable CreateMultipartUploadCallable(const CreateMultipartUploadRequestT& request) const
        {
            return SubmitCallable(&S3Client::CreateMultipartUpload, request);
        }

        /**
         * An Async wrapper for CreateMultipartUpload that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateMultipartUploadRequestT = Model::CreateMultipartUploadRequest>
        void CreateMultipartUploadAsync(const CreateMultipartUploadRequestT& request, const CreateMultipartUploadResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::CreateMultipartUpload, request, handler, context);
        }

        /**
         * <p>Creates a session that establishes temporary security credentials to support
         * fast authentication and authorization for the Zonal endpoint API operations on
         * directory buckets. For more information about Zonal endpoint API operations that
         * include the Availability Zone in the request endpoint, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-APIs.html">S3
         * Express One Zone APIs</a> in the <i>Amazon S3 User Guide</i>. </p> <p>To make
         * Zonal endpoint API requests on a directory bucket, use the
         * <code>CreateSession</code> API operation. Specifically, you grant
         * <code>s3express:CreateSession</code> permission to a bucket in a bucket policy
         * or an IAM identity-based policy. Then, you use IAM credentials to make the
         * <code>CreateSession</code> API request on the bucket, which returns temporary
         * security credentials that include the access key ID, secret access key, session
         * token, and expiration. These credentials have associated permissions to access
         * the Zonal endpoint API operations. After the session is created, you don’t need
         * to use other policies to grant permissions to each Zonal endpoint API
         * individually. Instead, in your Zonal endpoint API requests, you sign your
         * requests by applying the temporary security credentials of the session to the
         * request headers and following the SigV4 protocol for authentication. You also
         * apply the session token to the <code>x-amz-s3session-token</code> request header
         * for authorization. Temporary security credentials are scoped to the bucket and
         * expire after 5 minutes. After the expiration time, any calls that you make with
         * those credentials will fail. You must use IAM credentials again to make a
         * <code>CreateSession</code> API request that generates a new set of temporary
         * credentials for use. Temporary credentials cannot be extended or refreshed
         * beyond the original specified interval.</p> <p>If you use Amazon Web Services
         * SDKs, SDKs handle the session token refreshes automatically to avoid service
         * interruptions when a session expires. We recommend that you use the Amazon Web
         * Services SDKs to initiate and manage requests to the CreateSession API. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-optimizing-performance-guidelines-design-patterns.html#s3-express-optimizing-performance-session-authentication">Performance
         * guidelines and design patterns</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <ul> <li> <p>You must make requests for this API operation to the Zonal
         * endpoint. These endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.
         * Path-style requests are not supported. For more information about endpoints in
         * Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> <li> <p> <b> <code>CopyObject</code> API operation</b> - Unlike other
         * Zonal endpoint API operations, the <code>CopyObject</code> API operation doesn't
         * use the temporary security credentials returned from the
         * <code>CreateSession</code> API operation for authentication and authorization.
         * For information about authentication and authorization of the
         * <code>CopyObject</code> API operation on directory buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>.</p>
         * </li> <li> <p> <b> <code>HeadBucket</code> API operation</b> - Unlike other
         * Zonal endpoint API operations, the <code>HeadBucket</code> API operation doesn't
         * use the temporary security credentials returned from the
         * <code>CreateSession</code> API operation for authentication and authorization.
         * For information about authentication and authorization of the
         * <code>HeadBucket</code> API operation on directory buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadBucket.html">HeadBucket</a>.</p>
         * </li> </ul>  <dl> <dt>Permissions</dt> <dd> <p>To obtain temporary
         * security credentials, you must create a bucket policy or an IAM identity-based
         * policy that grants <code>s3express:CreateSession</code> permission to the
         * bucket. In a policy, you can have the <code>s3express:SessionMode</code>
         * condition key to control who can create a <code>ReadWrite</code> or
         * <code>ReadOnly</code> session. For more information about <code>ReadWrite</code>
         * or <code>ReadOnly</code> sessions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html#API_CreateSession_RequestParameters">
         * <code>x-amz-create-session-mode</code> </a>. For example policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-example-bucket-policies.html">Example
         * bucket policies for S3 Express One Zone</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-identity-policies.html">Amazon
         * Web Services Identity and Access Management (IAM) identity-based policies for S3
         * Express One Zone</a> in the <i>Amazon S3 User Guide</i>. </p> <p>To grant
         * cross-account access to Zonal endpoint API operations, the bucket policy should
         * also grant both accounts the <code>s3express:CreateSession</code>
         * permission.</p> <p>If you want to encrypt objects with SSE-KMS, you must also
         * have the <code>kms:GenerateDataKey</code> and the <code>kms:Decrypt</code>
         * permissions in IAM identity-based policies and KMS key policies for the target
         * KMS key.</p> </dd> <dt>Encryption</dt> <dd> <p>For directory buckets, there are
         * only two supported options for server-side encryption: server-side encryption
         * with Amazon S3 managed keys (SSE-S3) (<code>AES256</code>) and server-side
         * encryption with KMS keys (SSE-KMS) (<code>aws:kms</code>). We recommend that the
         * bucket's default encryption uses the desired encryption configuration and you
         * don't override the bucket default encryption in your <code>CreateSession</code>
         * requests or <code>PUT</code> object requests. Then, new objects are
         * automatically encrypted with the desired encryption settings. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-serv-side-encryption.html">Protecting
         * data with server-side encryption</a> in the <i>Amazon S3 User Guide</i>. For
         * more information about the encryption overriding behaviors in directory buckets,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-specifying-kms-encryption.html">Specifying
         * server-side encryption with KMS for new object uploads</a>.</p> <p>For <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-differences.html#s3-express-differences-api-operations">Zonal
         * endpoint (object-level) API operations</a> except <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>,
         * you authenticate and authorize requests through <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">CreateSession</a>
         * for low latency. To encrypt new objects in a directory bucket with SSE-KMS, you
         * must specify SSE-KMS as the directory bucket's default encryption configuration
         * with a KMS key (specifically, a <a
         * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">customer
         * managed key</a>). Then, when a session is created for Zonal endpoint API
         * operations, new objects are automatically encrypted and decrypted with SSE-KMS
         * and S3 Bucket Keys during the session.</p>  <p> Only 1 <a
         * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">customer
         * managed key</a> is supported per directory bucket for the lifetime of the
         * bucket. The <a
         * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-managed-cmk">Amazon
         * Web Services managed key</a> (<code>aws/s3</code>) isn't supported. After you
         * specify SSE-KMS as your bucket's default encryption configuration with a
         * customer managed key, you can't change the customer managed key for the bucket's
         * SSE-KMS configuration. </p>  <p>In the Zonal endpoint API calls (except
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>)
         * using the REST API, you can't override the values of the encryption settings
         * (<code>x-amz-server-side-encryption</code>,
         * <code>x-amz-server-side-encryption-aws-kms-key-id</code>,
         * <code>x-amz-server-side-encryption-context</code>, and
         * <code>x-amz-server-side-encryption-bucket-key-enabled</code>) from the
         * <code>CreateSession</code> request. You don't need to explicitly specify these
         * encryption settings values in Zonal endpoint API calls, and Amazon S3 will use
         * the encryption settings values from the <code>CreateSession</code> request to
         * protect new objects in the directory bucket. </p>  <p>When you use the CLI
         * or the Amazon Web Services SDKs, for <code>CreateSession</code>, the session
         * token refreshes automatically to avoid service interruptions when a session
         * expires. The CLI or the Amazon Web Services SDKs use the bucket's default
         * encryption configuration for the <code>CreateSession</code> request. It's not
         * supported to override the encryption settings values in the
         * <code>CreateSession</code> request. Also, in the Zonal endpoint API calls
         * (except <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>),
         * it's not supported to override the values of the encryption settings from the
         * <code>CreateSession</code> request. </p>  </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CreateSession">AWS
         * API Reference</a></p>
         */
        virtual Model::CreateSessionOutcome CreateSession(const Model::CreateSessionRequest& request) const;

        /**
         * A Callable wrapper for CreateSession that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename CreateSessionRequestT = Model::CreateSessionRequest>
        Model::CreateSessionOutcomeCallable CreateSessionCallable(const CreateSessionRequestT& request) const
        {
            return SubmitCallable(&S3Client::CreateSession, request);
        }

        /**
         * An Async wrapper for CreateSession that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename CreateSessionRequestT = Model::CreateSessionRequest>
        void CreateSessionAsync(const CreateSessionRequestT& request, const CreateSessionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::CreateSession, request, handler, context);
        }

        /**
         * <p>Deletes the S3 bucket. All objects (including all object versions and delete
         * markers) in the bucket must be deleted before the bucket itself can be
         * deleted.</p>  <ul> <li> <p> <b>Directory buckets</b> - If multipart
         * uploads in a directory bucket are in progress, you can't delete the bucket until
         * all the in-progress multipart uploads are aborted or completed.</p> </li> <li>
         * <p> <b>Directory buckets </b> - For directory buckets, you must make requests
         * for this API operation to the Regional endpoint. These endpoints support
         * path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General
         * purpose bucket permissions</b> - You must have the <code>s3:DeleteBucket</code>
         * permission on the specified bucket in a policy.</p> </li> <li> <p> <b>Directory
         * bucket permissions</b> - You must have the <code>s3express:DeleteBucket</code>
         * permission in an IAM identity-based policy instead of a bucket policy.
         * Cross-account access to this API operation isn't supported. This operation can
         * only be performed by the Amazon Web Services account that owns the resource. For
         * more information about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following operations are related to <code>DeleteBucket</code>:</p> <ul>
         * <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html">DeleteObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucket">AWS API
         * Reference</a></p>
         */
        virtual Model::DeleteBucketOutcome DeleteBucket(const Model::DeleteBucketRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucket that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketRequestT = Model::DeleteBucketRequest>
        Model::DeleteBucketOutcomeCallable DeleteBucketCallable(const DeleteBucketRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucket, request);
        }

        /**
         * An Async wrapper for DeleteBucket that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketRequestT = Model::DeleteBucketRequest>
        void DeleteBucketAsync(const DeleteBucketRequestT& request, const DeleteBucketResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucket, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Deletes an analytics configuration for the bucket (specified by the analytics
         * configuration ID).</p> <p>To use this operation, you must have permissions to
         * perform the <code>s3:PutAnalyticsConfiguration</code> action. The bucket owner
         * has this permission by default. The bucket owner can grant this permission to
         * others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * the Amazon S3 analytics feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/analytics-storage-class.html">Amazon
         * S3 Analytics – Storage Class Analysis</a>. </p> <p>The following operations are
         * related to <code>DeleteBucketAnalyticsConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketAnalyticsConfiguration.html">GetBucketAnalyticsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketAnalyticsConfigurations.html">ListBucketAnalyticsConfigurations</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketAnalyticsConfiguration.html">PutBucketAnalyticsConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketAnalyticsConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketAnalyticsConfigurationOutcome DeleteBucketAnalyticsConfiguration(const Model::DeleteBucketAnalyticsConfigurationRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketAnalyticsConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketAnalyticsConfigurationRequestT = Model::DeleteBucketAnalyticsConfigurationRequest>
        Model::DeleteBucketAnalyticsConfigurationOutcomeCallable DeleteBucketAnalyticsConfigurationCallable(const DeleteBucketAnalyticsConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketAnalyticsConfiguration, request);
        }

        /**
         * An Async wrapper for DeleteBucketAnalyticsConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketAnalyticsConfigurationRequestT = Model::DeleteBucketAnalyticsConfigurationRequest>
        void DeleteBucketAnalyticsConfigurationAsync(const DeleteBucketAnalyticsConfigurationRequestT& request, const DeleteBucketAnalyticsConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketAnalyticsConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Deletes the <code>cors</code> configuration information set for the
         * bucket.</p> <p>To use this operation, you must have permission to perform the
         * <code>s3:PutBucketCORS</code> action. The bucket owner has this permission by
         * default and can grant this permission to others. </p> <p>For information about
         * <code>cors</code>, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cors.html">Enabling
         * Cross-Origin Resource Sharing</a> in the <i>Amazon S3 User Guide</i>.</p> <p
         * class="title"> <b>Related Resources</b> </p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketCors.html">PutBucketCors</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTOPTIONSobject.html">RESTOPTIONSobject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketCors">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketCorsOutcome DeleteBucketCors(const Model::DeleteBucketCorsRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketCors that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketCorsRequestT = Model::DeleteBucketCorsRequest>
        Model::DeleteBucketCorsOutcomeCallable DeleteBucketCorsCallable(const DeleteBucketCorsRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketCors, request);
        }

        /**
         * An Async wrapper for DeleteBucketCors that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketCorsRequestT = Model::DeleteBucketCorsRequest>
        void DeleteBucketCorsAsync(const DeleteBucketCorsRequestT& request, const DeleteBucketCorsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketCors, request, handler, context);
        }

        /**
         * <p>This implementation of the DELETE action resets the default encryption for
         * the bucket as server-side encryption with Amazon S3 managed keys (SSE-S3).</p>
         *  <ul> <li> <p> <b>General purpose buckets</b> - For information about the
         * bucket default encryption feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-encryption.html">Amazon
         * S3 Bucket Default Encryption</a> in the <i>Amazon S3 User Guide</i>.</p> </li>
         * <li> <p> <b>Directory buckets</b> - For directory buckets, there are only two
         * supported options for server-side encryption: SSE-S3 and SSE-KMS. For
         * information about the default encryption configuration in directory buckets, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-bucket-encryption.html">Setting
         * default server-side encryption behavior for directory buckets</a>.</p> </li>
         * </ul>  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose
         * bucket permissions</b> - The <code>s3:PutEncryptionConfiguration</code>
         * permission is required in a policy. The bucket owner has this permission by
         * default. The bucket owner can grant this permission to others. For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> </li> <li> <p>
         * <b>Directory bucket permissions</b> - To grant access to this API operation, you
         * must have the <code>s3express:PutEncryptionConfiguration</code> permission in an
         * IAM identity-based policy instead of a bucket policy. Cross-account access to
         * this API operation isn't supported. This operation can only be performed by the
         * Amazon Web Services account that owns the resource. For more information about
         * directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following operations are related to
         * <code>DeleteBucketEncryption</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketEncryption.html">PutBucketEncryption</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketEncryption.html">GetBucketEncryption</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketEncryption">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketEncryptionOutcome DeleteBucketEncryption(const Model::DeleteBucketEncryptionRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketEncryption that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketEncryptionRequestT = Model::DeleteBucketEncryptionRequest>
        Model::DeleteBucketEncryptionOutcomeCallable DeleteBucketEncryptionCallable(const DeleteBucketEncryptionRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketEncryption, request);
        }

        /**
         * An Async wrapper for DeleteBucketEncryption that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketEncryptionRequestT = Model::DeleteBucketEncryptionRequest>
        void DeleteBucketEncryptionAsync(const DeleteBucketEncryptionRequestT& request, const DeleteBucketEncryptionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketEncryption, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Deletes the S3 Intelligent-Tiering configuration from the specified
         * bucket.</p> <p>The S3 Intelligent-Tiering storage class is designed to optimize
         * storage costs by automatically moving data to the most cost-effective storage
         * access tier, without performance impact or operational overhead. S3
         * Intelligent-Tiering delivers automatic cost savings in three low latency and
         * high throughput access tiers. To get the lowest storage cost on data that can be
         * accessed in minutes to hours, you can choose to activate additional archiving
         * capabilities.</p> <p>The S3 Intelligent-Tiering storage class is the ideal
         * storage class for data with unknown, changing, or unpredictable access patterns,
         * independent of object size or retention period. If the size of an object is less
         * than 128 KB, it is not monitored and not eligible for auto-tiering. Smaller
         * objects can be stored, but they are always charged at the Frequent Access tier
         * rates in the S3 Intelligent-Tiering storage class.</p> <p>For more information,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html#sc-dynamic-data-access">Storage
         * class for automatically optimizing frequently and infrequently accessed
         * objects</a>.</p> <p>Operations related to
         * <code>DeleteBucketIntelligentTieringConfiguration</code> include: </p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketIntelligentTieringConfiguration.html">GetBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketIntelligentTieringConfiguration.html">PutBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketIntelligentTieringConfigurations.html">ListBucketIntelligentTieringConfigurations</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketIntelligentTieringConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketIntelligentTieringConfigurationOutcome DeleteBucketIntelligentTieringConfiguration(const Model::DeleteBucketIntelligentTieringConfigurationRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketIntelligentTieringConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketIntelligentTieringConfigurationRequestT = Model::DeleteBucketIntelligentTieringConfigurationRequest>
        Model::DeleteBucketIntelligentTieringConfigurationOutcomeCallable DeleteBucketIntelligentTieringConfigurationCallable(const DeleteBucketIntelligentTieringConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketIntelligentTieringConfiguration, request);
        }

        /**
         * An Async wrapper for DeleteBucketIntelligentTieringConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketIntelligentTieringConfigurationRequestT = Model::DeleteBucketIntelligentTieringConfigurationRequest>
        void DeleteBucketIntelligentTieringConfigurationAsync(const DeleteBucketIntelligentTieringConfigurationRequestT& request, const DeleteBucketIntelligentTieringConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketIntelligentTieringConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Deletes an inventory configuration (identified by the inventory ID) from the
         * bucket.</p> <p>To use this operation, you must have permissions to perform the
         * <code>s3:PutInventoryConfiguration</code> action. The bucket owner has this
         * permission by default. The bucket owner can grant this permission to others. For
         * more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * the Amazon S3 inventory feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-inventory.html">Amazon
         * S3 Inventory</a>.</p> <p>Operations related to
         * <code>DeleteBucketInventoryConfiguration</code> include: </p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketInventoryConfiguration.html">GetBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketInventoryConfiguration.html">PutBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketInventoryConfigurations.html">ListBucketInventoryConfigurations</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketInventoryConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketInventoryConfigurationOutcome DeleteBucketInventoryConfiguration(const Model::DeleteBucketInventoryConfigurationRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketInventoryConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketInventoryConfigurationRequestT = Model::DeleteBucketInventoryConfigurationRequest>
        Model::DeleteBucketInventoryConfigurationOutcomeCallable DeleteBucketInventoryConfigurationCallable(const DeleteBucketInventoryConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketInventoryConfiguration, request);
        }

        /**
         * An Async wrapper for DeleteBucketInventoryConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketInventoryConfigurationRequestT = Model::DeleteBucketInventoryConfigurationRequest>
        void DeleteBucketInventoryConfigurationAsync(const DeleteBucketInventoryConfigurationRequestT& request, const DeleteBucketInventoryConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketInventoryConfiguration, request, handler, context);
        }

        /**
         * <p>Deletes the lifecycle configuration from the specified bucket. Amazon S3
         * removes all the lifecycle configuration rules in the lifecycle subresource
         * associated with the bucket. Your objects never expire, and Amazon S3 no longer
         * automatically deletes any objects on the basis of rules contained in the deleted
         * lifecycle configuration.</p> <dl> <dt>Permissions</dt> <dd> <ul> <li> <p>
         * <b>General purpose bucket permissions</b> - By default, all Amazon S3 resources
         * are private, including buckets, objects, and related subresources (for example,
         * lifecycle configuration and website configuration). Only the resource owner
         * (that is, the Amazon Web Services account that created it) can access the
         * resource. The resource owner can optionally grant access permissions to others
         * by writing an access policy. For this operation, a user must have the
         * <code>s3:PutLifecycleConfiguration</code> permission.</p> <p>For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> </li> </ul> <ul> <li>
         * <p> <b>Directory bucket permissions</b> - You must have the
         * <code>s3express:PutLifecycleConfiguration</code> permission in an IAM
         * identity-based policy to use this operation. Cross-account access to this API
         * operation isn't supported. The resource owner can optionally grant access
         * permissions to others by creating a role or user for them as long as they are
         * within the same account as the owner and resource.</p> <p>For more information
         * about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Authorizing
         * Regional endpoint APIs with IAM</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p> <b>Directory buckets </b> - For directory buckets, you must make
         * requests for this API operation to the Regional endpoint. These endpoints
         * support path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  </li> </ul> </dd> </dl> <dl> <dt>HTTP Host header syntax</dt> <dd> <p>
         * <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>For more information about the object expiration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/intro-lifecycle-rules.html#intro-lifecycle-rules-actions">Elements
         * to Describe Lifecycle Actions</a>.</p> <p>Related actions include:</p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html">PutBucketLifecycleConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLifecycleConfiguration.html">GetBucketLifecycleConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketLifecycle">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketLifecycleOutcome DeleteBucketLifecycle(const Model::DeleteBucketLifecycleRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketLifecycle that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketLifecycleRequestT = Model::DeleteBucketLifecycleRequest>
        Model::DeleteBucketLifecycleOutcomeCallable DeleteBucketLifecycleCallable(const DeleteBucketLifecycleRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketLifecycle, request);
        }

        /**
         * An Async wrapper for DeleteBucketLifecycle that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketLifecycleRequestT = Model::DeleteBucketLifecycleRequest>
        void DeleteBucketLifecycleAsync(const DeleteBucketLifecycleRequestT& request, const DeleteBucketLifecycleResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketLifecycle, request, handler, context);
        }

        /**
         * <p> Deletes a metadata table configuration from a general purpose bucket. For
         * more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/metadata-tables-overview.html">Accelerating
         * data discovery with S3 Metadata</a> in the <i>Amazon S3 User Guide</i>. </p>
         * <dl> <dt>Permissions</dt> <dd> <p>To use this operation, you must have the
         * <code>s3:DeleteBucketMetadataTableConfiguration</code> permission. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/metadata-tables-permissions.html">Setting
         * up permissions for configuring metadata tables</a> in the <i>Amazon S3 User
         * Guide</i>. </p> </dd> </dl> <p>The following operations are related to
         * <code>DeleteBucketMetadataTableConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucketMetadataTableConfiguration.html">CreateBucketMetadataTableConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketMetadataTableConfiguration.html">GetBucketMetadataTableConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketMetadataTableConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketMetadataTableConfigurationOutcome DeleteBucketMetadataTableConfiguration(const Model::DeleteBucketMetadataTableConfigurationRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketMetadataTableConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketMetadataTableConfigurationRequestT = Model::DeleteBucketMetadataTableConfigurationRequest>
        Model::DeleteBucketMetadataTableConfigurationOutcomeCallable DeleteBucketMetadataTableConfigurationCallable(const DeleteBucketMetadataTableConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketMetadataTableConfiguration, request);
        }

        /**
         * An Async wrapper for DeleteBucketMetadataTableConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketMetadataTableConfigurationRequestT = Model::DeleteBucketMetadataTableConfigurationRequest>
        void DeleteBucketMetadataTableConfigurationAsync(const DeleteBucketMetadataTableConfigurationRequestT& request, const DeleteBucketMetadataTableConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketMetadataTableConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Deletes a metrics configuration for the Amazon CloudWatch request metrics
         * (specified by the metrics configuration ID) from the bucket. Note that this
         * doesn't include the daily storage metrics.</p> <p> To use this operation, you
         * must have permissions to perform the <code>s3:PutMetricsConfiguration</code>
         * action. The bucket owner has this permission by default. The bucket owner can
         * grant this permission to others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * CloudWatch request metrics for Amazon S3, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cloudwatch-monitoring.html">Monitoring
         * Metrics with Amazon CloudWatch</a>. </p> <p>The following operations are related
         * to <code>DeleteBucketMetricsConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketMetricsConfiguration.html">GetBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketMetricsConfiguration.html">PutBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketMetricsConfigurations.html">ListBucketMetricsConfigurations</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cloudwatch-monitoring.html">Monitoring
         * Metrics with Amazon CloudWatch</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketMetricsConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketMetricsConfigurationOutcome DeleteBucketMetricsConfiguration(const Model::DeleteBucketMetricsConfigurationRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketMetricsConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketMetricsConfigurationRequestT = Model::DeleteBucketMetricsConfigurationRequest>
        Model::DeleteBucketMetricsConfigurationOutcomeCallable DeleteBucketMetricsConfigurationCallable(const DeleteBucketMetricsConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketMetricsConfiguration, request);
        }

        /**
         * An Async wrapper for DeleteBucketMetricsConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketMetricsConfigurationRequestT = Model::DeleteBucketMetricsConfigurationRequest>
        void DeleteBucketMetricsConfigurationAsync(const DeleteBucketMetricsConfigurationRequestT& request, const DeleteBucketMetricsConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketMetricsConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Removes <code>OwnershipControls</code> for an Amazon S3 bucket. To use this
         * operation, you must have the <code>s3:PutBucketOwnershipControls</code>
         * permission. For more information about Amazon S3 permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a>.</p> <p>For information about Amazon S3 Object
         * Ownership, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/about-object-ownership.html">Using
         * Object Ownership</a>. </p> <p>The following operations are related to
         * <code>DeleteBucketOwnershipControls</code>:</p> <ul> <li> <p>
         * <a>GetBucketOwnershipControls</a> </p> </li> <li> <p>
         * <a>PutBucketOwnershipControls</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketOwnershipControls">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketOwnershipControlsOutcome DeleteBucketOwnershipControls(const Model::DeleteBucketOwnershipControlsRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketOwnershipControls that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketOwnershipControlsRequestT = Model::DeleteBucketOwnershipControlsRequest>
        Model::DeleteBucketOwnershipControlsOutcomeCallable DeleteBucketOwnershipControlsCallable(const DeleteBucketOwnershipControlsRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketOwnershipControls, request);
        }

        /**
         * An Async wrapper for DeleteBucketOwnershipControls that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketOwnershipControlsRequestT = Model::DeleteBucketOwnershipControlsRequest>
        void DeleteBucketOwnershipControlsAsync(const DeleteBucketOwnershipControlsRequestT& request, const DeleteBucketOwnershipControlsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketOwnershipControls, request, handler, context);
        }

        /**
         * <p>Deletes the policy of a specified bucket.</p>  <p> <b>Directory buckets
         * </b> - For directory buckets, you must make requests for this API operation to
         * the Regional endpoint. These endpoints support path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <p>If you are using an identity other
         * than the root user of the Amazon Web Services account that owns the bucket, the
         * calling identity must both have the <code>DeleteBucketPolicy</code> permissions
         * on the specified bucket and belong to the bucket owner's account in order to use
         * this operation.</p> <p>If you don't have <code>DeleteBucketPolicy</code>
         * permissions, Amazon S3 returns a <code>403 Access Denied</code> error. If you
         * have the correct permissions, but you're not using an identity that belongs to
         * the bucket owner's account, Amazon S3 returns a <code>405 Method Not
         * Allowed</code> error.</p>  <p>To ensure that bucket owners don't
         * inadvertently lock themselves out of their own buckets, the root principal in a
         * bucket owner's Amazon Web Services account can perform the
         * <code>GetBucketPolicy</code>, <code>PutBucketPolicy</code>, and
         * <code>DeleteBucketPolicy</code> API actions, even if their bucket policy
         * explicitly denies the root principal's access. Bucket owner root principals can
         * only be blocked from performing these API actions by VPC endpoint policies and
         * Amazon Web Services Organizations policies.</p>  <ul> <li> <p>
         * <b>General purpose bucket permissions</b> - The
         * <code>s3:DeleteBucketPolicy</code> permission is required in a policy. For more
         * information about general purpose buckets bucket policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-iam-policies.html">Using
         * Bucket Policies and User Policies</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> <li> <p> <b>Directory bucket permissions</b> - To grant access to this API
         * operation, you must have the <code>s3express:DeleteBucketPolicy</code>
         * permission in an IAM identity-based policy instead of a bucket policy.
         * Cross-account access to this API operation isn't supported. This operation can
         * only be performed by the Amazon Web Services account that owns the resource. For
         * more information about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following operations are related to <code>DeleteBucketPolicy</code> </p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html">DeleteObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketPolicy">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketPolicyOutcome DeleteBucketPolicy(const Model::DeleteBucketPolicyRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketPolicy that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketPolicyRequestT = Model::DeleteBucketPolicyRequest>
        Model::DeleteBucketPolicyOutcomeCallable DeleteBucketPolicyCallable(const DeleteBucketPolicyRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketPolicy, request);
        }

        /**
         * An Async wrapper for DeleteBucketPolicy that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketPolicyRequestT = Model::DeleteBucketPolicyRequest>
        void DeleteBucketPolicyAsync(const DeleteBucketPolicyRequestT& request, const DeleteBucketPolicyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketPolicy, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p>  <p>
         * Deletes the replication configuration from the bucket.</p> <p>To use this
         * operation, you must have permissions to perform the
         * <code>s3:PutReplicationConfiguration</code> action. The bucket owner has these
         * permissions by default and can grant it to others. For more information about
         * permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>. </p>  <p>It can take a
         * while for the deletion of a replication configuration to fully propagate.</p>
         *  <p> For information about replication configuration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/replication.html">Replication</a>
         * in the <i>Amazon S3 User Guide</i>.</p> <p>The following operations are related
         * to <code>DeleteBucketReplication</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketReplication.html">PutBucketReplication</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketReplication.html">GetBucketReplication</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketReplication">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketReplicationOutcome DeleteBucketReplication(const Model::DeleteBucketReplicationRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketReplication that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketReplicationRequestT = Model::DeleteBucketReplicationRequest>
        Model::DeleteBucketReplicationOutcomeCallable DeleteBucketReplicationCallable(const DeleteBucketReplicationRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketReplication, request);
        }

        /**
         * An Async wrapper for DeleteBucketReplication that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketReplicationRequestT = Model::DeleteBucketReplicationRequest>
        void DeleteBucketReplicationAsync(const DeleteBucketReplicationRequestT& request, const DeleteBucketReplicationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketReplication, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Deletes the tags from the bucket.</p> <p>To use this operation, you must have
         * permission to perform the <code>s3:PutBucketTagging</code> action. By default,
         * the bucket owner has this permission and can grant this permission to others.
         * </p> <p>The following operations are related to
         * <code>DeleteBucketTagging</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketTagging.html">GetBucketTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketTagging.html">PutBucketTagging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketTagging">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketTaggingOutcome DeleteBucketTagging(const Model::DeleteBucketTaggingRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketTagging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketTaggingRequestT = Model::DeleteBucketTaggingRequest>
        Model::DeleteBucketTaggingOutcomeCallable DeleteBucketTaggingCallable(const DeleteBucketTaggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketTagging, request);
        }

        /**
         * An Async wrapper for DeleteBucketTagging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketTaggingRequestT = Model::DeleteBucketTaggingRequest>
        void DeleteBucketTaggingAsync(const DeleteBucketTaggingRequestT& request, const DeleteBucketTaggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketTagging, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>This action removes the website configuration for a bucket. Amazon S3 returns
         * a <code>200 OK</code> response upon successfully deleting a website
         * configuration on the specified bucket. You will get a <code>200 OK</code>
         * response if the website configuration you are trying to delete does not exist on
         * the bucket. Amazon S3 returns a <code>404</code> response if the bucket
         * specified in the request does not exist.</p> <p>This DELETE action requires the
         * <code>S3:DeleteBucketWebsite</code> permission. By default, only the bucket
         * owner can delete the website configuration attached to a bucket. However, bucket
         * owners can grant other users permission to delete the website configuration by
         * writing a bucket policy granting them the <code>S3:DeleteBucketWebsite</code>
         * permission. </p> <p>For more information about hosting websites, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/WebsiteHosting.html">Hosting
         * Websites on Amazon S3</a>. </p> <p>The following operations are related to
         * <code>DeleteBucketWebsite</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketWebsite.html">GetBucketWebsite</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketWebsite.html">PutBucketWebsite</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteBucketWebsite">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteBucketWebsiteOutcome DeleteBucketWebsite(const Model::DeleteBucketWebsiteRequest& request) const;

        /**
         * A Callable wrapper for DeleteBucketWebsite that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteBucketWebsiteRequestT = Model::DeleteBucketWebsiteRequest>
        Model::DeleteBucketWebsiteOutcomeCallable DeleteBucketWebsiteCallable(const DeleteBucketWebsiteRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteBucketWebsite, request);
        }

        /**
         * An Async wrapper for DeleteBucketWebsite that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteBucketWebsiteRequestT = Model::DeleteBucketWebsiteRequest>
        void DeleteBucketWebsiteAsync(const DeleteBucketWebsiteRequestT& request, const DeleteBucketWebsiteResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteBucketWebsite, request, handler, context);
        }

        /**
         * <p>Removes an object from a bucket. The behavior depends on the bucket's
         * versioning state: </p> <ul> <li> <p>If bucket versioning is not enabled, the
         * operation permanently deletes the object.</p> </li> <li> <p>If bucket versioning
         * is enabled, the operation inserts a delete marker, which becomes the current
         * version of the object. To permanently delete an object in a versioned bucket,
         * you must include the object’s <code>versionId</code> in the request. For more
         * information about versioning-enabled buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/DeletingObjectVersions.html">Deleting
         * object versions from a versioning-enabled bucket</a>.</p> </li> <li> <p>If
         * bucket versioning is suspended, the operation removes the object that has a null
         * <code>versionId</code>, if there is one, and inserts a delete marker that
         * becomes the current version of the object. If there isn't an object with a null
         * <code>versionId</code>, and all versions of the object have a
         * <code>versionId</code>, Amazon S3 does not remove the object and only inserts a
         * delete marker. To permanently delete an object that has a
         * <code>versionId</code>, you must include the object’s <code>versionId</code> in
         * the request. For more information about versioning-suspended buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/DeletingObjectsfromVersioningSuspendedBuckets.html">Deleting
         * objects from versioning-suspended buckets</a>.</p> </li> </ul>  <ul> <li>
         * <p> <b>Directory buckets</b> - S3 Versioning isn't enabled and supported for
         * directory buckets. For this API operation, only the <code>null</code> value of
         * the version ID is supported by directory buckets. You can only specify
         * <code>null</code> to the <code>versionId</code> query parameter in the
         * request.</p> </li> <li> <p> <b>Directory buckets</b> - For directory buckets,
         * you must make requests for this API operation to the Zonal endpoint. These
         * endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <p>To remove a specific version, you must use the
         * <code>versionId</code> query parameter. Using this query parameter permanently
         * deletes the version. If the object deleted is a delete marker, Amazon S3 sets
         * the response header <code>x-amz-delete-marker</code> to true. </p> <p>If the
         * object you want to delete is in a bucket where the bucket versioning
         * configuration is MFA Delete enabled, you must include the <code>x-amz-mfa</code>
         * request header in the DELETE <code>versionId</code> request. Requests that
         * include <code>x-amz-mfa</code> must use HTTPS. For more information about MFA
         * Delete, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/UsingMFADelete.html">Using
         * MFA Delete</a> in the <i>Amazon S3 User Guide</i>. To see sample requests that
         * use versioning, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTObjectDELETE.html#ExampleVersionObjectDelete">Sample
         * Request</a>. </p>  <p> <b>Directory buckets</b> - MFA delete is not
         * supported by directory buckets.</p>  <p>You can delete objects by
         * explicitly calling DELETE Object or calling (<a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycle.html">PutBucketLifecycle</a>)
         * to enable Amazon S3 to remove them for you. If you want to block users or
         * accounts from removing or deleting objects from your bucket, you must deny them
         * the <code>s3:DeleteObject</code>, <code>s3:DeleteObjectVersion</code>, and
         * <code>s3:PutLifeCycleConfiguration</code> actions. </p>  <p> <b>Directory
         * buckets</b> - S3 Lifecycle is not supported by directory buckets.</p> 
         * <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - The following permissions are required in your policies when
         * your <code>DeleteObjects</code> request includes specific headers.</p> <ul> <li>
         * <p> <b> <code>s3:DeleteObject</code> </b> - To delete an object from a bucket,
         * you must always have the <code>s3:DeleteObject</code> permission.</p> </li> <li>
         * <p> <b> <code>s3:DeleteObjectVersion</code> </b> - To delete a specific version
         * of an object from a versioning-enabled bucket, you must have the
         * <code>s3:DeleteObjectVersion</code> permission.</p> </li> </ul> </li> <li> <p>
         * <b>Directory bucket permissions</b> - To grant access to this API operation on a
         * directory bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following action is related to <code>DeleteObject</code>:</p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteObject">AWS API
         * Reference</a></p>
         */
        virtual Model::DeleteObjectOutcome DeleteObject(const Model::DeleteObjectRequest& request) const;

        /**
         * A Callable wrapper for DeleteObject that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteObjectRequestT = Model::DeleteObjectRequest>
        Model::DeleteObjectOutcomeCallable DeleteObjectCallable(const DeleteObjectRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteObject, request);
        }

        /**
         * An Async wrapper for DeleteObject that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteObjectRequestT = Model::DeleteObjectRequest>
        void DeleteObjectAsync(const DeleteObjectRequestT& request, const DeleteObjectResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteObject, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Removes the entire tag set from the specified object. For more information
         * about managing object tags, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-tagging.html">
         * Object Tagging</a>.</p> <p>To use this operation, you must have permission to
         * perform the <code>s3:DeleteObjectTagging</code> action.</p> <p>To delete tags of
         * a specific object version, add the <code>versionId</code> query parameter in the
         * request. You will need permission for the
         * <code>s3:DeleteObjectVersionTagging</code> action.</p> <p>The following
         * operations are related to <code>DeleteObjectTagging</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObjectTagging.html">PutObjectTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectTagging.html">GetObjectTagging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteObjectTagging">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteObjectTaggingOutcome DeleteObjectTagging(const Model::DeleteObjectTaggingRequest& request) const;

        /**
         * A Callable wrapper for DeleteObjectTagging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteObjectTaggingRequestT = Model::DeleteObjectTaggingRequest>
        Model::DeleteObjectTaggingOutcomeCallable DeleteObjectTaggingCallable(const DeleteObjectTaggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteObjectTagging, request);
        }

        /**
         * An Async wrapper for DeleteObjectTagging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteObjectTaggingRequestT = Model::DeleteObjectTaggingRequest>
        void DeleteObjectTaggingAsync(const DeleteObjectTaggingRequestT& request, const DeleteObjectTaggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteObjectTagging, request, handler, context);
        }

        /**
         * <p>This operation enables you to delete multiple objects from a bucket using a
         * single HTTP request. If you know the object keys that you want to delete, then
         * this operation provides a suitable alternative to sending individual delete
         * requests, reducing per-request overhead.</p> <p>The request can contain a list
         * of up to 1000 keys that you want to delete. In the XML, you provide the object
         * key names, and optionally, version IDs if you want to delete a specific version
         * of the object from a versioning-enabled bucket. For each key, Amazon S3 performs
         * a delete operation and returns the result of that delete, success or failure, in
         * the response. Note that if the object specified in the request is not found,
         * Amazon S3 returns the result as deleted.</p>  <ul> <li> <p> <b>Directory
         * buckets</b> - S3 Versioning isn't enabled and supported for directory
         * buckets.</p> </li> <li> <p> <b>Directory buckets</b> - For directory buckets,
         * you must make requests for this API operation to the Zonal endpoint. These
         * endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <p>The operation supports two modes for the response:
         * verbose and quiet. By default, the operation uses verbose mode in which the
         * response includes the result of deletion of each key in your request. In quiet
         * mode the response includes only keys where the delete operation encountered an
         * error. For a successful deletion in a quiet mode, the operation does not return
         * any information about the delete in the response body.</p> <p>When performing
         * this action on an MFA Delete enabled bucket, that attempts to delete any
         * versioned objects, you must include an MFA token. If you do not provide one, the
         * entire request will fail, even if there are non-versioned objects you are trying
         * to delete. If you provide an invalid token, whether there are versioned keys in
         * the request or not, the entire Multi-Object Delete request will fail. For
         * information about MFA Delete, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/Versioning.html#MultiFactorAuthenticationDelete">MFA
         * Delete</a> in the <i>Amazon S3 User Guide</i>.</p>  <p> <b>Directory
         * buckets</b> - MFA delete is not supported by directory buckets.</p>  <dl>
         * <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - The following permissions are required in your policies when
         * your <code>DeleteObjects</code> request includes specific headers.</p> <ul> <li>
         * <p> <b> <code>s3:DeleteObject</code> </b> - To delete an object from a bucket,
         * you must always specify the <code>s3:DeleteObject</code> permission.</p> </li>
         * <li> <p> <b> <code>s3:DeleteObjectVersion</code> </b> - To delete a specific
         * version of an object from a versioning-enabled bucket, you must specify the
         * <code>s3:DeleteObjectVersion</code> permission.</p> </li> </ul> </li> <li> <p>
         * <b>Directory bucket permissions</b> - To grant access to this API operation on a
         * directory bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>Content-MD5 request
         * header</dt> <dd> <ul> <li> <p> <b>General purpose bucket</b> - The Content-MD5
         * request header is required for all Multi-Object Delete requests. Amazon S3 uses
         * the header value to ensure that your request body has not been altered in
         * transit.</p> </li> <li> <p> <b>Directory bucket</b> - The Content-MD5 request
         * header or a additional checksum request header (including
         * <code>x-amz-checksum-crc32</code>, <code>x-amz-checksum-crc32c</code>,
         * <code>x-amz-checksum-sha1</code>, or <code>x-amz-checksum-sha256</code>) is
         * required for all Multi-Object Delete requests.</p> </li> </ul> </dd> <dt>HTTP
         * Host header syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host
         * header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>DeleteObjects</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteObjects">AWS
         * API Reference</a></p>
         */
        virtual Model::DeleteObjectsOutcome DeleteObjects(const Model::DeleteObjectsRequest& request) const;

        /**
         * A Callable wrapper for DeleteObjects that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeleteObjectsRequestT = Model::DeleteObjectsRequest>
        Model::DeleteObjectsOutcomeCallable DeleteObjectsCallable(const DeleteObjectsRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeleteObjects, request);
        }

        /**
         * An Async wrapper for DeleteObjects that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeleteObjectsRequestT = Model::DeleteObjectsRequest>
        void DeleteObjectsAsync(const DeleteObjectsRequestT& request, const DeleteObjectsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeleteObjects, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Removes the <code>PublicAccessBlock</code> configuration for an Amazon S3
         * bucket. To use this operation, you must have the
         * <code>s3:PutBucketPublicAccessBlock</code> permission. For more information
         * about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>The following
         * operations are related to <code>DeletePublicAccessBlock</code>:</p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html">Using
         * Amazon S3 Block Public Access</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetPublicAccessBlock.html">GetPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutPublicAccessBlock.html">PutPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketPolicyStatus.html">GetBucketPolicyStatus</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeletePublicAccessBlock">AWS
         * API Reference</a></p>
         */
        virtual Model::DeletePublicAccessBlockOutcome DeletePublicAccessBlock(const Model::DeletePublicAccessBlockRequest& request) const;

        /**
         * A Callable wrapper for DeletePublicAccessBlock that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename DeletePublicAccessBlockRequestT = Model::DeletePublicAccessBlockRequest>
        Model::DeletePublicAccessBlockOutcomeCallable DeletePublicAccessBlockCallable(const DeletePublicAccessBlockRequestT& request) const
        {
            return SubmitCallable(&S3Client::DeletePublicAccessBlock, request);
        }

        /**
         * An Async wrapper for DeletePublicAccessBlock that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename DeletePublicAccessBlockRequestT = Model::DeletePublicAccessBlockRequest>
        void DeletePublicAccessBlockAsync(const DeletePublicAccessBlockRequestT& request, const DeletePublicAccessBlockResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::DeletePublicAccessBlock, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>This implementation of the GET action uses the <code>accelerate</code>
         * subresource to return the Transfer Acceleration state of a bucket, which is
         * either <code>Enabled</code> or <code>Suspended</code>. Amazon S3 Transfer
         * Acceleration is a bucket-level feature that enables you to perform faster data
         * transfers to and from Amazon S3.</p> <p>To use this operation, you must have
         * permission to perform the <code>s3:GetAccelerateConfiguration</code> action. The
         * bucket owner has this permission by default. The bucket owner can grant this
         * permission to others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to your Amazon S3 Resources</a> in the <i>Amazon S3 User
         * Guide</i>.</p> <p>You set the Transfer Acceleration state of an existing bucket
         * to <code>Enabled</code> or <code>Suspended</code> by using the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketAccelerateConfiguration.html">PutBucketAccelerateConfiguration</a>
         * operation. </p> <p>A GET <code>accelerate</code> request does not return a state
         * value for a bucket that has no transfer acceleration state. A bucket has no
         * Transfer Acceleration state if a state has never been set on the bucket. </p>
         * <p>For more information about transfer acceleration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/transfer-acceleration.html">Transfer
         * Acceleration</a> in the Amazon S3 User Guide.</p> <p>The following operations
         * are related to <code>GetBucketAccelerateConfiguration</code>:</p> <ul> <li> <p>
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketAccelerateConfiguration.html">PutBucketAccelerateConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketAccelerateConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketAccelerateConfigurationOutcome GetBucketAccelerateConfiguration(const Model::GetBucketAccelerateConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketAccelerateConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketAccelerateConfigurationRequestT = Model::GetBucketAccelerateConfigurationRequest>
        Model::GetBucketAccelerateConfigurationOutcomeCallable GetBucketAccelerateConfigurationCallable(const GetBucketAccelerateConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketAccelerateConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketAccelerateConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketAccelerateConfigurationRequestT = Model::GetBucketAccelerateConfigurationRequest>
        void GetBucketAccelerateConfigurationAsync(const GetBucketAccelerateConfigurationRequestT& request, const GetBucketAccelerateConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketAccelerateConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>This implementation of the <code>GET</code> action uses the <code>acl</code>
         * subresource to return the access control list (ACL) of a bucket. To use
         * <code>GET</code> to return the ACL of the bucket, you must have the
         * <code>READ_ACP</code> access to the bucket. If <code>READ_ACP</code> permission
         * is granted to the anonymous user, you can return the ACL of the bucket without
         * using an authorization header.</p> <p>When you use this API operation with an
         * access point, provide the alias of the access point in place of the bucket
         * name.</p> <p>When you use this API operation with an Object Lambda access point,
         * provide the alias of the Object Lambda access point in place of the bucket name.
         * If the Object Lambda access point alias in a request is not valid, the error
         * code <code>InvalidAccessPointAliasError</code> is returned. For more information
         * about <code>InvalidAccessPointAliasError</code>, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#ErrorCodeList">List
         * of Error Codes</a>.</p>  <p>If your bucket uses the bucket owner enforced
         * setting for S3 Object Ownership, requests to read ACLs are still supported and
         * return the <code>bucket-owner-full-control</code> ACL with the owner being the
         * account that created the bucket. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html">
         * Controlling object ownership and disabling ACLs</a> in the <i>Amazon S3 User
         * Guide</i>.</p>  <p>The following operations are related to
         * <code>GetBucketAcl</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjects.html">ListObjects</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketAcl">AWS API
         * Reference</a></p>
         */
        virtual Model::GetBucketAclOutcome GetBucketAcl(const Model::GetBucketAclRequest& request) const;

        /**
         * A Callable wrapper for GetBucketAcl that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketAclRequestT = Model::GetBucketAclRequest>
        Model::GetBucketAclOutcomeCallable GetBucketAclCallable(const GetBucketAclRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketAcl, request);
        }

        /**
         * An Async wrapper for GetBucketAcl that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketAclRequestT = Model::GetBucketAclRequest>
        void GetBucketAclAsync(const GetBucketAclRequestT& request, const GetBucketAclResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketAcl, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>This implementation of the GET action returns an analytics configuration
         * (identified by the analytics configuration ID) from the bucket.</p> <p>To use
         * this operation, you must have permissions to perform the
         * <code>s3:GetAnalyticsConfiguration</code> action. The bucket owner has this
         * permission by default. The bucket owner can grant this permission to others. For
         * more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">
         * Permissions Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a> in the <i>Amazon S3 User
         * Guide</i>. </p> <p>For information about Amazon S3 analytics feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/analytics-storage-class.html">Amazon
         * S3 Analytics – Storage Class Analysis</a> in the <i>Amazon S3 User
         * Guide</i>.</p> <p>The following operations are related to
         * <code>GetBucketAnalyticsConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketAnalyticsConfiguration.html">DeleteBucketAnalyticsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketAnalyticsConfigurations.html">ListBucketAnalyticsConfigurations</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketAnalyticsConfiguration.html">PutBucketAnalyticsConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketAnalyticsConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketAnalyticsConfigurationOutcome GetBucketAnalyticsConfiguration(const Model::GetBucketAnalyticsConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketAnalyticsConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketAnalyticsConfigurationRequestT = Model::GetBucketAnalyticsConfigurationRequest>
        Model::GetBucketAnalyticsConfigurationOutcomeCallable GetBucketAnalyticsConfigurationCallable(const GetBucketAnalyticsConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketAnalyticsConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketAnalyticsConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketAnalyticsConfigurationRequestT = Model::GetBucketAnalyticsConfigurationRequest>
        void GetBucketAnalyticsConfigurationAsync(const GetBucketAnalyticsConfigurationRequestT& request, const GetBucketAnalyticsConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketAnalyticsConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the Cross-Origin Resource Sharing (CORS) configuration information
         * set for the bucket.</p> <p> To use this operation, you must have permission to
         * perform the <code>s3:GetBucketCORS</code> action. By default, the bucket owner
         * has this permission and can grant it to others.</p> <p>When you use this API
         * operation with an access point, provide the alias of the access point in place
         * of the bucket name.</p> <p>When you use this API operation with an Object Lambda
         * access point, provide the alias of the Object Lambda access point in place of
         * the bucket name. If the Object Lambda access point alias in a request is not
         * valid, the error code <code>InvalidAccessPointAliasError</code> is returned. For
         * more information about <code>InvalidAccessPointAliasError</code>, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#ErrorCodeList">List
         * of Error Codes</a>.</p> <p> For more information about CORS, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cors.html"> Enabling
         * Cross-Origin Resource Sharing</a>.</p> <p>The following operations are related
         * to <code>GetBucketCors</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketCors.html">PutBucketCors</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketCors.html">DeleteBucketCors</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketCors">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketCorsOutcome GetBucketCors(const Model::GetBucketCorsRequest& request) const;

        /**
         * A Callable wrapper for GetBucketCors that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketCorsRequestT = Model::GetBucketCorsRequest>
        Model::GetBucketCorsOutcomeCallable GetBucketCorsCallable(const GetBucketCorsRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketCors, request);
        }

        /**
         * An Async wrapper for GetBucketCors that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketCorsRequestT = Model::GetBucketCorsRequest>
        void GetBucketCorsAsync(const GetBucketCorsRequestT& request, const GetBucketCorsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketCors, request, handler, context);
        }

        /**
         * <p>Returns the default encryption configuration for an Amazon S3 bucket. By
         * default, all buckets have a default encryption configuration that uses
         * server-side encryption with Amazon S3 managed keys (SSE-S3). </p>  <ul>
         * <li> <p> <b>General purpose buckets</b> - For information about the bucket
         * default encryption feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-encryption.html">Amazon
         * S3 Bucket Default Encryption</a> in the <i>Amazon S3 User Guide</i>.</p> </li>
         * <li> <p> <b>Directory buckets</b> - For directory buckets, there are only two
         * supported options for server-side encryption: SSE-S3 and SSE-KMS. For
         * information about the default encryption configuration in directory buckets, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-bucket-encryption.html">Setting
         * default server-side encryption behavior for directory buckets</a>.</p> </li>
         * </ul>  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose
         * bucket permissions</b> - The <code>s3:GetEncryptionConfiguration</code>
         * permission is required in a policy. The bucket owner has this permission by
         * default. The bucket owner can grant this permission to others. For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> </li> <li> <p>
         * <b>Directory bucket permissions</b> - To grant access to this API operation, you
         * must have the <code>s3express:GetEncryptionConfiguration</code> permission in an
         * IAM identity-based policy instead of a bucket policy. Cross-account access to
         * this API operation isn't supported. This operation can only be performed by the
         * Amazon Web Services account that owns the resource. For more information about
         * directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following operations are related to <code>GetBucketEncryption</code>:</p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketEncryption.html">PutBucketEncryption</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketEncryption.html">DeleteBucketEncryption</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketEncryption">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketEncryptionOutcome GetBucketEncryption(const Model::GetBucketEncryptionRequest& request) const;

        /**
         * A Callable wrapper for GetBucketEncryption that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketEncryptionRequestT = Model::GetBucketEncryptionRequest>
        Model::GetBucketEncryptionOutcomeCallable GetBucketEncryptionCallable(const GetBucketEncryptionRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketEncryption, request);
        }

        /**
         * An Async wrapper for GetBucketEncryption that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketEncryptionRequestT = Model::GetBucketEncryptionRequest>
        void GetBucketEncryptionAsync(const GetBucketEncryptionRequestT& request, const GetBucketEncryptionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketEncryption, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Gets the S3 Intelligent-Tiering configuration from the specified bucket.</p>
         * <p>The S3 Intelligent-Tiering storage class is designed to optimize storage
         * costs by automatically moving data to the most cost-effective storage access
         * tier, without performance impact or operational overhead. S3 Intelligent-Tiering
         * delivers automatic cost savings in three low latency and high throughput access
         * tiers. To get the lowest storage cost on data that can be accessed in minutes to
         * hours, you can choose to activate additional archiving capabilities.</p> <p>The
         * S3 Intelligent-Tiering storage class is the ideal storage class for data with
         * unknown, changing, or unpredictable access patterns, independent of object size
         * or retention period. If the size of an object is less than 128 KB, it is not
         * monitored and not eligible for auto-tiering. Smaller objects can be stored, but
         * they are always charged at the Frequent Access tier rates in the S3
         * Intelligent-Tiering storage class.</p> <p>For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html#sc-dynamic-data-access">Storage
         * class for automatically optimizing frequently and infrequently accessed
         * objects</a>.</p> <p>Operations related to
         * <code>GetBucketIntelligentTieringConfiguration</code> include: </p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketIntelligentTieringConfiguration.html">DeleteBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketIntelligentTieringConfiguration.html">PutBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketIntelligentTieringConfigurations.html">ListBucketIntelligentTieringConfigurations</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketIntelligentTieringConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketIntelligentTieringConfigurationOutcome GetBucketIntelligentTieringConfiguration(const Model::GetBucketIntelligentTieringConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketIntelligentTieringConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketIntelligentTieringConfigurationRequestT = Model::GetBucketIntelligentTieringConfigurationRequest>
        Model::GetBucketIntelligentTieringConfigurationOutcomeCallable GetBucketIntelligentTieringConfigurationCallable(const GetBucketIntelligentTieringConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketIntelligentTieringConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketIntelligentTieringConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketIntelligentTieringConfigurationRequestT = Model::GetBucketIntelligentTieringConfigurationRequest>
        void GetBucketIntelligentTieringConfigurationAsync(const GetBucketIntelligentTieringConfigurationRequestT& request, const GetBucketIntelligentTieringConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketIntelligentTieringConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns an inventory configuration (identified by the inventory configuration
         * ID) from the bucket.</p> <p>To use this operation, you must have permissions to
         * perform the <code>s3:GetInventoryConfiguration</code> action. The bucket owner
         * has this permission by default and can grant this permission to others. For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * the Amazon S3 inventory feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-inventory.html">Amazon
         * S3 Inventory</a>.</p> <p>The following operations are related to
         * <code>GetBucketInventoryConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketInventoryConfiguration.html">DeleteBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketInventoryConfigurations.html">ListBucketInventoryConfigurations</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketInventoryConfiguration.html">PutBucketInventoryConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketInventoryConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketInventoryConfigurationOutcome GetBucketInventoryConfiguration(const Model::GetBucketInventoryConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketInventoryConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketInventoryConfigurationRequestT = Model::GetBucketInventoryConfigurationRequest>
        Model::GetBucketInventoryConfigurationOutcomeCallable GetBucketInventoryConfigurationCallable(const GetBucketInventoryConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketInventoryConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketInventoryConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketInventoryConfigurationRequestT = Model::GetBucketInventoryConfigurationRequest>
        void GetBucketInventoryConfigurationAsync(const GetBucketInventoryConfigurationRequestT& request, const GetBucketInventoryConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketInventoryConfiguration, request, handler, context);
        }

        /**
         * <p>Returns the lifecycle configuration information set on the bucket. For
         * information about lifecycle configuration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lifecycle-mgmt.html">Object
         * Lifecycle Management</a>.</p> <p>Bucket lifecycle configuration now supports
         * specifying a lifecycle rule using an object key name prefix, one or more object
         * tags, object size, or any combination of these. Accordingly, this section
         * describes the latest API, which is compatible with the new functionality. The
         * previous version of the API supported filtering based only on an object key name
         * prefix, which is supported for general purpose buckets for backward
         * compatibility. For the related API description, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLifecycle.html">GetBucketLifecycle</a>.</p>
         *  <p>Lifecyle configurations for directory buckets only support expiring
         * objects and cancelling multipart uploads. Expiring of versioned objects,
         * transitions and tag filters are not supported.</p>  <dl>
         * <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - By default, all Amazon S3 resources are private, including
         * buckets, objects, and related subresources (for example, lifecycle configuration
         * and website configuration). Only the resource owner (that is, the Amazon Web
         * Services account that created it) can access the resource. The resource owner
         * can optionally grant access permissions to others by writing an access policy.
         * For this operation, a user must have the
         * <code>s3:GetLifecycleConfiguration</code> permission.</p> <p>For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> </li> </ul> <ul> <li>
         * <p> <b>Directory bucket permissions</b> - You must have the
         * <code>s3express:GetLifecycleConfiguration</code> permission in an IAM
         * identity-based policy to use this operation. Cross-account access to this API
         * operation isn't supported. The resource owner can optionally grant access
         * permissions to others by creating a role or user for them as long as they are
         * within the same account as the owner and resource.</p> <p>For more information
         * about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Authorizing
         * Regional endpoint APIs with IAM</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p> <b>Directory buckets </b> - For directory buckets, you must make
         * requests for this API operation to the Regional endpoint. These endpoints
         * support path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  </li> </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory
         * buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region</i>.amazonaws.com</code>.</p> </dd> </dl> <p>
         * <code>GetBucketLifecycleConfiguration</code> has the following special
         * error:</p> <ul> <li> <p>Error code: <code>NoSuchLifecycleConfiguration</code>
         * </p> <ul> <li> <p>Description: The lifecycle configuration does not exist.</p>
         * </li> <li> <p>HTTP Status Code: 404 Not Found</p> </li> <li> <p>SOAP Fault Code
         * Prefix: Client</p> </li> </ul> </li> </ul> <p>The following operations are
         * related to <code>GetBucketLifecycleConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLifecycle.html">GetBucketLifecycle</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycle.html">PutBucketLifecycle</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketLifecycle.html">DeleteBucketLifecycle</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketLifecycleConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketLifecycleConfigurationOutcome GetBucketLifecycleConfiguration(const Model::GetBucketLifecycleConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketLifecycleConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketLifecycleConfigurationRequestT = Model::GetBucketLifecycleConfigurationRequest>
        Model::GetBucketLifecycleConfigurationOutcomeCallable GetBucketLifecycleConfigurationCallable(const GetBucketLifecycleConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketLifecycleConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketLifecycleConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketLifecycleConfigurationRequestT = Model::GetBucketLifecycleConfigurationRequest>
        void GetBucketLifecycleConfigurationAsync(const GetBucketLifecycleConfigurationRequestT& request, const GetBucketLifecycleConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketLifecycleConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the Region the bucket resides in. You set the bucket's Region using
         * the <code>LocationConstraint</code> request parameter in a
         * <code>CreateBucket</code> request. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>.</p>
         * <p>When you use this API operation with an access point, provide the alias of
         * the access point in place of the bucket name.</p> <p>When you use this API
         * operation with an Object Lambda access point, provide the alias of the Object
         * Lambda access point in place of the bucket name. If the Object Lambda access
         * point alias in a request is not valid, the error code
         * <code>InvalidAccessPointAliasError</code> is returned. For more information
         * about <code>InvalidAccessPointAliasError</code>, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#ErrorCodeList">List
         * of Error Codes</a>.</p>  <p>We recommend that you use <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadBucket.html">HeadBucket</a>
         * to return the Region that a bucket resides in. For backward compatibility,
         * Amazon S3 continues to support GetBucketLocation.</p>  <p>The following
         * operations are related to <code>GetBucketLocation</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketLocation">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketLocationOutcome GetBucketLocation(const Model::GetBucketLocationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketLocation that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketLocationRequestT = Model::GetBucketLocationRequest>
        Model::GetBucketLocationOutcomeCallable GetBucketLocationCallable(const GetBucketLocationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketLocation, request);
        }

        /**
         * An Async wrapper for GetBucketLocation that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketLocationRequestT = Model::GetBucketLocationRequest>
        void GetBucketLocationAsync(const GetBucketLocationRequestT& request, const GetBucketLocationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketLocation, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the logging status of a bucket and the permissions users have to view
         * and modify that status.</p> <p>The following operations are related to
         * <code>GetBucketLogging</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLogging.html">PutBucketLogging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketLogging">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketLoggingOutcome GetBucketLogging(const Model::GetBucketLoggingRequest& request) const;

        /**
         * A Callable wrapper for GetBucketLogging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketLoggingRequestT = Model::GetBucketLoggingRequest>
        Model::GetBucketLoggingOutcomeCallable GetBucketLoggingCallable(const GetBucketLoggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketLogging, request);
        }

        /**
         * An Async wrapper for GetBucketLogging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketLoggingRequestT = Model::GetBucketLoggingRequest>
        void GetBucketLoggingAsync(const GetBucketLoggingRequestT& request, const GetBucketLoggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketLogging, request, handler, context);
        }

        /**
         * <p> Retrieves the metadata table configuration for a general purpose bucket. For
         * more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/metadata-tables-overview.html">Accelerating
         * data discovery with S3 Metadata</a> in the <i>Amazon S3 User Guide</i>. </p>
         * <dl> <dt>Permissions</dt> <dd> <p>To use this operation, you must have the
         * <code>s3:GetBucketMetadataTableConfiguration</code> permission. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/metadata-tables-permissions.html">Setting
         * up permissions for configuring metadata tables</a> in the <i>Amazon S3 User
         * Guide</i>. </p> </dd> </dl> <p>The following operations are related to
         * <code>GetBucketMetadataTableConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucketMetadataTableConfiguration.html">CreateBucketMetadataTableConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketMetadataTableConfiguration.html">DeleteBucketMetadataTableConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketMetadataTableConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketMetadataTableConfigurationOutcome GetBucketMetadataTableConfiguration(const Model::GetBucketMetadataTableConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketMetadataTableConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketMetadataTableConfigurationRequestT = Model::GetBucketMetadataTableConfigurationRequest>
        Model::GetBucketMetadataTableConfigurationOutcomeCallable GetBucketMetadataTableConfigurationCallable(const GetBucketMetadataTableConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketMetadataTableConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketMetadataTableConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketMetadataTableConfigurationRequestT = Model::GetBucketMetadataTableConfigurationRequest>
        void GetBucketMetadataTableConfigurationAsync(const GetBucketMetadataTableConfigurationRequestT& request, const GetBucketMetadataTableConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketMetadataTableConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Gets a metrics configuration (specified by the metrics configuration ID) from
         * the bucket. Note that this doesn't include the daily storage metrics.</p> <p> To
         * use this operation, you must have permissions to perform the
         * <code>s3:GetMetricsConfiguration</code> action. The bucket owner has this
         * permission by default. The bucket owner can grant this permission to others. For
         * more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p> For information
         * about CloudWatch request metrics for Amazon S3, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cloudwatch-monitoring.html">Monitoring
         * Metrics with Amazon CloudWatch</a>.</p> <p>The following operations are related
         * to <code>GetBucketMetricsConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketMetricsConfiguration.html">PutBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketMetricsConfiguration.html">DeleteBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketMetricsConfigurations.html">ListBucketMetricsConfigurations</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cloudwatch-monitoring.html">Monitoring
         * Metrics with Amazon CloudWatch</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketMetricsConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketMetricsConfigurationOutcome GetBucketMetricsConfiguration(const Model::GetBucketMetricsConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketMetricsConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketMetricsConfigurationRequestT = Model::GetBucketMetricsConfigurationRequest>
        Model::GetBucketMetricsConfigurationOutcomeCallable GetBucketMetricsConfigurationCallable(const GetBucketMetricsConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketMetricsConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketMetricsConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketMetricsConfigurationRequestT = Model::GetBucketMetricsConfigurationRequest>
        void GetBucketMetricsConfigurationAsync(const GetBucketMetricsConfigurationRequestT& request, const GetBucketMetricsConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketMetricsConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the notification configuration of a bucket.</p> <p>If notifications
         * are not enabled on the bucket, the action returns an empty
         * <code>NotificationConfiguration</code> element.</p> <p>By default, you must be
         * the bucket owner to read the notification configuration of a bucket. However,
         * the bucket owner can use a bucket policy to grant permission to other users to
         * read this configuration with the <code>s3:GetBucketNotification</code>
         * permission.</p> <p>When you use this API operation with an access point, provide
         * the alias of the access point in place of the bucket name.</p> <p>When you use
         * this API operation with an Object Lambda access point, provide the alias of the
         * Object Lambda access point in place of the bucket name. If the Object Lambda
         * access point alias in a request is not valid, the error code
         * <code>InvalidAccessPointAliasError</code> is returned. For more information
         * about <code>InvalidAccessPointAliasError</code>, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#ErrorCodeList">List
         * of Error Codes</a>.</p> <p>For more information about setting and reading the
         * notification configuration on a bucket, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Setting
         * Up Notification of Bucket Events</a>. For more information about bucket
         * policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-iam-policies.html">Using
         * Bucket Policies</a>.</p> <p>The following action is related to
         * <code>GetBucketNotification</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketNotification.html">PutBucketNotification</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketNotificationConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketNotificationConfigurationOutcome GetBucketNotificationConfiguration(const Model::GetBucketNotificationConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketNotificationConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketNotificationConfigurationRequestT = Model::GetBucketNotificationConfigurationRequest>
        Model::GetBucketNotificationConfigurationOutcomeCallable GetBucketNotificationConfigurationCallable(const GetBucketNotificationConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketNotificationConfiguration, request);
        }

        /**
         * An Async wrapper for GetBucketNotificationConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketNotificationConfigurationRequestT = Model::GetBucketNotificationConfigurationRequest>
        void GetBucketNotificationConfigurationAsync(const GetBucketNotificationConfigurationRequestT& request, const GetBucketNotificationConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketNotificationConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Retrieves <code>OwnershipControls</code> for an Amazon S3 bucket. To use this
         * operation, you must have the <code>s3:GetBucketOwnershipControls</code>
         * permission. For more information about Amazon S3 permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html">Specifying
         * permissions in a policy</a>. </p> <p>For information about Amazon S3 Object
         * Ownership, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html">Using
         * Object Ownership</a>. </p> <p>The following operations are related to
         * <code>GetBucketOwnershipControls</code>:</p> <ul> <li> <p>
         * <a>PutBucketOwnershipControls</a> </p> </li> <li> <p>
         * <a>DeleteBucketOwnershipControls</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketOwnershipControls">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketOwnershipControlsOutcome GetBucketOwnershipControls(const Model::GetBucketOwnershipControlsRequest& request) const;

        /**
         * A Callable wrapper for GetBucketOwnershipControls that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketOwnershipControlsRequestT = Model::GetBucketOwnershipControlsRequest>
        Model::GetBucketOwnershipControlsOutcomeCallable GetBucketOwnershipControlsCallable(const GetBucketOwnershipControlsRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketOwnershipControls, request);
        }

        /**
         * An Async wrapper for GetBucketOwnershipControls that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketOwnershipControlsRequestT = Model::GetBucketOwnershipControlsRequest>
        void GetBucketOwnershipControlsAsync(const GetBucketOwnershipControlsRequestT& request, const GetBucketOwnershipControlsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketOwnershipControls, request, handler, context);
        }

        /**
         * <p>Returns the policy of a specified bucket.</p>  <p> <b>Directory buckets
         * </b> - For directory buckets, you must make requests for this API operation to
         * the Regional endpoint. These endpoints support path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <p>If you are using an identity other
         * than the root user of the Amazon Web Services account that owns the bucket, the
         * calling identity must both have the <code>GetBucketPolicy</code> permissions on
         * the specified bucket and belong to the bucket owner's account in order to use
         * this operation.</p> <p>If you don't have <code>GetBucketPolicy</code>
         * permissions, Amazon S3 returns a <code>403 Access Denied</code> error. If you
         * have the correct permissions, but you're not using an identity that belongs to
         * the bucket owner's account, Amazon S3 returns a <code>405 Method Not
         * Allowed</code> error.</p>  <p>To ensure that bucket owners don't
         * inadvertently lock themselves out of their own buckets, the root principal in a
         * bucket owner's Amazon Web Services account can perform the
         * <code>GetBucketPolicy</code>, <code>PutBucketPolicy</code>, and
         * <code>DeleteBucketPolicy</code> API actions, even if their bucket policy
         * explicitly denies the root principal's access. Bucket owner root principals can
         * only be blocked from performing these API actions by VPC endpoint policies and
         * Amazon Web Services Organizations policies.</p>  <ul> <li> <p>
         * <b>General purpose bucket permissions</b> - The <code>s3:GetBucketPolicy</code>
         * permission is required in a policy. For more information about general purpose
         * buckets bucket policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-iam-policies.html">Using
         * Bucket Policies and User Policies</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> <li> <p> <b>Directory bucket permissions</b> - To grant access to this API
         * operation, you must have the <code>s3express:GetBucketPolicy</code> permission
         * in an IAM identity-based policy instead of a bucket policy. Cross-account access
         * to this API operation isn't supported. This operation can only be performed by
         * the Amazon Web Services account that owns the resource. For more information
         * about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd> <dt>Example bucket
         * policies</dt> <dd> <p> <b>General purpose buckets example bucket policies</b> -
         * See <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/example-bucket-policies.html">Bucket
         * policy examples</a> in the <i>Amazon S3 User Guide</i>.</p> <p> <b>Directory
         * bucket example bucket policies</b> - See <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-example-bucket-policies.html">Example
         * bucket policies for S3 Express One Zone</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory
         * buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following action is related to <code>GetBucketPolicy</code>:</p> <ul>
         * <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketPolicy">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketPolicyOutcome GetBucketPolicy(const Model::GetBucketPolicyRequest& request) const;

        /**
         * A Callable wrapper for GetBucketPolicy that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketPolicyRequestT = Model::GetBucketPolicyRequest>
        Model::GetBucketPolicyOutcomeCallable GetBucketPolicyCallable(const GetBucketPolicyRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketPolicy, request);
        }

        /**
         * An Async wrapper for GetBucketPolicy that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketPolicyRequestT = Model::GetBucketPolicyRequest>
        void GetBucketPolicyAsync(const GetBucketPolicyRequestT& request, const GetBucketPolicyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketPolicy, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Retrieves the policy status for an Amazon S3 bucket, indicating whether the
         * bucket is public. In order to use this operation, you must have the
         * <code>s3:GetBucketPolicyStatus</code> permission. For more information about
         * Amazon S3 permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a>.</p> <p> For more information about when Amazon S3
         * considers a bucket public, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html#access-control-block-public-access-policy-status">The
         * Meaning of "Public"</a>. </p> <p>The following operations are related to
         * <code>GetBucketPolicyStatus</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html">Using
         * Amazon S3 Block Public Access</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetPublicAccessBlock.html">GetPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutPublicAccessBlock.html">PutPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeletePublicAccessBlock.html">DeletePublicAccessBlock</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketPolicyStatus">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketPolicyStatusOutcome GetBucketPolicyStatus(const Model::GetBucketPolicyStatusRequest& request) const;

        /**
         * A Callable wrapper for GetBucketPolicyStatus that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketPolicyStatusRequestT = Model::GetBucketPolicyStatusRequest>
        Model::GetBucketPolicyStatusOutcomeCallable GetBucketPolicyStatusCallable(const GetBucketPolicyStatusRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketPolicyStatus, request);
        }

        /**
         * An Async wrapper for GetBucketPolicyStatus that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketPolicyStatusRequestT = Model::GetBucketPolicyStatusRequest>
        void GetBucketPolicyStatusAsync(const GetBucketPolicyStatusRequestT& request, const GetBucketPolicyStatusResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketPolicyStatus, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the replication configuration of a bucket.</p>  <p> It can take
         * a while to propagate the put or delete a replication configuration to all Amazon
         * S3 systems. Therefore, a get request soon after put or delete can return a wrong
         * result. </p>  <p> For information about replication configuration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/replication.html">Replication</a>
         * in the <i>Amazon S3 User Guide</i>.</p> <p>This action requires permissions for
         * the <code>s3:GetReplicationConfiguration</code> action. For more information
         * about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-iam-policies.html">Using
         * Bucket Policies and User Policies</a>.</p> <p>If you include the
         * <code>Filter</code> element in a replication configuration, you must also
         * include the <code>DeleteMarkerReplication</code> and <code>Priority</code>
         * elements. The response also returns those elements.</p> <p>For information about
         * <code>GetBucketReplication</code> errors, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#ReplicationErrorCodeList">List
         * of replication-related error codes</a> </p> <p>The following operations are
         * related to <code>GetBucketReplication</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketReplication.html">PutBucketReplication</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketReplication.html">DeleteBucketReplication</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketReplication">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketReplicationOutcome GetBucketReplication(const Model::GetBucketReplicationRequest& request) const;

        /**
         * A Callable wrapper for GetBucketReplication that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketReplicationRequestT = Model::GetBucketReplicationRequest>
        Model::GetBucketReplicationOutcomeCallable GetBucketReplicationCallable(const GetBucketReplicationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketReplication, request);
        }

        /**
         * An Async wrapper for GetBucketReplication that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketReplicationRequestT = Model::GetBucketReplicationRequest>
        void GetBucketReplicationAsync(const GetBucketReplicationRequestT& request, const GetBucketReplicationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketReplication, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the request payment configuration of a bucket. To use this version of
         * the operation, you must be the bucket owner. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/RequesterPaysBuckets.html">Requester
         * Pays Buckets</a>.</p> <p>The following operations are related to
         * <code>GetBucketRequestPayment</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjects.html">ListObjects</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketRequestPayment">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketRequestPaymentOutcome GetBucketRequestPayment(const Model::GetBucketRequestPaymentRequest& request) const;

        /**
         * A Callable wrapper for GetBucketRequestPayment that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketRequestPaymentRequestT = Model::GetBucketRequestPaymentRequest>
        Model::GetBucketRequestPaymentOutcomeCallable GetBucketRequestPaymentCallable(const GetBucketRequestPaymentRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketRequestPayment, request);
        }

        /**
         * An Async wrapper for GetBucketRequestPayment that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketRequestPaymentRequestT = Model::GetBucketRequestPaymentRequest>
        void GetBucketRequestPaymentAsync(const GetBucketRequestPaymentRequestT& request, const GetBucketRequestPaymentResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketRequestPayment, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the tag set associated with the bucket.</p> <p>To use this operation,
         * you must have permission to perform the <code>s3:GetBucketTagging</code> action.
         * By default, the bucket owner has this permission and can grant this permission
         * to others.</p> <p> <code>GetBucketTagging</code> has the following special
         * error:</p> <ul> <li> <p>Error code: <code>NoSuchTagSet</code> </p> <ul> <li>
         * <p>Description: There is no tag set associated with the bucket.</p> </li> </ul>
         * </li> </ul> <p>The following operations are related to
         * <code>GetBucketTagging</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketTagging.html">PutBucketTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketTagging.html">DeleteBucketTagging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketTagging">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketTaggingOutcome GetBucketTagging(const Model::GetBucketTaggingRequest& request) const;

        /**
         * A Callable wrapper for GetBucketTagging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketTaggingRequestT = Model::GetBucketTaggingRequest>
        Model::GetBucketTaggingOutcomeCallable GetBucketTaggingCallable(const GetBucketTaggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketTagging, request);
        }

        /**
         * An Async wrapper for GetBucketTagging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketTaggingRequestT = Model::GetBucketTaggingRequest>
        void GetBucketTaggingAsync(const GetBucketTaggingRequestT& request, const GetBucketTaggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketTagging, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the versioning state of a bucket.</p> <p>To retrieve the versioning
         * state of a bucket, you must be the bucket owner.</p> <p>This implementation also
         * returns the MFA Delete status of the versioning state. If the MFA Delete status
         * is <code>enabled</code>, the bucket owner must use an authentication device to
         * change the versioning state of the bucket.</p> <p>The following operations are
         * related to <code>GetBucketVersioning</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html">DeleteObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketVersioning">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketVersioningOutcome GetBucketVersioning(const Model::GetBucketVersioningRequest& request) const;

        /**
         * A Callable wrapper for GetBucketVersioning that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketVersioningRequestT = Model::GetBucketVersioningRequest>
        Model::GetBucketVersioningOutcomeCallable GetBucketVersioningCallable(const GetBucketVersioningRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketVersioning, request);
        }

        /**
         * An Async wrapper for GetBucketVersioning that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketVersioningRequestT = Model::GetBucketVersioningRequest>
        void GetBucketVersioningAsync(const GetBucketVersioningRequestT& request, const GetBucketVersioningResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketVersioning, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the website configuration for a bucket. To host website on Amazon S3,
         * you can configure a bucket as website by adding a website configuration. For
         * more information about hosting websites, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/WebsiteHosting.html">Hosting
         * Websites on Amazon S3</a>. </p> <p>This GET action requires the
         * <code>S3:GetBucketWebsite</code> permission. By default, only the bucket owner
         * can read the bucket website configuration. However, bucket owners can allow
         * other users to read the website configuration by writing a bucket policy
         * granting them the <code>S3:GetBucketWebsite</code> permission.</p> <p>The
         * following operations are related to <code>GetBucketWebsite</code>:</p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketWebsite.html">DeleteBucketWebsite</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketWebsite.html">PutBucketWebsite</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetBucketWebsite">AWS
         * API Reference</a></p>
         */
        virtual Model::GetBucketWebsiteOutcome GetBucketWebsite(const Model::GetBucketWebsiteRequest& request) const;

        /**
         * A Callable wrapper for GetBucketWebsite that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetBucketWebsiteRequestT = Model::GetBucketWebsiteRequest>
        Model::GetBucketWebsiteOutcomeCallable GetBucketWebsiteCallable(const GetBucketWebsiteRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetBucketWebsite, request);
        }

        /**
         * An Async wrapper for GetBucketWebsite that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetBucketWebsiteRequestT = Model::GetBucketWebsiteRequest>
        void GetBucketWebsiteAsync(const GetBucketWebsiteRequestT& request, const GetBucketWebsiteResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetBucketWebsite, request, handler, context);
        }

        /**
         * <p>Retrieves an object from Amazon S3.</p> <p>In the <code>GetObject</code>
         * request, specify the full key name for the object.</p> <p> <b>General purpose
         * buckets</b> - Both the virtual-hosted-style requests and the path-style requests
         * are supported. For a virtual hosted-style request example, if you have the
         * object <code>photos/2006/February/sample.jpg</code>, specify the object key name
         * as <code>/photos/2006/February/sample.jpg</code>. For a path-style request
         * example, if you have the object <code>photos/2006/February/sample.jpg</code> in
         * the bucket named <code>examplebucket</code>, specify the object key name as
         * <code>/examplebucket/photos/2006/February/sample.jpg</code>. For more
         * information about request types, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/VirtualHosting.html#VirtualHostingSpecifyBucket">HTTP
         * Host Header Bucket Specification</a> in the <i>Amazon S3 User Guide</i>.</p> <p>
         * <b>Directory buckets</b> - Only virtual-hosted-style requests are supported. For
         * a virtual hosted-style request example, if you have the object
         * <code>photos/2006/February/sample.jpg</code> in the bucket named
         * <code>examplebucket--use1-az5--x-s3</code>, specify the object key name as
         * <code>/photos/2006/February/sample.jpg</code>. Also, when you make requests to
         * this API operation, your requests are sent to the Zonal endpoint. These
         * endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - You must have the required permissions in a policy. To use
         * <code>GetObject</code>, you must have the <code>READ</code> access to the object
         * (or version). If you grant <code>READ</code> access to the anonymous user, the
         * <code>GetObject</code> operation returns the object without using an
         * authorization header. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * permissions in a policy</a> in the <i>Amazon S3 User Guide</i>.</p> <p>If you
         * include a <code>versionId</code> in your request header, you must have the
         * <code>s3:GetObjectVersion</code> permission to access a specific version of an
         * object. The <code>s3:GetObject</code> permission is not required in this
         * scenario.</p> <p>If you request the current version of an object without a
         * specific <code>versionId</code> in the request header, only the
         * <code>s3:GetObject</code> permission is required. The
         * <code>s3:GetObjectVersion</code> permission is not required in this scenario.
         * </p> <p>If the object that you request doesn’t exist, the error that Amazon S3
         * returns depends on whether you also have the <code>s3:ListBucket</code>
         * permission.</p> <ul> <li> <p>If you have the <code>s3:ListBucket</code>
         * permission on the bucket, Amazon S3 returns an HTTP status code <code>404 Not
         * Found</code> error.</p> </li> <li> <p>If you don’t have the
         * <code>s3:ListBucket</code> permission, Amazon S3 returns an HTTP status code
         * <code>403 Access Denied</code> error.</p> </li> </ul> </li> <li> <p>
         * <b>Directory bucket permissions</b> - To grant access to this API operation on a
         * directory bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> <p>If the object is encrypted using
         * SSE-KMS, you must also have the <code>kms:GenerateDataKey</code> and
         * <code>kms:Decrypt</code> permissions in IAM identity-based policies and KMS key
         * policies for the KMS key.</p> </li> </ul> </dd> <dt>Storage classes</dt> <dd>
         * <p>If the object you are retrieving is stored in the S3 Glacier Flexible
         * Retrieval storage class, the S3 Glacier Deep Archive storage class, the S3
         * Intelligent-Tiering Archive Access tier, or the S3 Intelligent-Tiering Deep
         * Archive Access tier, before you can retrieve the object you must first restore a
         * copy using <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_RestoreObject.html">RestoreObject</a>.
         * Otherwise, this operation returns an <code>InvalidObjectState</code> error. For
         * information about restoring archived objects, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/restoring-objects.html">Restoring
         * Archived Objects</a> in the <i>Amazon S3 User Guide</i>.</p> <p> <b>Directory
         * buckets </b> - For directory buckets, only the S3 Express One Zone storage class
         * is supported to store newly created objects. Unsupported storage class values
         * won't write a destination object and will respond with the HTTP status code
         * <code>400 Bad Request</code>.</p> </dd> <dt>Encryption</dt> <dd> <p>Encryption
         * request headers, like <code>x-amz-server-side-encryption</code>, should not be
         * sent for the <code>GetObject</code> requests, if your object uses server-side
         * encryption with Amazon S3 managed encryption keys (SSE-S3), server-side
         * encryption with Key Management Service (KMS) keys (SSE-KMS), or dual-layer
         * server-side encryption with Amazon Web Services KMS keys (DSSE-KMS). If you
         * include the header in your <code>GetObject</code> requests for the object that
         * uses these types of keys, you’ll get an HTTP <code>400 Bad Request</code>
         * error.</p> <p> <b>Directory buckets</b> - For directory buckets, there are only
         * two supported options for server-side encryption: SSE-S3 and SSE-KMS. SSE-C
         * isn't supported. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-serv-side-encryption.html">Protecting
         * data with server-side encryption</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </dd> <dt>Overriding response header values through the request</dt> <dd>
         * <p>There are times when you want to override certain response header values of a
         * <code>GetObject</code> response. For example, you might override the
         * <code>Content-Disposition</code> response header value through your
         * <code>GetObject</code> request.</p> <p>You can override values for a set of
         * response headers. These modified response header values are included only in a
         * successful response, that is, when the HTTP status code <code>200 OK</code> is
         * returned. The headers you can override using the following query parameters in
         * the request are a subset of the headers that Amazon S3 accepts when you create
         * an object. </p> <p>The response headers that you can override for the
         * <code>GetObject</code> response are <code>Cache-Control</code>,
         * <code>Content-Disposition</code>, <code>Content-Encoding</code>,
         * <code>Content-Language</code>, <code>Content-Type</code>, and
         * <code>Expires</code>.</p> <p>To override values for a set of response headers in
         * the <code>GetObject</code> response, you can use the following query parameters
         * in the request.</p> <ul> <li> <p> <code>response-cache-control</code> </p> </li>
         * <li> <p> <code>response-content-disposition</code> </p> </li> <li> <p>
         * <code>response-content-encoding</code> </p> </li> <li> <p>
         * <code>response-content-language</code> </p> </li> <li> <p>
         * <code>response-content-type</code> </p> </li> <li> <p>
         * <code>response-expires</code> </p> </li> </ul>  <p>When you use these
         * parameters, you must sign the request by using either an Authorization header or
         * a presigned URL. These parameters cannot be used with an unsigned (anonymous)
         * request.</p>  </dd> <dt>HTTP Host header syntax</dt> <dd> <p>
         * <b>Directory buckets </b> - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>GetObject</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBuckets.html">ListBuckets</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAcl.html">GetObjectAcl</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObject">AWS API
         * Reference</a></p>
         */
        virtual Model::GetObjectOutcome GetObject(const Model::GetObjectRequest& request) const;

        /**
         * A Callable wrapper for GetObject that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        virtual Model::GetObjectOutcomeCallable GetObjectCallable(const Model::GetObjectRequest& request) const;

        /**
         * An Async wrapper for GetObject that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        virtual void GetObjectAsync(const Model::GetObjectRequest& request, const GetObjectResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const;

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the access control list (ACL) of an object. To use this operation,
         * you must have <code>s3:GetObjectAcl</code> permissions or <code>READ_ACP</code>
         * access to the object. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/acl-overview.html#acl-access-policy-permission-mapping">Mapping
         * of ACL permissions and access policy permissions</a> in the <i>Amazon S3 User
         * Guide</i> </p> <p>This functionality is not supported for Amazon S3 on
         * Outposts.</p> <p>By default, GET returns ACL information about the current
         * version of an object. To return ACL information about a different version, use
         * the versionId subresource.</p>  <p>If your bucket uses the bucket owner
         * enforced setting for S3 Object Ownership, requests to read ACLs are still
         * supported and return the <code>bucket-owner-full-control</code> ACL with the
         * owner being the account that created the bucket. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html">
         * Controlling object ownership and disabling ACLs</a> in the <i>Amazon S3 User
         * Guide</i>.</p>  <p>The following operations are related to
         * <code>GetObjectAcl</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html">DeleteObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectAcl">AWS API
         * Reference</a></p>
         */
        virtual Model::GetObjectAclOutcome GetObjectAcl(const Model::GetObjectAclRequest& request) const;

        /**
         * A Callable wrapper for GetObjectAcl that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectAclRequestT = Model::GetObjectAclRequest>
        Model::GetObjectAclOutcomeCallable GetObjectAclCallable(const GetObjectAclRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectAcl, request);
        }

        /**
         * An Async wrapper for GetObjectAcl that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectAclRequestT = Model::GetObjectAclRequest>
        void GetObjectAclAsync(const GetObjectAclRequestT& request, const GetObjectAclResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectAcl, request, handler, context);
        }

        /**
         * <p>Retrieves all the metadata from an object without returning the object
         * itself. This operation is useful if you're interested only in an object's
         * metadata. </p> <p> <code>GetObjectAttributes</code> combines the functionality
         * of <code>HeadObject</code> and <code>ListParts</code>. All of the data returned
         * with each of those individual calls can be returned with a single call to
         * <code>GetObjectAttributes</code>.</p>  <p> <b>Directory buckets</b> - For
         * directory buckets, you must make requests for this API operation to the Zonal
         * endpoint. These endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - To use <code>GetObjectAttributes</code>, you must have READ
         * access to the object. The permissions that you need to use this operation depend
         * on whether the bucket is versioned. If the bucket is versioned, you need both
         * the <code>s3:GetObjectVersion</code> and
         * <code>s3:GetObjectVersionAttributes</code> permissions for this operation. If
         * the bucket is not versioned, you need the <code>s3:GetObject</code> and
         * <code>s3:GetObjectAttributes</code> permissions. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a> in the <i>Amazon S3 User Guide</i>. If the object
         * that you request does not exist, the error Amazon S3 returns depends on whether
         * you also have the <code>s3:ListBucket</code> permission.</p> <ul> <li> <p>If you
         * have the <code>s3:ListBucket</code> permission on the bucket, Amazon S3 returns
         * an HTTP status code <code>404 Not Found</code> ("no such key") error.</p> </li>
         * <li> <p>If you don't have the <code>s3:ListBucket</code> permission, Amazon S3
         * returns an HTTP status code <code>403 Forbidden</code> ("access denied")
         * error.</p> </li> </ul> </li> <li> <p> <b>Directory bucket permissions</b> - To
         * grant access to this API operation on a directory bucket, we recommend that you
         * use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> <p>If the object is encrypted with SSE-KMS,
         * you must also have the <code>kms:GenerateDataKey</code> and
         * <code>kms:Decrypt</code> permissions in IAM identity-based policies and KMS key
         * policies for the KMS key.</p> </li> </ul> </dd> <dt>Encryption</dt> <dd> 
         * <p>Encryption request headers, like <code>x-amz-server-side-encryption</code>,
         * should not be sent for <code>HEAD</code> requests if your object uses
         * server-side encryption with Key Management Service (KMS) keys (SSE-KMS),
         * dual-layer server-side encryption with Amazon Web Services KMS keys (DSSE-KMS),
         * or server-side encryption with Amazon S3 managed encryption keys (SSE-S3). The
         * <code>x-amz-server-side-encryption</code> header is used when you
         * <code>PUT</code> an object to S3 and want to specify the encryption method. If
         * you include this header in a <code>GET</code> request for an object that uses
         * these types of keys, you’ll get an HTTP <code>400 Bad Request</code> error. It's
         * because the encryption method can't be changed when you retrieve the object.</p>
         *  <p>If you encrypt an object by using server-side encryption with
         * customer-provided encryption keys (SSE-C) when you store the object in Amazon
         * S3, then when you retrieve the metadata from the object, you must use the
         * following headers to provide the encryption key for the server to be able to
         * retrieve the object's metadata. The headers are: </p> <ul> <li> <p>
         * <code>x-amz-server-side-encryption-customer-algorithm</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-customer-key</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-customer-key-MD5</code> </p> </li> </ul>
         * <p>For more information about SSE-C, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ServerSideEncryptionCustomerKeys.html">Server-Side
         * Encryption (Using Customer-Provided Encryption Keys)</a> in the <i>Amazon S3
         * User Guide</i>.</p>  <p> <b>Directory bucket permissions</b> - For
         * directory buckets, there are only two supported options for server-side
         * encryption: server-side encryption with Amazon S3 managed keys (SSE-S3)
         * (<code>AES256</code>) and server-side encryption with KMS keys (SSE-KMS)
         * (<code>aws:kms</code>). We recommend that the bucket's default encryption uses
         * the desired encryption configuration and you don't override the bucket default
         * encryption in your <code>CreateSession</code> requests or <code>PUT</code>
         * object requests. Then, new objects are automatically encrypted with the desired
         * encryption settings. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-serv-side-encryption.html">Protecting
         * data with server-side encryption</a> in the <i>Amazon S3 User Guide</i>. For
         * more information about the encryption overriding behaviors in directory buckets,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-specifying-kms-encryption.html">Specifying
         * server-side encryption with KMS for new object uploads</a>.</p>  </dd>
         * <dt>Versioning</dt> <dd> <p> <b>Directory buckets</b> - S3 Versioning isn't
         * enabled and supported for directory buckets. For this API operation, only the
         * <code>null</code> value of the version ID is supported by directory buckets. You
         * can only specify <code>null</code> to the <code>versionId</code> query parameter
         * in the request.</p> </dd> <dt>Conditional request headers</dt> <dd> <p>Consider
         * the following when using request headers:</p> <ul> <li> <p>If both of the
         * <code>If-Match</code> and <code>If-Unmodified-Since</code> headers are present
         * in the request as follows, then Amazon S3 returns the HTTP status code <code>200
         * OK</code> and the data requested:</p> <ul> <li> <p> <code>If-Match</code>
         * condition evaluates to <code>true</code>.</p> </li> <li> <p>
         * <code>If-Unmodified-Since</code> condition evaluates to <code>false</code>.</p>
         * </li> </ul> <p>For more information about conditional requests, see <a
         * href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>.</p> </li> <li> <p>If
         * both of the <code>If-None-Match</code> and <code>If-Modified-Since</code>
         * headers are present in the request as follows, then Amazon S3 returns the HTTP
         * status code <code>304 Not Modified</code>:</p> <ul> <li> <p>
         * <code>If-None-Match</code> condition evaluates to <code>false</code>.</p> </li>
         * <li> <p> <code>If-Modified-Since</code> condition evaluates to
         * <code>true</code>.</p> </li> </ul> <p>For more information about conditional
         * requests, see <a href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>.</p>
         * </li> </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets
         * </b> - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following actions are related to
         * <code>GetObjectAttributes</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAcl.html">GetObjectAcl</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectLegalHold.html">GetObjectLegalHold</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectLockConfiguration.html">GetObjectLockConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectRetention.html">GetObjectRetention</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectTagging.html">GetObjectTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadObject.html">HeadObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectAttributes">AWS
         * API Reference</a></p>
         */
        virtual Model::GetObjectAttributesOutcome GetObjectAttributes(const Model::GetObjectAttributesRequest& request) const;

        /**
         * A Callable wrapper for GetObjectAttributes that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectAttributesRequestT = Model::GetObjectAttributesRequest>
        Model::GetObjectAttributesOutcomeCallable GetObjectAttributesCallable(const GetObjectAttributesRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectAttributes, request);
        }

        /**
         * An Async wrapper for GetObjectAttributes that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectAttributesRequestT = Model::GetObjectAttributesRequest>
        void GetObjectAttributesAsync(const GetObjectAttributesRequestT& request, const GetObjectAttributesResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectAttributes, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Gets an object's current legal hold status. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Locking
         * Objects</a>.</p> <p>This functionality is not supported for Amazon S3 on
         * Outposts.</p> <p>The following action is related to
         * <code>GetObjectLegalHold</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectLegalHold">AWS
         * API Reference</a></p>
         */
        virtual Model::GetObjectLegalHoldOutcome GetObjectLegalHold(const Model::GetObjectLegalHoldRequest& request) const;

        /**
         * A Callable wrapper for GetObjectLegalHold that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectLegalHoldRequestT = Model::GetObjectLegalHoldRequest>
        Model::GetObjectLegalHoldOutcomeCallable GetObjectLegalHoldCallable(const GetObjectLegalHoldRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectLegalHold, request);
        }

        /**
         * An Async wrapper for GetObjectLegalHold that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectLegalHoldRequestT = Model::GetObjectLegalHoldRequest>
        void GetObjectLegalHoldAsync(const GetObjectLegalHoldRequestT& request, const GetObjectLegalHoldResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectLegalHold, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Gets the Object Lock configuration for a bucket. The rule specified in the
         * Object Lock configuration will be applied by default to every new object placed
         * in the specified bucket. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Locking
         * Objects</a>.</p> <p>The following action is related to
         * <code>GetObjectLockConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectLockConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::GetObjectLockConfigurationOutcome GetObjectLockConfiguration(const Model::GetObjectLockConfigurationRequest& request) const;

        /**
         * A Callable wrapper for GetObjectLockConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectLockConfigurationRequestT = Model::GetObjectLockConfigurationRequest>
        Model::GetObjectLockConfigurationOutcomeCallable GetObjectLockConfigurationCallable(const GetObjectLockConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectLockConfiguration, request);
        }

        /**
         * An Async wrapper for GetObjectLockConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectLockConfigurationRequestT = Model::GetObjectLockConfigurationRequest>
        void GetObjectLockConfigurationAsync(const GetObjectLockConfigurationRequestT& request, const GetObjectLockConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectLockConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Retrieves an object's retention settings. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Locking
         * Objects</a>.</p> <p>This functionality is not supported for Amazon S3 on
         * Outposts.</p> <p>The following action is related to
         * <code>GetObjectRetention</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectRetention">AWS
         * API Reference</a></p>
         */
        virtual Model::GetObjectRetentionOutcome GetObjectRetention(const Model::GetObjectRetentionRequest& request) const;

        /**
         * A Callable wrapper for GetObjectRetention that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectRetentionRequestT = Model::GetObjectRetentionRequest>
        Model::GetObjectRetentionOutcomeCallable GetObjectRetentionCallable(const GetObjectRetentionRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectRetention, request);
        }

        /**
         * An Async wrapper for GetObjectRetention that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectRetentionRequestT = Model::GetObjectRetentionRequest>
        void GetObjectRetentionAsync(const GetObjectRetentionRequestT& request, const GetObjectRetentionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectRetention, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns the tag-set of an object. You send the GET request against the
         * tagging subresource associated with the object.</p> <p>To use this operation,
         * you must have permission to perform the <code>s3:GetObjectTagging</code> action.
         * By default, the GET action returns information about current version of an
         * object. For a versioned bucket, you can have multiple versions of an object in
         * your bucket. To retrieve tags of any other version, use the versionId query
         * parameter. You also need permission for the
         * <code>s3:GetObjectVersionTagging</code> action.</p> <p> By default, the bucket
         * owner has this permission and can grant this permission to others.</p> <p> For
         * information about the Amazon S3 object tagging feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-tagging.html">Object
         * Tagging</a>.</p> <p>The following actions are related to
         * <code>GetObjectTagging</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObjectTagging.html">DeleteObjectTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObjectTagging.html">PutObjectTagging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectTagging">AWS
         * API Reference</a></p>
         */
        virtual Model::GetObjectTaggingOutcome GetObjectTagging(const Model::GetObjectTaggingRequest& request) const;

        /**
         * A Callable wrapper for GetObjectTagging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectTaggingRequestT = Model::GetObjectTaggingRequest>
        Model::GetObjectTaggingOutcomeCallable GetObjectTaggingCallable(const GetObjectTaggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectTagging, request);
        }

        /**
         * An Async wrapper for GetObjectTagging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectTaggingRequestT = Model::GetObjectTaggingRequest>
        void GetObjectTaggingAsync(const GetObjectTaggingRequestT& request, const GetObjectTaggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectTagging, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns torrent files from a bucket. BitTorrent can save you bandwidth when
         * you're distributing large files.</p>  <p>You can get torrent only for
         * objects that are less than 5 GB in size, and that are not encrypted using
         * server-side encryption with a customer-provided encryption key.</p> 
         * <p>To use GET, you must have READ access to the object.</p> <p>This
         * functionality is not supported for Amazon S3 on Outposts.</p> <p>The following
         * action is related to <code>GetObjectTorrent</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetObjectTorrent">AWS
         * API Reference</a></p>
         */
        virtual Model::GetObjectTorrentOutcome GetObjectTorrent(const Model::GetObjectTorrentRequest& request) const;

        /**
         * A Callable wrapper for GetObjectTorrent that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetObjectTorrentRequestT = Model::GetObjectTorrentRequest>
        Model::GetObjectTorrentOutcomeCallable GetObjectTorrentCallable(const GetObjectTorrentRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetObjectTorrent, request);
        }

        /**
         * An Async wrapper for GetObjectTorrent that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetObjectTorrentRequestT = Model::GetObjectTorrentRequest>
        void GetObjectTorrentAsync(const GetObjectTorrentRequestT& request, const GetObjectTorrentResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetObjectTorrent, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Retrieves the <code>PublicAccessBlock</code> configuration for an Amazon S3
         * bucket. To use this operation, you must have the
         * <code>s3:GetBucketPublicAccessBlock</code> permission. For more information
         * about Amazon S3 permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a>.</p>  <p>When Amazon S3 evaluates the
         * <code>PublicAccessBlock</code> configuration for a bucket or an object, it
         * checks the <code>PublicAccessBlock</code> configuration for both the bucket (or
         * the bucket that contains the object) and the bucket owner's account. If the
         * <code>PublicAccessBlock</code> settings are different between the bucket and the
         * account, Amazon S3 uses the most restrictive combination of the bucket-level and
         * account-level settings.</p>  <p>For more information about when
         * Amazon S3 considers a bucket or an object public, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html#access-control-block-public-access-policy-status">The
         * Meaning of "Public"</a>.</p> <p>The following operations are related to
         * <code>GetPublicAccessBlock</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html">Using
         * Amazon S3 Block Public Access</a> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutPublicAccessBlock.html">PutPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetPublicAccessBlock.html">GetPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeletePublicAccessBlock.html">DeletePublicAccessBlock</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GetPublicAccessBlock">AWS
         * API Reference</a></p>
         */
        virtual Model::GetPublicAccessBlockOutcome GetPublicAccessBlock(const Model::GetPublicAccessBlockRequest& request) const;

        /**
         * A Callable wrapper for GetPublicAccessBlock that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename GetPublicAccessBlockRequestT = Model::GetPublicAccessBlockRequest>
        Model::GetPublicAccessBlockOutcomeCallable GetPublicAccessBlockCallable(const GetPublicAccessBlockRequestT& request) const
        {
            return SubmitCallable(&S3Client::GetPublicAccessBlock, request);
        }

        /**
         * An Async wrapper for GetPublicAccessBlock that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename GetPublicAccessBlockRequestT = Model::GetPublicAccessBlockRequest>
        void GetPublicAccessBlockAsync(const GetPublicAccessBlockRequestT& request, const GetPublicAccessBlockResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::GetPublicAccessBlock, request, handler, context);
        }

        /**
         * <p>You can use this operation to determine if a bucket exists and if you have
         * permission to access it. The action returns a <code>200 OK</code> if the bucket
         * exists and you have permission to access it.</p>  <p>If the bucket does
         * not exist or you do not have permission to access it, the <code>HEAD</code>
         * request returns a generic <code>400 Bad Request</code>, <code>403
         * Forbidden</code> or <code>404 Not Found</code> code. A message body is not
         * included, so you cannot determine the exception beyond these HTTP response
         * codes.</p>  <dl> <dt>Authentication and authorization</dt> <dd> <p>
         * <b>General purpose buckets</b> - Request to public buckets that grant the
         * s3:ListBucket permission publicly do not need to be signed. All other
         * <code>HeadBucket</code> requests must be authenticated and signed by using IAM
         * credentials (access key ID and secret access key for the IAM identities). All
         * headers with the <code>x-amz-</code> prefix, including
         * <code>x-amz-copy-source</code>, must be signed. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/RESTAuthentication.html">REST
         * Authentication</a>.</p> <p> <b>Directory buckets</b> - You must use IAM
         * credentials to authenticate and authorize your access to the
         * <code>HeadBucket</code> API operation, instead of using the temporary security
         * credentials through the <code>CreateSession</code> API operation.</p> <p>Amazon
         * Web Services CLI or SDKs handles authentication and authorization on your
         * behalf.</p> </dd> <dt>Permissions</dt> <dd> <p/> <ul> <li> <p> <b>General
         * purpose bucket permissions</b> - To use this operation, you must have
         * permissions to perform the <code>s3:ListBucket</code> action. The bucket owner
         * has this permission by default and can grant this permission to others. For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * access permissions to your Amazon S3 resources</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </li> <li> <p> <b>Directory bucket permissions</b> - You must
         * have the <b> <code>s3express:CreateSession</code> </b> permission in the
         * <code>Action</code> element of a policy. By default, the session is in the
         * <code>ReadWrite</code> mode. If you want to restrict the access, you can
         * explicitly set the <code>s3express:SessionMode</code> condition key to
         * <code>ReadOnly</code> on the bucket.</p> <p>For more information about example
         * bucket policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-example-bucket-policies.html">Example
         * bucket policies for S3 Express One Zone</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-identity-policies.html">Amazon
         * Web Services Identity and Access Management (IAM) identity-based policies for S3
         * Express One Zone</a> in the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd>
         * <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP
         * Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         *  <p>You must make requests for this API operation to the Zonal endpoint.
         * These endpoints support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.
         * Path-style requests are not supported. For more information about endpoints in
         * Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  </dd> </dl><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/HeadBucket">AWS API
         * Reference</a></p>
         */
        virtual Model::HeadBucketOutcome HeadBucket(const Model::HeadBucketRequest& request) const;

        /**
         * A Callable wrapper for HeadBucket that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename HeadBucketRequestT = Model::HeadBucketRequest>
        Model::HeadBucketOutcomeCallable HeadBucketCallable(const HeadBucketRequestT& request) const
        {
            return SubmitCallable(&S3Client::HeadBucket, request);
        }

        /**
         * An Async wrapper for HeadBucket that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename HeadBucketRequestT = Model::HeadBucketRequest>
        void HeadBucketAsync(const HeadBucketRequestT& request, const HeadBucketResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::HeadBucket, request, handler, context);
        }

        /**
         * <p>The <code>HEAD</code> operation retrieves metadata from an object without
         * returning the object itself. This operation is useful if you're interested only
         * in an object's metadata.</p>  <p>A <code>HEAD</code> request has the same
         * options as a <code>GET</code> operation on an object. The response is identical
         * to the <code>GET</code> response except that there is no response body. Because
         * of this, if the <code>HEAD</code> request generates an error, it returns a
         * generic code, such as <code>400 Bad Request</code>, <code>403 Forbidden</code>,
         * <code>404 Not Found</code>, <code>405 Method Not Allowed</code>, <code>412
         * Precondition Failed</code>, or <code>304 Not Modified</code>. It's not possible
         * to retrieve the exact exception of these error codes.</p>  <p>Request
         * headers are limited to 8 KB in size. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTCommonRequestHeaders.html">Common
         * Request Headers</a>.</p> <dl> <dt>Permissions</dt> <dd> <p/> <ul> <li> <p>
         * <b>General purpose bucket permissions</b> - To use <code>HEAD</code>, you must
         * have the <code>s3:GetObject</code> permission. You need the relevant read object
         * (or version) permission for this operation. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/list_amazons3.html">Actions,
         * resources, and condition keys for Amazon S3</a> in the <i>Amazon S3 User
         * Guide</i>. For more information about the permissions to S3 API operations by S3
         * resource types, see <a
         * href="/AmazonS3/latest/userguide/using-with-s3-policy-actions.html">Required
         * permissions for Amazon S3 API operations</a> in the <i>Amazon S3 User
         * Guide</i>.</p> <p>If the object you request doesn't exist, the error that Amazon
         * S3 returns depends on whether you also have the <code>s3:ListBucket</code>
         * permission.</p> <ul> <li> <p>If you have the <code>s3:ListBucket</code>
         * permission on the bucket, Amazon S3 returns an HTTP status code <code>404 Not
         * Found</code> error.</p> </li> <li> <p>If you don’t have the
         * <code>s3:ListBucket</code> permission, Amazon S3 returns an HTTP status code
         * <code>403 Forbidden</code> error.</p> </li> </ul> </li> <li> <p> <b>Directory
         * bucket permissions</b> - To grant access to this API operation on a directory
         * bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> <p>If you enable
         * <code>x-amz-checksum-mode</code> in the request and the object is encrypted with
         * Amazon Web Services Key Management Service (Amazon Web Services KMS), you must
         * also have the <code>kms:GenerateDataKey</code> and <code>kms:Decrypt</code>
         * permissions in IAM identity-based policies and KMS key policies for the KMS key
         * to retrieve the checksum of the object.</p> </li> </ul> </dd>
         * <dt>Encryption</dt> <dd>  <p>Encryption request headers, like
         * <code>x-amz-server-side-encryption</code>, should not be sent for
         * <code>HEAD</code> requests if your object uses server-side encryption with Key
         * Management Service (KMS) keys (SSE-KMS), dual-layer server-side encryption with
         * Amazon Web Services KMS keys (DSSE-KMS), or server-side encryption with Amazon
         * S3 managed encryption keys (SSE-S3). The
         * <code>x-amz-server-side-encryption</code> header is used when you
         * <code>PUT</code> an object to S3 and want to specify the encryption method. If
         * you include this header in a <code>HEAD</code> request for an object that uses
         * these types of keys, you’ll get an HTTP <code>400 Bad Request</code> error. It's
         * because the encryption method can't be changed when you retrieve the object.</p>
         *  <p>If you encrypt an object by using server-side encryption with
         * customer-provided encryption keys (SSE-C) when you store the object in Amazon
         * S3, then when you retrieve the metadata from the object, you must use the
         * following headers to provide the encryption key for the server to be able to
         * retrieve the object's metadata. The headers are: </p> <ul> <li> <p>
         * <code>x-amz-server-side-encryption-customer-algorithm</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-customer-key</code> </p> </li> <li> <p>
         * <code>x-amz-server-side-encryption-customer-key-MD5</code> </p> </li> </ul>
         * <p>For more information about SSE-C, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ServerSideEncryptionCustomerKeys.html">Server-Side
         * Encryption (Using Customer-Provided Encryption Keys)</a> in the <i>Amazon S3
         * User Guide</i>.</p>  <p> <b>Directory bucket </b> - For directory buckets,
         * there are only two supported options for server-side encryption: SSE-S3 and
         * SSE-KMS. SSE-C isn't supported. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-serv-side-encryption.html">Protecting
         * data with server-side encryption</a> in the <i>Amazon S3 User Guide</i>. </p>
         *  </dd> <dt>Versioning</dt> <dd> <ul> <li> <p>If the current version of
         * the object is a delete marker, Amazon S3 behaves as if the object was deleted
         * and includes <code>x-amz-delete-marker: true</code> in the response.</p> </li>
         * <li> <p>If the specified version is a delete marker, the response returns a
         * <code>405 Method Not Allowed</code> error and the <code>Last-Modified:
         * timestamp</code> response header.</p> </li> </ul>  <ul> <li> <p>
         * <b>Directory buckets</b> - Delete marker is not supported for directory
         * buckets.</p> </li> <li> <p> <b>Directory buckets</b> - S3 Versioning isn't
         * enabled and supported for directory buckets. For this API operation, only the
         * <code>null</code> value of the version ID is supported by directory buckets. You
         * can only specify <code>null</code> to the <code>versionId</code> query parameter
         * in the request.</p> </li> </ul>  </dd> <dt>HTTP Host header syntax</dt>
         * <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         *  <p>For directory buckets, you must make requests for this API operation
         * to the Zonal endpoint. These endpoints support virtual-hosted-style requests in
         * the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  </dd> </dl> <p>The following actions are related to
         * <code>HeadObject</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/HeadObject">AWS API
         * Reference</a></p>
         */
        virtual Model::HeadObjectOutcome HeadObject(const Model::HeadObjectRequest& request) const;

        /**
         * A Callable wrapper for HeadObject that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename HeadObjectRequestT = Model::HeadObjectRequest>
        Model::HeadObjectOutcomeCallable HeadObjectCallable(const HeadObjectRequestT& request) const
        {
            return SubmitCallable(&S3Client::HeadObject, request);
        }

        /**
         * An Async wrapper for HeadObject that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename HeadObjectRequestT = Model::HeadObjectRequest>
        void HeadObjectAsync(const HeadObjectRequestT& request, const HeadObjectResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::HeadObject, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Lists the analytics configurations for the bucket. You can have up to 1,000
         * analytics configurations per bucket.</p> <p>This action supports list pagination
         * and does not return more than 100 configurations at a time. You should always
         * check the <code>IsTruncated</code> element in the response. If there are no more
         * configurations to list, <code>IsTruncated</code> is set to false. If there are
         * more configurations to list, <code>IsTruncated</code> is set to true, and there
         * will be a value in <code>NextContinuationToken</code>. You use the
         * <code>NextContinuationToken</code> value to continue the pagination of the list
         * by passing the value in continuation-token in the request to <code>GET</code>
         * the next page.</p> <p>To use this operation, you must have permissions to
         * perform the <code>s3:GetAnalyticsConfiguration</code> action. The bucket owner
         * has this permission by default. The bucket owner can grant this permission to
         * others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * Amazon S3 analytics feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/analytics-storage-class.html">Amazon
         * S3 Analytics – Storage Class Analysis</a>. </p> <p>The following operations are
         * related to <code>ListBucketAnalyticsConfigurations</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketAnalyticsConfiguration.html">GetBucketAnalyticsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketAnalyticsConfiguration.html">DeleteBucketAnalyticsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketAnalyticsConfiguration.html">PutBucketAnalyticsConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListBucketAnalyticsConfigurations">AWS
         * API Reference</a></p>
         */
        virtual Model::ListBucketAnalyticsConfigurationsOutcome ListBucketAnalyticsConfigurations(const Model::ListBucketAnalyticsConfigurationsRequest& request) const;

        /**
         * A Callable wrapper for ListBucketAnalyticsConfigurations that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListBucketAnalyticsConfigurationsRequestT = Model::ListBucketAnalyticsConfigurationsRequest>
        Model::ListBucketAnalyticsConfigurationsOutcomeCallable ListBucketAnalyticsConfigurationsCallable(const ListBucketAnalyticsConfigurationsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListBucketAnalyticsConfigurations, request);
        }

        /**
         * An Async wrapper for ListBucketAnalyticsConfigurations that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListBucketAnalyticsConfigurationsRequestT = Model::ListBucketAnalyticsConfigurationsRequest>
        void ListBucketAnalyticsConfigurationsAsync(const ListBucketAnalyticsConfigurationsRequestT& request, const ListBucketAnalyticsConfigurationsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListBucketAnalyticsConfigurations, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Lists the S3 Intelligent-Tiering configuration from the specified bucket.</p>
         * <p>The S3 Intelligent-Tiering storage class is designed to optimize storage
         * costs by automatically moving data to the most cost-effective storage access
         * tier, without performance impact or operational overhead. S3 Intelligent-Tiering
         * delivers automatic cost savings in three low latency and high throughput access
         * tiers. To get the lowest storage cost on data that can be accessed in minutes to
         * hours, you can choose to activate additional archiving capabilities.</p> <p>The
         * S3 Intelligent-Tiering storage class is the ideal storage class for data with
         * unknown, changing, or unpredictable access patterns, independent of object size
         * or retention period. If the size of an object is less than 128 KB, it is not
         * monitored and not eligible for auto-tiering. Smaller objects can be stored, but
         * they are always charged at the Frequent Access tier rates in the S3
         * Intelligent-Tiering storage class.</p> <p>For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html#sc-dynamic-data-access">Storage
         * class for automatically optimizing frequently and infrequently accessed
         * objects</a>.</p> <p>Operations related to
         * <code>ListBucketIntelligentTieringConfigurations</code> include: </p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketIntelligentTieringConfiguration.html">DeleteBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketIntelligentTieringConfiguration.html">PutBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketIntelligentTieringConfiguration.html">GetBucketIntelligentTieringConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListBucketIntelligentTieringConfigurations">AWS
         * API Reference</a></p>
         */
        virtual Model::ListBucketIntelligentTieringConfigurationsOutcome ListBucketIntelligentTieringConfigurations(const Model::ListBucketIntelligentTieringConfigurationsRequest& request) const;

        /**
         * A Callable wrapper for ListBucketIntelligentTieringConfigurations that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListBucketIntelligentTieringConfigurationsRequestT = Model::ListBucketIntelligentTieringConfigurationsRequest>
        Model::ListBucketIntelligentTieringConfigurationsOutcomeCallable ListBucketIntelligentTieringConfigurationsCallable(const ListBucketIntelligentTieringConfigurationsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListBucketIntelligentTieringConfigurations, request);
        }

        /**
         * An Async wrapper for ListBucketIntelligentTieringConfigurations that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListBucketIntelligentTieringConfigurationsRequestT = Model::ListBucketIntelligentTieringConfigurationsRequest>
        void ListBucketIntelligentTieringConfigurationsAsync(const ListBucketIntelligentTieringConfigurationsRequestT& request, const ListBucketIntelligentTieringConfigurationsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListBucketIntelligentTieringConfigurations, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns a list of inventory configurations for the bucket. You can have up to
         * 1,000 analytics configurations per bucket.</p> <p>This action supports list
         * pagination and does not return more than 100 configurations at a time. Always
         * check the <code>IsTruncated</code> element in the response. If there are no more
         * configurations to list, <code>IsTruncated</code> is set to false. If there are
         * more configurations to list, <code>IsTruncated</code> is set to true, and there
         * is a value in <code>NextContinuationToken</code>. You use the
         * <code>NextContinuationToken</code> value to continue the pagination of the list
         * by passing the value in continuation-token in the request to <code>GET</code>
         * the next page.</p> <p> To use this operation, you must have permissions to
         * perform the <code>s3:GetInventoryConfiguration</code> action. The bucket owner
         * has this permission by default. The bucket owner can grant this permission to
         * others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * the Amazon S3 inventory feature, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-inventory.html">Amazon
         * S3 Inventory</a> </p> <p>The following operations are related to
         * <code>ListBucketInventoryConfigurations</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketInventoryConfiguration.html">GetBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketInventoryConfiguration.html">DeleteBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketInventoryConfiguration.html">PutBucketInventoryConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListBucketInventoryConfigurations">AWS
         * API Reference</a></p>
         */
        virtual Model::ListBucketInventoryConfigurationsOutcome ListBucketInventoryConfigurations(const Model::ListBucketInventoryConfigurationsRequest& request) const;

        /**
         * A Callable wrapper for ListBucketInventoryConfigurations that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListBucketInventoryConfigurationsRequestT = Model::ListBucketInventoryConfigurationsRequest>
        Model::ListBucketInventoryConfigurationsOutcomeCallable ListBucketInventoryConfigurationsCallable(const ListBucketInventoryConfigurationsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListBucketInventoryConfigurations, request);
        }

        /**
         * An Async wrapper for ListBucketInventoryConfigurations that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListBucketInventoryConfigurationsRequestT = Model::ListBucketInventoryConfigurationsRequest>
        void ListBucketInventoryConfigurationsAsync(const ListBucketInventoryConfigurationsRequestT& request, const ListBucketInventoryConfigurationsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListBucketInventoryConfigurations, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Lists the metrics configurations for the bucket. The metrics configurations
         * are only for the request metrics of the bucket and do not provide information on
         * daily storage metrics. You can have up to 1,000 configurations per bucket.</p>
         * <p>This action supports list pagination and does not return more than 100
         * configurations at a time. Always check the <code>IsTruncated</code> element in
         * the response. If there are no more configurations to list,
         * <code>IsTruncated</code> is set to false. If there are more configurations to
         * list, <code>IsTruncated</code> is set to true, and there is a value in
         * <code>NextContinuationToken</code>. You use the
         * <code>NextContinuationToken</code> value to continue the pagination of the list
         * by passing the value in <code>continuation-token</code> in the request to
         * <code>GET</code> the next page.</p> <p>To use this operation, you must have
         * permissions to perform the <code>s3:GetMetricsConfiguration</code> action. The
         * bucket owner has this permission by default. The bucket owner can grant this
         * permission to others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For more information
         * about metrics configurations and CloudWatch request metrics, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cloudwatch-monitoring.html">Monitoring
         * Metrics with Amazon CloudWatch</a>.</p> <p>The following operations are related
         * to <code>ListBucketMetricsConfigurations</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketMetricsConfiguration.html">PutBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketMetricsConfiguration.html">GetBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketMetricsConfiguration.html">DeleteBucketMetricsConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListBucketMetricsConfigurations">AWS
         * API Reference</a></p>
         */
        virtual Model::ListBucketMetricsConfigurationsOutcome ListBucketMetricsConfigurations(const Model::ListBucketMetricsConfigurationsRequest& request) const;

        /**
         * A Callable wrapper for ListBucketMetricsConfigurations that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListBucketMetricsConfigurationsRequestT = Model::ListBucketMetricsConfigurationsRequest>
        Model::ListBucketMetricsConfigurationsOutcomeCallable ListBucketMetricsConfigurationsCallable(const ListBucketMetricsConfigurationsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListBucketMetricsConfigurations, request);
        }

        /**
         * An Async wrapper for ListBucketMetricsConfigurations that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListBucketMetricsConfigurationsRequestT = Model::ListBucketMetricsConfigurationsRequest>
        void ListBucketMetricsConfigurationsAsync(const ListBucketMetricsConfigurationsRequestT& request, const ListBucketMetricsConfigurationsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListBucketMetricsConfigurations, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns a list of all buckets owned by the authenticated sender of the
         * request. To grant IAM permission to use this operation, you must add the
         * <code>s3:ListAllMyBuckets</code> policy action. </p> <p>For information about
         * Amazon S3 buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/creating-buckets-s3.html">Creating,
         * configuring, and working with Amazon S3 buckets</a>.</p>  <p>We
         * strongly recommend using only paginated <code>ListBuckets</code> requests.
         * Unpaginated <code>ListBuckets</code> requests are only supported for Amazon Web
         * Services accounts set to the default general purpose bucket quota of 10,000. If
         * you have an approved general purpose bucket quota above 10,000, you must send
         * paginated <code>ListBuckets</code> requests to list your account’s buckets. All
         * unpaginated <code>ListBuckets</code> requests will be rejected for Amazon Web
         * Services accounts with a general purpose bucket quota greater than 10,000. </p>
         * <p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListBuckets">AWS API
         * Reference</a></p>
         */
        virtual Model::ListBucketsOutcome ListBuckets(const Model::ListBucketsRequest& request = {}) const;

        /**
         * A Callable wrapper for ListBuckets that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListBucketsRequestT = Model::ListBucketsRequest>
        Model::ListBucketsOutcomeCallable ListBucketsCallable(const ListBucketsRequestT& request = {}) const
        {
            return SubmitCallable(&S3Client::ListBuckets, request);
        }

        /**
         * An Async wrapper for ListBuckets that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListBucketsRequestT = Model::ListBucketsRequest>
        void ListBucketsAsync(const ListBucketsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const ListBucketsRequestT& request = {}) const
        {
            return SubmitAsync(&S3Client::ListBuckets, request, handler, context);
        }

        /**
         * <p>Returns a list of all Amazon S3 directory buckets owned by the authenticated
         * sender of the request. For more information about directory buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-buckets-overview.html">Directory
         * buckets</a> in the <i>Amazon S3 User Guide</i>.</p>  <p> <b>Directory
         * buckets </b> - For directory buckets, you must make requests for this API
         * operation to the Regional endpoint. These endpoints support path-style requests
         * in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <p>You must have the
         * <code>s3express:ListAllMyDirectoryBuckets</code> permission in an IAM
         * identity-based policy instead of a bucket policy. Cross-account access to this
         * API operation isn't supported. This operation can only be performed by the
         * Amazon Web Services account that owns the resource. For more information about
         * directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </dd> <dt>HTTP Host header syntax</dt> <dd>
         * <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region</i>.amazonaws.com</code>.</p> </dd> </dl>
         *  <p> The <code>BucketRegion</code> response element is not part of the
         * <code>ListDirectoryBuckets</code> Response Syntax.</p> <p><h3>See
         * Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListDirectoryBuckets">AWS
         * API Reference</a></p>
         */
        virtual Model::ListDirectoryBucketsOutcome ListDirectoryBuckets(const Model::ListDirectoryBucketsRequest& request = {}) const;

        /**
         * A Callable wrapper for ListDirectoryBuckets that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListDirectoryBucketsRequestT = Model::ListDirectoryBucketsRequest>
        Model::ListDirectoryBucketsOutcomeCallable ListDirectoryBucketsCallable(const ListDirectoryBucketsRequestT& request = {}) const
        {
            return SubmitCallable(&S3Client::ListDirectoryBuckets, request);
        }

        /**
         * An Async wrapper for ListDirectoryBuckets that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListDirectoryBucketsRequestT = Model::ListDirectoryBucketsRequest>
        void ListDirectoryBucketsAsync(const ListDirectoryBucketsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr, const ListDirectoryBucketsRequestT& request = {}) const
        {
            return SubmitAsync(&S3Client::ListDirectoryBuckets, request, handler, context);
        }

        /**
         * <p>This operation lists in-progress multipart uploads in a bucket. An
         * in-progress multipart upload is a multipart upload that has been initiated by
         * the <code>CreateMultipartUpload</code> request, but has not yet been completed
         * or aborted.</p>  <p> <b>Directory buckets</b> - If multipart uploads in a
         * directory bucket are in progress, you can't delete the bucket until all the
         * in-progress multipart uploads are aborted or completed. To delete these
         * in-progress multipart uploads, use the <code>ListMultipartUploads</code>
         * operation to list the in-progress multipart uploads in the bucket and use the
         * <code>AbortMultipartUpload</code> operation to abort all the in-progress
         * multipart uploads. </p>  <p>The <code>ListMultipartUploads</code>
         * operation returns a maximum of 1,000 multipart uploads in the response. The
         * limit of 1,000 multipart uploads is also the default value. You can further
         * limit the number of uploads in a response by specifying the
         * <code>max-uploads</code> request parameter. If there are more than 1,000
         * multipart uploads that satisfy your <code>ListMultipartUploads</code> request,
         * the response returns an <code>IsTruncated</code> element with the value of
         * <code>true</code>, a <code>NextKeyMarker</code> element, and a
         * <code>NextUploadIdMarker</code> element. To list the remaining multipart
         * uploads, you need to make subsequent <code>ListMultipartUploads</code> requests.
         * In these requests, include two query parameters: <code>key-marker</code> and
         * <code>upload-id-marker</code>. Set the value of <code>key-marker</code> to the
         * <code>NextKeyMarker</code> value from the previous response. Similarly, set the
         * value of <code>upload-id-marker</code> to the <code>NextUploadIdMarker</code>
         * value from the previous response.</p>  <p> <b>Directory buckets</b> - The
         * <code>upload-id-marker</code> element and the <code>NextUploadIdMarker</code>
         * element aren't supported by directory buckets. To list the additional multipart
         * uploads, you only need to set the value of <code>key-marker</code> to the
         * <code>NextKeyMarker</code> value from the previous response. </p>  <p>For
         * more information about multipart uploads, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/uploadobjusingmpu.html">Uploading
         * Objects Using Multipart Upload</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p> <b>Directory buckets</b> - For directory buckets, you must make
         * requests for this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - For information about permissions required to use the
         * multipart upload API, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuAndPermissions.html">Multipart
         * Upload and Permissions</a> in the <i>Amazon S3 User Guide</i>.</p> </li> <li>
         * <p> <b>Directory bucket permissions</b> - To grant access to this API operation
         * on a directory bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>Sorting of multipart
         * uploads in response</dt> <dd> <ul> <li> <p> <b>General purpose bucket</b> - In
         * the <code>ListMultipartUploads</code> response, the multipart uploads are sorted
         * based on two criteria:</p> <ul> <li> <p>Key-based sorting - Multipart uploads
         * are initially sorted in ascending order based on their object keys.</p> </li>
         * <li> <p>Time-based sorting - For uploads that share the same object key, they
         * are further sorted in ascending order based on the upload initiation time. Among
         * uploads with the same key, the one that was initiated first will appear before
         * the ones that were initiated later.</p> </li> </ul> </li> <li> <p> <b>Directory
         * bucket</b> - In the <code>ListMultipartUploads</code> response, the multipart
         * uploads aren't sorted lexicographically based on the object keys. </p> </li>
         * </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets </b>
         * - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>ListMultipartUploads</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListMultipartUploads">AWS
         * API Reference</a></p>
         */
        virtual Model::ListMultipartUploadsOutcome ListMultipartUploads(const Model::ListMultipartUploadsRequest& request) const;

        /**
         * A Callable wrapper for ListMultipartUploads that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListMultipartUploadsRequestT = Model::ListMultipartUploadsRequest>
        Model::ListMultipartUploadsOutcomeCallable ListMultipartUploadsCallable(const ListMultipartUploadsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListMultipartUploads, request);
        }

        /**
         * An Async wrapper for ListMultipartUploads that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListMultipartUploadsRequestT = Model::ListMultipartUploadsRequest>
        void ListMultipartUploadsAsync(const ListMultipartUploadsRequestT& request, const ListMultipartUploadsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListMultipartUploads, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns metadata about all versions of the objects in a bucket. You can also
         * use request parameters as selection criteria to return metadata about a subset
         * of all the object versions.</p>  <p> To use this operation, you must
         * have permission to perform the <code>s3:ListBucketVersions</code> action. Be
         * aware of the name difference. </p>   <p> A <code>200 OK</code>
         * response can contain valid or invalid XML. Make sure to design your application
         * to parse the contents of the response and handle it appropriately.</p> 
         * <p>To use this operation, you must have READ access to the bucket.</p> <p>The
         * following operations are related to <code>ListObjectVersions</code>:</p> <ul>
         * <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjectsV2.html">ListObjectsV2</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html">DeleteObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListObjectVersions">AWS
         * API Reference</a></p>
         */
        virtual Model::ListObjectVersionsOutcome ListObjectVersions(const Model::ListObjectVersionsRequest& request) const;

        /**
         * A Callable wrapper for ListObjectVersions that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListObjectVersionsRequestT = Model::ListObjectVersionsRequest>
        Model::ListObjectVersionsOutcomeCallable ListObjectVersionsCallable(const ListObjectVersionsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListObjectVersions, request);
        }

        /**
         * An Async wrapper for ListObjectVersions that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListObjectVersionsRequestT = Model::ListObjectVersionsRequest>
        void ListObjectVersionsAsync(const ListObjectVersionsRequestT& request, const ListObjectVersionsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListObjectVersions, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Returns some or all (up to 1,000) of the objects in a bucket. You can use the
         * request parameters as selection criteria to return a subset of the objects in a
         * bucket. A 200 OK response can contain valid or invalid XML. Be sure to design
         * your application to parse the contents of the response and handle it
         * appropriately.</p>  <p>This action has been revised. We recommend
         * that you use the newer version, <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjectsV2.html">ListObjectsV2</a>,
         * when developing applications. For backward compatibility, Amazon S3 continues to
         * support <code>ListObjects</code>.</p>  <p>The following operations
         * are related to <code>ListObjects</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjectsV2.html">ListObjectsV2</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBuckets.html">ListBuckets</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListObjects">AWS API
         * Reference</a></p>
         */
        virtual Model::ListObjectsOutcome ListObjects(const Model::ListObjectsRequest& request) const;

        /**
         * A Callable wrapper for ListObjects that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListObjectsRequestT = Model::ListObjectsRequest>
        Model::ListObjectsOutcomeCallable ListObjectsCallable(const ListObjectsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListObjects, request);
        }

        /**
         * An Async wrapper for ListObjects that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListObjectsRequestT = Model::ListObjectsRequest>
        void ListObjectsAsync(const ListObjectsRequestT& request, const ListObjectsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListObjects, request, handler, context);
        }

        /**
         * <p>Returns some or all (up to 1,000) of the objects in a bucket with each
         * request. You can use the request parameters as selection criteria to return a
         * subset of the objects in a bucket. A <code>200 OK</code> response can contain
         * valid or invalid XML. Make sure to design your application to parse the contents
         * of the response and handle it appropriately. For more information about listing
         * objects, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/ListingKeysUsingAPIs.html">Listing
         * object keys programmatically</a> in the <i>Amazon S3 User Guide</i>. To get a
         * list of your buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBuckets.html">ListBuckets</a>.</p>
         *  <ul> <li> <p> <b>General purpose bucket</b> - For general purpose
         * buckets, <code>ListObjectsV2</code> doesn't return prefixes that are related
         * only to in-progress multipart uploads.</p> </li> <li> <p> <b>Directory
         * buckets</b> - For directory buckets, <code>ListObjectsV2</code> response
         * includes the prefixes that are related only to in-progress multipart uploads.
         * </p> </li> <li> <p> <b>Directory buckets</b> - For directory buckets, you must
         * make requests for this API operation to the Zonal endpoint. These endpoints
         * support virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General
         * purpose bucket permissions</b> - To use this operation, you must have READ
         * access to the bucket. You must have permission to perform the
         * <code>s3:ListBucket</code> action. The bucket owner has this permission by
         * default and can grant this permission to others. For more information about
         * permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </li> <li> <p> <b>Directory bucket permissions</b> - To grant
         * access to this API operation on a directory bucket, we recommend that you use
         * the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>Sorting order of
         * returned objects</dt> <dd> <ul> <li> <p> <b>General purpose bucket</b> - For
         * general purpose buckets, <code>ListObjectsV2</code> returns objects in
         * lexicographical order based on their key names.</p> </li> <li> <p> <b>Directory
         * bucket</b> - For directory buckets, <code>ListObjectsV2</code> does not return
         * objects in lexicographical order.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl>  <p>This section describes the latest revision of this
         * action. We recommend that you use this revised API operation for application
         * development. For backward compatibility, Amazon S3 continues to support the
         * prior version of this API operation, <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjects.html">ListObjects</a>.</p>
         *  <p>The following operations are related to
         * <code>ListObjectsV2</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListObjectsV2">AWS
         * API Reference</a></p>
         */
        virtual Model::ListObjectsV2Outcome ListObjectsV2(const Model::ListObjectsV2Request& request) const;

        /**
         * A Callable wrapper for ListObjectsV2 that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListObjectsV2RequestT = Model::ListObjectsV2Request>
        Model::ListObjectsV2OutcomeCallable ListObjectsV2Callable(const ListObjectsV2RequestT& request) const
        {
            return SubmitCallable(&S3Client::ListObjectsV2, request);
        }

        /**
         * An Async wrapper for ListObjectsV2 that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListObjectsV2RequestT = Model::ListObjectsV2Request>
        void ListObjectsV2Async(const ListObjectsV2RequestT& request, const ListObjectsV2ResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListObjectsV2, request, handler, context);
        }

        /**
         * <p>Lists the parts that have been uploaded for a specific multipart upload.</p>
         * <p>To use this operation, you must provide the <code>upload ID</code> in the
         * request. You obtain this uploadID by sending the initiate multipart upload
         * request through <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>.</p>
         * <p>The <code>ListParts</code> request returns a maximum of 1,000 uploaded parts.
         * The limit of 1,000 parts is also the default value. You can restrict the number
         * of parts in a response by specifying the <code>max-parts</code> request
         * parameter. If your multipart upload consists of more than 1,000 parts, the
         * response returns an <code>IsTruncated</code> field with the value of
         * <code>true</code>, and a <code>NextPartNumberMarker</code> element. To list
         * remaining uploaded parts, in subsequent <code>ListParts</code> requests, include
         * the <code>part-number-marker</code> query string parameter and set its value to
         * the <code>NextPartNumberMarker</code> field value from the previous
         * response.</p> <p>For more information on multipart uploads, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/uploadobjusingmpu.html">Uploading
         * Objects Using Multipart Upload</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p> <b>Directory buckets</b> - For directory buckets, you must make
         * requests for this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - For information about permissions required to use the
         * multipart upload API, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuAndPermissions.html">Multipart
         * Upload and Permissions</a> in the <i>Amazon S3 User Guide</i>.</p> <p>If the
         * upload was created using server-side encryption with Key Management Service
         * (KMS) keys (SSE-KMS) or dual-layer server-side encryption with Amazon Web
         * Services KMS keys (DSSE-KMS), you must have permission to the
         * <code>kms:Decrypt</code> action for the <code>ListParts</code> request to
         * succeed.</p> </li> <li> <p> <b>Directory bucket permissions</b> - To grant
         * access to this API operation on a directory bucket, we recommend that you use
         * the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> </li> </ul> </dd> <dt>HTTP Host header
         * syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header syntax is
         * <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>ListParts</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAttributes.html">GetObjectAttributes</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListMultipartUploads.html">ListMultipartUploads</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ListParts">AWS API
         * Reference</a></p>
         */
        virtual Model::ListPartsOutcome ListParts(const Model::ListPartsRequest& request) const;

        /**
         * A Callable wrapper for ListParts that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename ListPartsRequestT = Model::ListPartsRequest>
        Model::ListPartsOutcomeCallable ListPartsCallable(const ListPartsRequestT& request) const
        {
            return SubmitCallable(&S3Client::ListParts, request);
        }

        /**
         * An Async wrapper for ListParts that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename ListPartsRequestT = Model::ListPartsRequest>
        void ListPartsAsync(const ListPartsRequestT& request, const ListPartsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::ListParts, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the accelerate configuration of an existing bucket. Amazon S3 Transfer
         * Acceleration is a bucket-level feature that enables you to perform faster data
         * transfers to Amazon S3.</p> <p> To use this operation, you must have permission
         * to perform the <code>s3:PutAccelerateConfiguration</code> action. The bucket
         * owner has this permission by default. The bucket owner can grant this permission
         * to others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p> The Transfer
         * Acceleration state of a bucket can be set to one of the following two
         * values:</p> <ul> <li> <p> Enabled – Enables accelerated data transfers to the
         * bucket.</p> </li> <li> <p> Suspended – Disables accelerated data transfers to
         * the bucket.</p> </li> </ul> <p>The <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketAccelerateConfiguration.html">GetBucketAccelerateConfiguration</a>
         * action returns the transfer acceleration state of a bucket.</p> <p>After setting
         * the Transfer Acceleration state of a bucket to Enabled, it might take up to
         * thirty minutes before the data transfer rates to the bucket increase.</p> <p>
         * The name of the bucket used for Transfer Acceleration must be DNS-compliant and
         * must not contain periods (".").</p> <p> For more information about transfer
         * acceleration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/transfer-acceleration.html">Transfer
         * Acceleration</a>.</p> <p>The following operations are related to
         * <code>PutBucketAccelerateConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketAccelerateConfiguration.html">GetBucketAccelerateConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketAccelerateConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketAccelerateConfigurationOutcome PutBucketAccelerateConfiguration(const Model::PutBucketAccelerateConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketAccelerateConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketAccelerateConfigurationRequestT = Model::PutBucketAccelerateConfigurationRequest>
        Model::PutBucketAccelerateConfigurationOutcomeCallable PutBucketAccelerateConfigurationCallable(const PutBucketAccelerateConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketAccelerateConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketAccelerateConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketAccelerateConfigurationRequestT = Model::PutBucketAccelerateConfigurationRequest>
        void PutBucketAccelerateConfigurationAsync(const PutBucketAccelerateConfigurationRequestT& request, const PutBucketAccelerateConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketAccelerateConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the permissions on an existing bucket using access control lists (ACL).
         * For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/S3_ACLs_UsingACLs.html">Using
         * ACLs</a>. To set the ACL of a bucket, you must have the <code>WRITE_ACP</code>
         * permission.</p> <p>You can use one of the following two ways to set a bucket's
         * permissions:</p> <ul> <li> <p>Specify the ACL in the request body</p> </li> <li>
         * <p>Specify permissions using request headers</p> </li> </ul>  <p>You
         * cannot specify access permission using both the body and the request
         * headers.</p>  <p>Depending on your application needs, you may choose to
         * set the ACL on a bucket using either the request body or the headers. For
         * example, if you have an existing application that updates a bucket ACL using the
         * request body, then you can continue to use that approach.</p>  <p>If
         * your bucket uses the bucket owner enforced setting for S3 Object Ownership, ACLs
         * are disabled and no longer affect permissions. You must use policies to grant
         * access to your bucket and the objects in it. Requests to set ACLs or update ACLs
         * fail and return the <code>AccessControlListNotSupported</code> error code.
         * Requests to read ACLs are still supported. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html">Controlling
         * object ownership</a> in the <i>Amazon S3 User Guide</i>.</p>  <dl>
         * <dt>Permissions</dt> <dd> <p>You can set access permissions by using one of the
         * following methods:</p> <ul> <li> <p>Specify a canned ACL with the
         * <code>x-amz-acl</code> request header. Amazon S3 supports a set of predefined
         * ACLs, known as <i>canned ACLs</i>. Each canned ACL has a predefined set of
         * grantees and permissions. Specify the canned ACL name as the value of
         * <code>x-amz-acl</code>. If you use this header, you cannot use other access
         * control-specific headers in your request. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html#CannedACL">Canned
         * ACL</a>.</p> </li> <li> <p>Specify access permissions explicitly with the
         * <code>x-amz-grant-read</code>, <code>x-amz-grant-read-acp</code>,
         * <code>x-amz-grant-write-acp</code>, and <code>x-amz-grant-full-control</code>
         * headers. When using these headers, you specify explicit access permissions and
         * grantees (Amazon Web Services accounts or Amazon S3 groups) who will receive the
         * permission. If you use these ACL-specific headers, you cannot use the
         * <code>x-amz-acl</code> header to set a canned ACL. These parameters map to the
         * set of permissions that Amazon S3 supports in an ACL. For more information, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
         * Control List (ACL) Overview</a>.</p> <p>You specify each grantee as a type=value
         * pair, where the type is one of the following:</p> <ul> <li> <p> <code>id</code>
         * – if the value specified is the canonical user ID of an Amazon Web Services
         * account</p> </li> <li> <p> <code>uri</code> – if you are granting permissions to
         * a predefined group</p> </li> <li> <p> <code>emailAddress</code> – if the value
         * specified is the email address of an Amazon Web Services account</p> 
         * <p>Using email addresses to specify a grantee is only supported in the following
         * Amazon Web Services Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li>
         * <li> <p>US West (N. California)</p> </li> <li> <p> US West (Oregon)</p> </li>
         * <li> <p> Asia Pacific (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p>
         * </li> <li> <p>Asia Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li>
         * <li> <p>South America (São Paulo)</p> </li> </ul> <p>For a list of all the
         * Amazon S3 supported Regions and endpoints, see <a
         * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
         * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
         * </li> </ul> <p>For example, the following <code>x-amz-grant-write</code> header
         * grants create, overwrite, and delete objects permission to LogDelivery group
         * predefined by Amazon S3 and two Amazon Web Services accounts identified by their
         * email addresses.</p> <p> <code>x-amz-grant-write:
         * uri="http://acs.amazonaws.com/groups/s3/LogDelivery", id="111122223333",
         * id="555566667777" </code> </p> </li> </ul> <p>You can use either a canned ACL or
         * specify access permissions explicitly. You cannot do both.</p> </dd> <dt>Grantee
         * Values</dt> <dd> <p>You can specify the person (grantee) to whom you're
         * assigning access rights (using request elements) in the following ways:</p> <ul>
         * <li> <p>By the person's ID:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="CanonicalUser"&gt;&lt;ID&gt;&lt;&gt;ID&lt;&gt;&lt;/ID&gt;&lt;DisplayName&gt;&lt;&gt;GranteesEmail&lt;&gt;&lt;/DisplayName&gt;
         * &lt;/Grantee&gt;</code> </p> <p>DisplayName is optional and ignored in the
         * request</p> </li> <li> <p>By URI:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="Group"&gt;&lt;URI&gt;&lt;&gt;http://acs.amazonaws.com/groups/global/AuthenticatedUsers&lt;&gt;&lt;/URI&gt;&lt;/Grantee&gt;</code>
         * </p> </li> <li> <p>By Email address:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="AmazonCustomerByEmail"&gt;&lt;EmailAddress&gt;&lt;&gt;Grantees@email.com&lt;&gt;&lt;/EmailAddress&gt;&amp;&lt;/Grantee&gt;</code>
         * </p> <p>The grantee is resolved to the CanonicalUser and, in a response to a GET
         * Object acl request, appears as the CanonicalUser. </p>  <p>Using email
         * addresses to specify a grantee is only supported in the following Amazon Web
         * Services Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li> <li> <p>US
         * West (N. California)</p> </li> <li> <p> US West (Oregon)</p> </li> <li> <p> Asia
         * Pacific (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p> </li> <li>
         * <p>Asia Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li> <li>
         * <p>South America (São Paulo)</p> </li> </ul> <p>For a list of all the Amazon S3
         * supported Regions and endpoints, see <a
         * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
         * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
         * </li> </ul> </dd> </dl> <p>The following operations are related to
         * <code>PutBucketAcl</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucket.html">DeleteBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectAcl.html">GetObjectAcl</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketAcl">AWS API
         * Reference</a></p>
         */
        virtual Model::PutBucketAclOutcome PutBucketAcl(const Model::PutBucketAclRequest& request) const;

        /**
         * A Callable wrapper for PutBucketAcl that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketAclRequestT = Model::PutBucketAclRequest>
        Model::PutBucketAclOutcomeCallable PutBucketAclCallable(const PutBucketAclRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketAcl, request);
        }

        /**
         * An Async wrapper for PutBucketAcl that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketAclRequestT = Model::PutBucketAclRequest>
        void PutBucketAclAsync(const PutBucketAclRequestT& request, const PutBucketAclResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketAcl, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets an analytics configuration for the bucket (specified by the analytics
         * configuration ID). You can have up to 1,000 analytics configurations per
         * bucket.</p> <p>You can choose to have storage class analysis export analysis
         * reports sent to a comma-separated values (CSV) flat file. See the
         * <code>DataExport</code> request element. Reports are updated daily and are based
         * on the object filters that you configure. When selecting data export, you
         * specify a destination bucket and an optional destination prefix where the file
         * is written. You can export the data to a destination bucket in a different
         * account. However, the destination bucket must be in the same Region as the
         * bucket that you are making the PUT analytics configuration to. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/analytics-storage-class.html">Amazon
         * S3 Analytics – Storage Class Analysis</a>. </p>  <p>You must create a
         * bucket policy on the destination bucket where the exported file is written to
         * grant permissions to Amazon S3 to write objects to the bucket. For an example
         * policy, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/example-bucket-policies.html#example-bucket-policies-use-case-9">Granting
         * Permissions for Amazon S3 Inventory and Storage Class Analysis</a>.</p>
         *  <p>To use this operation, you must have permissions to perform the
         * <code>s3:PutAnalyticsConfiguration</code> action. The bucket owner has this
         * permission by default. The bucket owner can grant this permission to others. For
         * more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>
         * <code>PutBucketAnalyticsConfiguration</code> has the following special
         * errors:</p> <ul> <li> <ul> <li> <p> <i>HTTP Error: HTTP 400 Bad Request</i> </p>
         * </li> <li> <p> <i>Code: InvalidArgument</i> </p> </li> <li> <p> <i>Cause:
         * Invalid argument.</i> </p> </li> </ul> </li> <li> <ul> <li> <p> <i>HTTP Error:
         * HTTP 400 Bad Request</i> </p> </li> <li> <p> <i>Code: TooManyConfigurations</i>
         * </p> </li> <li> <p> <i>Cause: You are attempting to create a new configuration
         * but have already reached the 1,000-configuration limit.</i> </p> </li> </ul>
         * </li> <li> <ul> <li> <p> <i>HTTP Error: HTTP 403 Forbidden</i> </p> </li> <li>
         * <p> <i>Code: AccessDenied</i> </p> </li> <li> <p> <i>Cause: You are not the
         * owner of the specified bucket, or you do not have the
         * s3:PutAnalyticsConfiguration bucket permission to set the configuration on the
         * bucket.</i> </p> </li> </ul> </li> </ul> <p>The following operations are related
         * to <code>PutBucketAnalyticsConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketAnalyticsConfiguration.html">GetBucketAnalyticsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketAnalyticsConfiguration.html">DeleteBucketAnalyticsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketAnalyticsConfigurations.html">ListBucketAnalyticsConfigurations</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketAnalyticsConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketAnalyticsConfigurationOutcome PutBucketAnalyticsConfiguration(const Model::PutBucketAnalyticsConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketAnalyticsConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketAnalyticsConfigurationRequestT = Model::PutBucketAnalyticsConfigurationRequest>
        Model::PutBucketAnalyticsConfigurationOutcomeCallable PutBucketAnalyticsConfigurationCallable(const PutBucketAnalyticsConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketAnalyticsConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketAnalyticsConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketAnalyticsConfigurationRequestT = Model::PutBucketAnalyticsConfigurationRequest>
        void PutBucketAnalyticsConfigurationAsync(const PutBucketAnalyticsConfigurationRequestT& request, const PutBucketAnalyticsConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketAnalyticsConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the <code>cors</code> configuration for your bucket. If the
         * configuration exists, Amazon S3 replaces it.</p> <p>To use this operation, you
         * must be allowed to perform the <code>s3:PutBucketCORS</code> action. By default,
         * the bucket owner has this permission and can grant it to others.</p> <p>You set
         * this configuration on a bucket so that the bucket can service cross-origin
         * requests. For example, you might want to enable a request whose origin is
         * <code>http://www.example.com</code> to access your Amazon S3 bucket at
         * <code>my.example.bucket.com</code> by using the browser's
         * <code>XMLHttpRequest</code> capability.</p> <p>To enable cross-origin resource
         * sharing (CORS) on a bucket, you add the <code>cors</code> subresource to the
         * bucket. The <code>cors</code> subresource is an XML document in which you
         * configure rules that identify origins and the HTTP methods that can be executed
         * on your bucket. The document is limited to 64 KB in size. </p> <p>When Amazon S3
         * receives a cross-origin request (or a pre-flight OPTIONS request) against a
         * bucket, it evaluates the <code>cors</code> configuration on the bucket and uses
         * the first <code>CORSRule</code> rule that matches the incoming browser request
         * to enable a cross-origin request. For a rule to match, the following conditions
         * must be met:</p> <ul> <li> <p>The request's <code>Origin</code> header must
         * match <code>AllowedOrigin</code> elements.</p> </li> <li> <p>The request method
         * (for example, GET, PUT, HEAD, and so on) or the
         * <code>Access-Control-Request-Method</code> header in case of a pre-flight
         * <code>OPTIONS</code> request must be one of the <code>AllowedMethod</code>
         * elements. </p> </li> <li> <p>Every header specified in the
         * <code>Access-Control-Request-Headers</code> request header of a pre-flight
         * request must match an <code>AllowedHeader</code> element. </p> </li> </ul> <p>
         * For more information about CORS, go to <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cors.html">Enabling
         * Cross-Origin Resource Sharing</a> in the <i>Amazon S3 User Guide</i>.</p> <p>The
         * following operations are related to <code>PutBucketCors</code>:</p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketCors.html">GetBucketCors</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketCors.html">DeleteBucketCors</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTOPTIONSobject.html">RESTOPTIONSobject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketCors">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketCorsOutcome PutBucketCors(const Model::PutBucketCorsRequest& request) const;

        /**
         * A Callable wrapper for PutBucketCors that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketCorsRequestT = Model::PutBucketCorsRequest>
        Model::PutBucketCorsOutcomeCallable PutBucketCorsCallable(const PutBucketCorsRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketCors, request);
        }

        /**
         * An Async wrapper for PutBucketCors that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketCorsRequestT = Model::PutBucketCorsRequest>
        void PutBucketCorsAsync(const PutBucketCorsRequestT& request, const PutBucketCorsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketCors, request, handler, context);
        }

        /**
         * <p>This operation configures default encryption and Amazon S3 Bucket Keys for an
         * existing bucket.</p>  <p> <b>Directory buckets </b> - For directory
         * buckets, you must make requests for this API operation to the Regional endpoint.
         * These endpoints support path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p>By default, all buckets have a default encryption configuration that
         * uses server-side encryption with Amazon S3 managed keys (SSE-S3).</p> 
         * <ul> <li> <p> <b>General purpose buckets</b> </p> <ul> <li> <p>You can
         * optionally configure default encryption for a bucket by using server-side
         * encryption with Key Management Service (KMS) keys (SSE-KMS) or dual-layer
         * server-side encryption with Amazon Web Services KMS keys (DSSE-KMS). If you
         * specify default encryption by using SSE-KMS, you can also configure <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-key.html">Amazon S3
         * Bucket Keys</a>. For information about the bucket default encryption feature,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-encryption.html">Amazon
         * S3 Bucket Default Encryption</a> in the <i>Amazon S3 User Guide</i>. </p> </li>
         * <li> <p>If you use PutBucketEncryption to set your <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-encryption.html">default
         * bucket encryption</a> to SSE-KMS, you should verify that your KMS key ID is
         * correct. Amazon S3 doesn't validate the KMS key ID provided in
         * PutBucketEncryption requests.</p> </li> </ul> </li> <li> <p> <b>Directory
         * buckets </b> - You can optionally configure default encryption for a bucket by
         * using server-side encryption with Key Management Service (KMS) keys
         * (SSE-KMS).</p> <ul> <li> <p>We recommend that the bucket's default encryption
         * uses the desired encryption configuration and you don't override the bucket
         * default encryption in your <code>CreateSession</code> requests or
         * <code>PUT</code> object requests. Then, new objects are automatically encrypted
         * with the desired encryption settings. For more information about the encryption
         * overriding behaviors in directory buckets, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-specifying-kms-encryption.html">Specifying
         * server-side encryption with KMS for new object uploads</a>.</p> </li> <li>
         * <p>Your SSE-KMS configuration can only support 1 <a
         * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">customer
         * managed key</a> per directory bucket for the lifetime of the bucket. The <a
         * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-managed-cmk">Amazon
         * Web Services managed key</a> (<code>aws/s3</code>) isn't supported. </p> </li>
         * <li> <p>S3 Bucket Keys are always enabled for <code>GET</code> and
         * <code>PUT</code> operations in a directory bucket and can’t be disabled. S3
         * Bucket Keys aren't supported, when you copy SSE-KMS encrypted objects from
         * general purpose buckets to directory buckets, from directory buckets to general
         * purpose buckets, or between directory buckets, through <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>,
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>,
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-buckets-objects-Batch-Ops">the
         * Copy operation in Batch Operations</a>, or <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/create-import-job">the
         * import jobs</a>. In this case, Amazon S3 makes a call to KMS every time a copy
         * request is made for a KMS-encrypted object.</p> </li> <li> <p>When you specify
         * an <a
         * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">KMS
         * customer managed key</a> for encryption in your directory bucket, only use the
         * key ID or key ARN. The key alias format of the KMS key isn't supported.</p>
         * </li> <li> <p>For directory buckets, if you use PutBucketEncryption to set your
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/bucket-encryption.html">default
         * bucket encryption</a> to SSE-KMS, Amazon S3 validates the KMS key ID provided in
         * PutBucketEncryption requests.</p> </li> </ul> </li> </ul>  
         * <p>If you're specifying a customer managed KMS key, we recommend using a fully
         * qualified KMS key ARN. If you use a KMS key alias instead, then KMS resolves the
         * key within the requester’s account. This behavior can result in data that's
         * encrypted with a KMS key that belongs to the requester, and not the bucket
         * owner.</p> <p>Also, this action requires Amazon Web Services Signature Version
         * 4. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html">
         * Authenticating Requests (Amazon Web Services Signature Version 4)</a>. </p>
         *  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose
         * bucket permissions</b> - The <code>s3:PutEncryptionConfiguration</code>
         * permission is required in a policy. The bucket owner has this permission by
         * default. The bucket owner can grant this permission to others. For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </li> <li> <p> <b>Directory bucket permissions</b> - To grant
         * access to this API operation, you must have the
         * <code>s3express:PutEncryptionConfiguration</code> permission in an IAM
         * identity-based policy instead of a bucket policy. Cross-account access to this
         * API operation isn't supported. This operation can only be performed by the
         * Amazon Web Services account that owns the resource. For more information about
         * directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> <p>To set a directory bucket default
         * encryption with SSE-KMS, you must also have the <code>kms:GenerateDataKey</code>
         * and the <code>kms:Decrypt</code> permissions in IAM identity-based policies and
         * KMS key policies for the target KMS key.</p> </li> </ul> </dd> <dt>HTTP Host
         * header syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host header
         * syntax is <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>PutBucketEncryption</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketEncryption.html">GetBucketEncryption</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketEncryption.html">DeleteBucketEncryption</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketEncryption">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketEncryptionOutcome PutBucketEncryption(const Model::PutBucketEncryptionRequest& request) const;

        /**
         * A Callable wrapper for PutBucketEncryption that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketEncryptionRequestT = Model::PutBucketEncryptionRequest>
        Model::PutBucketEncryptionOutcomeCallable PutBucketEncryptionCallable(const PutBucketEncryptionRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketEncryption, request);
        }

        /**
         * An Async wrapper for PutBucketEncryption that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketEncryptionRequestT = Model::PutBucketEncryptionRequest>
        void PutBucketEncryptionAsync(const PutBucketEncryptionRequestT& request, const PutBucketEncryptionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketEncryption, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Puts a S3 Intelligent-Tiering configuration to the specified bucket. You can
         * have up to 1,000 S3 Intelligent-Tiering configurations per bucket.</p> <p>The S3
         * Intelligent-Tiering storage class is designed to optimize storage costs by
         * automatically moving data to the most cost-effective storage access tier,
         * without performance impact or operational overhead. S3 Intelligent-Tiering
         * delivers automatic cost savings in three low latency and high throughput access
         * tiers. To get the lowest storage cost on data that can be accessed in minutes to
         * hours, you can choose to activate additional archiving capabilities.</p> <p>The
         * S3 Intelligent-Tiering storage class is the ideal storage class for data with
         * unknown, changing, or unpredictable access patterns, independent of object size
         * or retention period. If the size of an object is less than 128 KB, it is not
         * monitored and not eligible for auto-tiering. Smaller objects can be stored, but
         * they are always charged at the Frequent Access tier rates in the S3
         * Intelligent-Tiering storage class.</p> <p>For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html#sc-dynamic-data-access">Storage
         * class for automatically optimizing frequently and infrequently accessed
         * objects</a>.</p> <p>Operations related to
         * <code>PutBucketIntelligentTieringConfiguration</code> include: </p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketIntelligentTieringConfiguration.html">DeleteBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketIntelligentTieringConfiguration.html">GetBucketIntelligentTieringConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketIntelligentTieringConfigurations.html">ListBucketIntelligentTieringConfigurations</a>
         * </p> </li> </ul>  <p>You only need S3 Intelligent-Tiering enabled on a
         * bucket if you want to automatically move objects stored in the S3
         * Intelligent-Tiering storage class to the Archive Access or Deep Archive Access
         * tier.</p>  <p> <code>PutBucketIntelligentTieringConfiguration</code> has
         * the following special errors:</p> <dl> <dt>HTTP 400 Bad Request Error</dt> <dd>
         * <p> <i>Code:</i> InvalidArgument</p> <p> <i>Cause:</i> Invalid Argument</p>
         * </dd> <dt>HTTP 400 Bad Request Error</dt> <dd> <p> <i>Code:</i>
         * TooManyConfigurations</p> <p> <i>Cause:</i> You are attempting to create a new
         * configuration but have already reached the 1,000-configuration limit. </p> </dd>
         * <dt>HTTP 403 Forbidden Error</dt> <dd> <p> <i>Cause:</i> You are not the owner
         * of the specified bucket, or you do not have the
         * <code>s3:PutIntelligentTieringConfiguration</code> bucket permission to set the
         * configuration on the bucket. </p> </dd> </dl><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketIntelligentTieringConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketIntelligentTieringConfigurationOutcome PutBucketIntelligentTieringConfiguration(const Model::PutBucketIntelligentTieringConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketIntelligentTieringConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketIntelligentTieringConfigurationRequestT = Model::PutBucketIntelligentTieringConfigurationRequest>
        Model::PutBucketIntelligentTieringConfigurationOutcomeCallable PutBucketIntelligentTieringConfigurationCallable(const PutBucketIntelligentTieringConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketIntelligentTieringConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketIntelligentTieringConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketIntelligentTieringConfigurationRequestT = Model::PutBucketIntelligentTieringConfigurationRequest>
        void PutBucketIntelligentTieringConfigurationAsync(const PutBucketIntelligentTieringConfigurationRequestT& request, const PutBucketIntelligentTieringConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketIntelligentTieringConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>This implementation of the <code>PUT</code> action adds an inventory
         * configuration (identified by the inventory ID) to the bucket. You can have up to
         * 1,000 inventory configurations per bucket. </p> <p>Amazon S3 inventory generates
         * inventories of the objects in the bucket on a daily or weekly basis, and the
         * results are published to a flat file. The bucket that is inventoried is called
         * the <i>source</i> bucket, and the bucket where the inventory flat file is stored
         * is called the <i>destination</i> bucket. The <i>destination</i> bucket must be
         * in the same Amazon Web Services Region as the <i>source</i> bucket. </p> <p>When
         * you configure an inventory for a <i>source</i> bucket, you specify the
         * <i>destination</i> bucket where you want the inventory to be stored, and whether
         * to generate the inventory daily or weekly. You can also configure what object
         * metadata to include and whether to inventory all object versions or only current
         * versions. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-inventory.html">Amazon
         * S3 Inventory</a> in the Amazon S3 User Guide.</p>  <p>You must create
         * a bucket policy on the <i>destination</i> bucket to grant permissions to Amazon
         * S3 to write objects to the bucket in the defined location. For an example
         * policy, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/example-bucket-policies.html#example-bucket-policies-use-case-9">
         * Granting Permissions for Amazon S3 Inventory and Storage Class Analysis</a>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <p>To use this operation, you must
         * have permission to perform the <code>s3:PutInventoryConfiguration</code> action.
         * The bucket owner has this permission by default and can grant this permission to
         * others. </p> <p>The <code>s3:PutInventoryConfiguration</code> permission allows
         * a user to create an <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/storage-inventory.html">S3
         * Inventory</a> report that includes all object metadata fields available and to
         * specify the destination bucket to store the inventory. A user with read access
         * to objects in the destination bucket can also access all object metadata fields
         * that are available in the inventory report. </p> <p>To restrict access to an
         * inventory report, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/example-bucket-policies.html#example-bucket-policies-use-case-10">Restricting
         * access to an Amazon S3 Inventory report</a> in the <i>Amazon S3 User Guide</i>.
         * For more information about the metadata fields available in S3 Inventory, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/storage-inventory.html#storage-inventory-contents">Amazon
         * S3 Inventory lists</a> in the <i>Amazon S3 User Guide</i>. For more information
         * about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * related to bucket subresource operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Identity
         * and access management in Amazon S3</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </dd> </dl> <p> <code>PutBucketInventoryConfiguration</code> has the following
         * special errors:</p> <dl> <dt>HTTP 400 Bad Request Error</dt> <dd> <p>
         * <i>Code:</i> InvalidArgument</p> <p> <i>Cause:</i> Invalid Argument</p> </dd>
         * <dt>HTTP 400 Bad Request Error</dt> <dd> <p> <i>Code:</i>
         * TooManyConfigurations</p> <p> <i>Cause:</i> You are attempting to create a new
         * configuration but have already reached the 1,000-configuration limit. </p> </dd>
         * <dt>HTTP 403 Forbidden Error</dt> <dd> <p> <i>Cause:</i> You are not the owner
         * of the specified bucket, or you do not have the
         * <code>s3:PutInventoryConfiguration</code> bucket permission to set the
         * configuration on the bucket. </p> </dd> </dl> <p>The following operations are
         * related to <code>PutBucketInventoryConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketInventoryConfiguration.html">GetBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketInventoryConfiguration.html">DeleteBucketInventoryConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketInventoryConfigurations.html">ListBucketInventoryConfigurations</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketInventoryConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketInventoryConfigurationOutcome PutBucketInventoryConfiguration(const Model::PutBucketInventoryConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketInventoryConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketInventoryConfigurationRequestT = Model::PutBucketInventoryConfigurationRequest>
        Model::PutBucketInventoryConfigurationOutcomeCallable PutBucketInventoryConfigurationCallable(const PutBucketInventoryConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketInventoryConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketInventoryConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketInventoryConfigurationRequestT = Model::PutBucketInventoryConfigurationRequest>
        void PutBucketInventoryConfigurationAsync(const PutBucketInventoryConfigurationRequestT& request, const PutBucketInventoryConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketInventoryConfiguration, request, handler, context);
        }

        /**
         * <p>Creates a new lifecycle configuration for the bucket or replaces an existing
         * lifecycle configuration. Keep in mind that this will overwrite an existing
         * lifecycle configuration, so if you want to retain any configuration details,
         * they must be included in the new lifecycle configuration. For information about
         * lifecycle configuration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lifecycle-mgmt.html">Managing
         * your storage lifecycle</a>.</p>  <p>Bucket lifecycle configuration now
         * supports specifying a lifecycle rule using an object key name prefix, one or
         * more object tags, object size, or any combination of these. Accordingly, this
         * section describes the latest API. The previous version of the API supported
         * filtering based only on an object key name prefix, which is supported for
         * backward compatibility. For the related API description, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycle.html">PutBucketLifecycle</a>.</p>
         *  <dl> <dt>Rules</dt> <dt>Permissions</dt> <dt>HTTP Host header
         * syntax</dt> <dd> <p>You specify the lifecycle configuration in your request
         * body. The lifecycle configuration is specified as XML consisting of one or more
         * rules. An Amazon S3 Lifecycle configuration can have up to 1,000 rules. This
         * limit is not adjustable.</p> <p>Bucket lifecycle configuration supports
         * specifying a lifecycle rule using an object key name prefix, one or more object
         * tags, object size, or any combination of these. Accordingly, this section
         * describes the latest API. The previous version of the API supported filtering
         * based only on an object key name prefix, which is supported for backward
         * compatibility for general purpose buckets. For the related API description, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycle.html">PutBucketLifecycle</a>.
         * </p>  <p>Lifecyle configurations for directory buckets only support
         * expiring objects and cancelling multipart uploads. Expiring of versioned
         * objects,transitions and tag filters are not supported.</p>  <p>A
         * lifecycle rule consists of the following:</p> <ul> <li> <p>A filter identifying
         * a subset of objects to which the rule applies. The filter can be based on a key
         * name prefix, object tags, object size, or any combination of these.</p> </li>
         * <li> <p>A status indicating whether the rule is in effect.</p> </li> <li> <p>One
         * or more lifecycle transition and expiration actions that you want Amazon S3 to
         * perform on the objects identified by the filter. If the state of your bucket is
         * versioning-enabled or versioning-suspended, you can have many versions of the
         * same object (one current version and zero or more noncurrent versions). Amazon
         * S3 provides predefined actions that you can specify for current and noncurrent
         * object versions.</p> </li> </ul> <p>For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lifecycle-mgmt.html">Object
         * Lifecycle Management</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/intro-lifecycle-rules.html">Lifecycle
         * Configuration Elements</a>.</p> </dd> <dd> <ul> <li> <p> <b>General purpose
         * bucket permissions</b> - By default, all Amazon S3 resources are private,
         * including buckets, objects, and related subresources (for example, lifecycle
         * configuration and website configuration). Only the resource owner (that is, the
         * Amazon Web Services account that created it) can access the resource. The
         * resource owner can optionally grant access permissions to others by writing an
         * access policy. For this operation, a user must have the
         * <code>s3:PutLifecycleConfiguration</code> permission.</p> <p>You can also
         * explicitly deny permissions. An explicit deny also supersedes any other
         * permissions. If you want to block users or accounts from removing or deleting
         * objects from your bucket, you must deny them permissions for the following
         * actions:</p> <ul> <li> <p> <code>s3:DeleteObject</code> </p> </li> <li> <p>
         * <code>s3:DeleteObjectVersion</code> </p> </li> <li> <p>
         * <code>s3:PutLifecycleConfiguration</code> </p> <p>For more information about
         * permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> </li> </ul> </li> </ul>
         * <ul> <li> <p> <b>Directory bucket permissions</b> - You must have the
         * <code>s3express:PutLifecycleConfiguration</code> permission in an IAM
         * identity-based policy to use this operation. Cross-account access to this API
         * operation isn't supported. The resource owner can optionally grant access
         * permissions to others by creating a role or user for them as long as they are
         * within the same account as the owner and resource.</p> <p>For more information
         * about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Authorizing
         * Regional endpoint APIs with IAM</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p> <b>Directory buckets </b> - For directory buckets, you must make
         * requests for this API operation to the Regional endpoint. These endpoints
         * support path-style requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  </li> </ul> </dd> <dd> <p> <b>Directory buckets </b> - The HTTP Host
         * header syntax is <code>s3express-control.<i>region</i>.amazonaws.com</code>.</p>
         * <p>The following operations are related to
         * <code>PutBucketLifecycleConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLifecycleConfiguration.html">GetBucketLifecycleConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketLifecycle.html">DeleteBucketLifecycle</a>
         * </p> </li> </ul> </dd> </dl><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketLifecycleConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketLifecycleConfigurationOutcome PutBucketLifecycleConfiguration(const Model::PutBucketLifecycleConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketLifecycleConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketLifecycleConfigurationRequestT = Model::PutBucketLifecycleConfigurationRequest>
        Model::PutBucketLifecycleConfigurationOutcomeCallable PutBucketLifecycleConfigurationCallable(const PutBucketLifecycleConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketLifecycleConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketLifecycleConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketLifecycleConfigurationRequestT = Model::PutBucketLifecycleConfigurationRequest>
        void PutBucketLifecycleConfigurationAsync(const PutBucketLifecycleConfigurationRequestT& request, const PutBucketLifecycleConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketLifecycleConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Set the logging parameters for a bucket and to specify permissions for who
         * can view and modify the logging parameters. All logs are saved to buckets in the
         * same Amazon Web Services Region as the source bucket. To set the logging status
         * of a bucket, you must be the bucket owner.</p> <p>The bucket owner is
         * automatically granted FULL_CONTROL to all logs. You use the <code>Grantee</code>
         * request element to grant access to other people. The <code>Permissions</code>
         * request element specifies the kind of access the grantee has to the logs.</p>
         *  <p>If the target bucket for log delivery uses the bucket owner
         * enforced setting for S3 Object Ownership, you can't use the <code>Grantee</code>
         * request element to grant access to others. Permissions can only be granted using
         * policies. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/enable-server-access-logging.html#grant-log-delivery-permissions-general">Permissions
         * for server access log delivery</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Grantee Values</dt> <dd> <p>You can specify the person
         * (grantee) to whom you're assigning access rights (by using request elements) in
         * the following ways:</p> <ul> <li> <p>By the person's ID:</p> <p>
         * <code>&lt;Grantee xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="CanonicalUser"&gt;&lt;ID&gt;&lt;&gt;ID&lt;&gt;&lt;/ID&gt;&lt;DisplayName&gt;&lt;&gt;GranteesEmail&lt;&gt;&lt;/DisplayName&gt;
         * &lt;/Grantee&gt;</code> </p> <p> <code>DisplayName</code> is optional and
         * ignored in the request.</p> </li> <li> <p>By Email address:</p> <p> <code>
         * &lt;Grantee xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="AmazonCustomerByEmail"&gt;&lt;EmailAddress&gt;&lt;&gt;Grantees@email.com&lt;&gt;&lt;/EmailAddress&gt;&lt;/Grantee&gt;</code>
         * </p> <p>The grantee is resolved to the <code>CanonicalUser</code> and, in a
         * response to a <code>GETObjectAcl</code> request, appears as the
         * CanonicalUser.</p> </li> <li> <p>By URI:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="Group"&gt;&lt;URI&gt;&lt;&gt;http://acs.amazonaws.com/groups/global/AuthenticatedUsers&lt;&gt;&lt;/URI&gt;&lt;/Grantee&gt;</code>
         * </p> </li> </ul> </dd> </dl> <p>To enable logging, you use
         * <code>LoggingEnabled</code> and its children request elements. To disable
         * logging, you use an empty <code>BucketLoggingStatus</code> request element:</p>
         * <p> <code>&lt;BucketLoggingStatus xmlns="http://doc.s3.amazonaws.com/2006-03-01"
         * /&gt;</code> </p> <p>For more information about server access logging, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/ServerLogs.html">Server
         * Access Logging</a> in the <i>Amazon S3 User Guide</i>. </p> <p>For more
         * information about creating a bucket, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>.
         * For more information about returning the logging status of a bucket, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLogging.html">GetBucketLogging</a>.</p>
         * <p>The following operations are related to <code>PutBucketLogging</code>:</p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucket.html">DeleteBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLogging.html">GetBucketLogging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketLogging">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketLoggingOutcome PutBucketLogging(const Model::PutBucketLoggingRequest& request) const;

        /**
         * A Callable wrapper for PutBucketLogging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketLoggingRequestT = Model::PutBucketLoggingRequest>
        Model::PutBucketLoggingOutcomeCallable PutBucketLoggingCallable(const PutBucketLoggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketLogging, request);
        }

        /**
         * An Async wrapper for PutBucketLogging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketLoggingRequestT = Model::PutBucketLoggingRequest>
        void PutBucketLoggingAsync(const PutBucketLoggingRequestT& request, const PutBucketLoggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketLogging, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets a metrics configuration (specified by the metrics configuration ID) for
         * the bucket. You can have up to 1,000 metrics configurations per bucket. If
         * you're updating an existing metrics configuration, note that this is a full
         * replacement of the existing metrics configuration. If you don't include the
         * elements you want to keep, they are erased.</p> <p>To use this operation, you
         * must have permissions to perform the <code>s3:PutMetricsConfiguration</code>
         * action. The bucket owner has this permission by default. The bucket owner can
         * grant this permission to others. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>For information about
         * CloudWatch request metrics for Amazon S3, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/cloudwatch-monitoring.html">Monitoring
         * Metrics with Amazon CloudWatch</a>.</p> <p>The following operations are related
         * to <code>PutBucketMetricsConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketMetricsConfiguration.html">DeleteBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketMetricsConfiguration.html">GetBucketMetricsConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListBucketMetricsConfigurations.html">ListBucketMetricsConfigurations</a>
         * </p> </li> </ul> <p> <code>PutBucketMetricsConfiguration</code> has the
         * following special error:</p> <ul> <li> <p>Error code:
         * <code>TooManyConfigurations</code> </p> <ul> <li> <p>Description: You are
         * attempting to create a new configuration but have already reached the
         * 1,000-configuration limit.</p> </li> <li> <p>HTTP Status Code: HTTP 400 Bad
         * Request</p> </li> </ul> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketMetricsConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketMetricsConfigurationOutcome PutBucketMetricsConfiguration(const Model::PutBucketMetricsConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketMetricsConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketMetricsConfigurationRequestT = Model::PutBucketMetricsConfigurationRequest>
        Model::PutBucketMetricsConfigurationOutcomeCallable PutBucketMetricsConfigurationCallable(const PutBucketMetricsConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketMetricsConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketMetricsConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketMetricsConfigurationRequestT = Model::PutBucketMetricsConfigurationRequest>
        void PutBucketMetricsConfigurationAsync(const PutBucketMetricsConfigurationRequestT& request, const PutBucketMetricsConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketMetricsConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Enables notifications of specified events for a bucket. For more information
         * about event notifications, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Configuring
         * Event Notifications</a>.</p> <p>Using this API, you can replace an existing
         * notification configuration. The configuration is an XML file that defines the
         * event types that you want Amazon S3 to publish and the destination where you
         * want Amazon S3 to publish an event notification when it detects an event of the
         * specified type.</p> <p>By default, your bucket has no event notifications
         * configured. That is, the notification configuration will be an empty
         * <code>NotificationConfiguration</code>.</p> <p>
         * <code>&lt;NotificationConfiguration&gt;</code> </p> <p>
         * <code>&lt;/NotificationConfiguration&gt;</code> </p> <p>This action replaces the
         * existing notification configuration with the configuration you include in the
         * request body.</p> <p>After Amazon S3 receives this request, it first verifies
         * that any Amazon Simple Notification Service (Amazon SNS) or Amazon Simple Queue
         * Service (Amazon SQS) destination exists, and that the bucket owner has
         * permission to publish to it by sending a test notification. In the case of
         * Lambda destinations, Amazon S3 verifies that the Lambda function permissions
         * grant Amazon S3 permission to invoke the function from the Amazon S3 bucket. For
         * more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Configuring
         * Notifications for Amazon S3 Events</a>.</p> <p>You can disable notifications by
         * adding the empty NotificationConfiguration element.</p> <p>For more information
         * about the number of event notification configurations that you can create per
         * bucket, see <a
         * href="https://docs.aws.amazon.com/general/latest/gr/s3.html#limits_s3">Amazon S3
         * service quotas</a> in <i>Amazon Web Services General Reference</i>.</p> <p>By
         * default, only the bucket owner can configure notifications on a bucket. However,
         * bucket owners can use a bucket policy to grant permission to other users to set
         * this configuration with the required <code>s3:PutBucketNotification</code>
         * permission.</p>  <p>The PUT notification is an atomic operation. For
         * example, suppose your notification configuration includes SNS topic, SQS queue,
         * and Lambda function configurations. When you send a PUT request with this
         * configuration, Amazon S3 sends test messages to your SNS topic. If the message
         * fails, the entire PUT action will fail, and Amazon S3 will not add the
         * configuration to your bucket.</p>  <p>If the configuration in the request
         * body includes only one <code>TopicConfiguration</code> specifying only the
         * <code>s3:ReducedRedundancyLostObject</code> event type, the response will also
         * include the <code>x-amz-sns-test-message-id</code> header containing the message
         * ID of the test notification sent to the topic.</p> <p>The following action is
         * related to <code>PutBucketNotificationConfiguration</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketNotificationConfiguration.html">GetBucketNotificationConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketNotificationConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketNotificationConfigurationOutcome PutBucketNotificationConfiguration(const Model::PutBucketNotificationConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketNotificationConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketNotificationConfigurationRequestT = Model::PutBucketNotificationConfigurationRequest>
        Model::PutBucketNotificationConfigurationOutcomeCallable PutBucketNotificationConfigurationCallable(const PutBucketNotificationConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketNotificationConfiguration, request);
        }

        /**
         * An Async wrapper for PutBucketNotificationConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketNotificationConfigurationRequestT = Model::PutBucketNotificationConfigurationRequest>
        void PutBucketNotificationConfigurationAsync(const PutBucketNotificationConfigurationRequestT& request, const PutBucketNotificationConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketNotificationConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Creates or modifies <code>OwnershipControls</code> for an Amazon S3 bucket.
         * To use this operation, you must have the
         * <code>s3:PutBucketOwnershipControls</code> permission. For more information
         * about Amazon S3 permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/user-guide/using-with-s3-actions.html">Specifying
         * permissions in a policy</a>. </p> <p>For information about Amazon S3 Object
         * Ownership, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/user-guide/about-object-ownership.html">Using
         * object ownership</a>. </p> <p>The following operations are related to
         * <code>PutBucketOwnershipControls</code>:</p> <ul> <li> <p>
         * <a>GetBucketOwnershipControls</a> </p> </li> <li> <p>
         * <a>DeleteBucketOwnershipControls</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketOwnershipControls">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketOwnershipControlsOutcome PutBucketOwnershipControls(const Model::PutBucketOwnershipControlsRequest& request) const;

        /**
         * A Callable wrapper for PutBucketOwnershipControls that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketOwnershipControlsRequestT = Model::PutBucketOwnershipControlsRequest>
        Model::PutBucketOwnershipControlsOutcomeCallable PutBucketOwnershipControlsCallable(const PutBucketOwnershipControlsRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketOwnershipControls, request);
        }

        /**
         * An Async wrapper for PutBucketOwnershipControls that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketOwnershipControlsRequestT = Model::PutBucketOwnershipControlsRequest>
        void PutBucketOwnershipControlsAsync(const PutBucketOwnershipControlsRequestT& request, const PutBucketOwnershipControlsResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketOwnershipControls, request, handler, context);
        }

        /**
         * <p>Applies an Amazon S3 bucket policy to an Amazon S3 bucket.</p>  <p>
         * <b>Directory buckets </b> - For directory buckets, you must make requests for
         * this API operation to the Regional endpoint. These endpoints support path-style
         * requests in the format
         * <code>https://s3express-control.<i>region-code</i>.amazonaws.com/<i>bucket-name</i>
         * </code>. Virtual-hosted-style requests aren't supported. For more information
         * about endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <p>If you are using an identity other
         * than the root user of the Amazon Web Services account that owns the bucket, the
         * calling identity must both have the <code>PutBucketPolicy</code> permissions on
         * the specified bucket and belong to the bucket owner's account in order to use
         * this operation.</p> <p>If you don't have <code>PutBucketPolicy</code>
         * permissions, Amazon S3 returns a <code>403 Access Denied</code> error. If you
         * have the correct permissions, but you're not using an identity that belongs to
         * the bucket owner's account, Amazon S3 returns a <code>405 Method Not
         * Allowed</code> error.</p>  <p>To ensure that bucket owners don't
         * inadvertently lock themselves out of their own buckets, the root principal in a
         * bucket owner's Amazon Web Services account can perform the
         * <code>GetBucketPolicy</code>, <code>PutBucketPolicy</code>, and
         * <code>DeleteBucketPolicy</code> API actions, even if their bucket policy
         * explicitly denies the root principal's access. Bucket owner root principals can
         * only be blocked from performing these API actions by VPC endpoint policies and
         * Amazon Web Services Organizations policies.</p>  <ul> <li> <p>
         * <b>General purpose bucket permissions</b> - The <code>s3:PutBucketPolicy</code>
         * permission is required in a policy. For more information about general purpose
         * buckets bucket policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-iam-policies.html">Using
         * Bucket Policies and User Policies</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> <li> <p> <b>Directory bucket permissions</b> - To grant access to this API
         * operation, you must have the <code>s3express:PutBucketPolicy</code> permission
         * in an IAM identity-based policy instead of a bucket policy. Cross-account access
         * to this API operation isn't supported. This operation can only be performed by
         * the Amazon Web Services account that owns the resource. For more information
         * about directory bucket policies and permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam.html">Amazon
         * Web Services Identity and Access Management (IAM) for S3 Express One Zone</a> in
         * the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd> <dt>Example bucket
         * policies</dt> <dd> <p> <b>General purpose buckets example bucket policies</b> -
         * See <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/example-bucket-policies.html">Bucket
         * policy examples</a> in the <i>Amazon S3 User Guide</i>.</p> <p> <b>Directory
         * bucket example bucket policies</b> - See <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-example-bucket-policies.html">Example
         * bucket policies for S3 Express One Zone</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory
         * buckets </b> - The HTTP Host header syntax is
         * <code>s3express-control.<i>region-code</i>.amazonaws.com</code>.</p> </dd> </dl>
         * <p>The following operations are related to <code>PutBucketPolicy</code>:</p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucket.html">DeleteBucket</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketPolicy">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketPolicyOutcome PutBucketPolicy(const Model::PutBucketPolicyRequest& request) const;

        /**
         * A Callable wrapper for PutBucketPolicy that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketPolicyRequestT = Model::PutBucketPolicyRequest>
        Model::PutBucketPolicyOutcomeCallable PutBucketPolicyCallable(const PutBucketPolicyRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketPolicy, request);
        }

        /**
         * An Async wrapper for PutBucketPolicy that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketPolicyRequestT = Model::PutBucketPolicyRequest>
        void PutBucketPolicyAsync(const PutBucketPolicyRequestT& request, const PutBucketPolicyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketPolicy, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p>  <p>
         * Creates a replication configuration or replaces an existing one. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/replication.html">Replication</a>
         * in the <i>Amazon S3 User Guide</i>. </p> <p>Specify the replication
         * configuration in the request body. In the replication configuration, you provide
         * the name of the destination bucket or buckets where you want Amazon S3 to
         * replicate objects, the IAM role that Amazon S3 can assume to replicate objects
         * on your behalf, and other relevant information. You can invoke this request for
         * a specific Amazon Web Services Region by using the <a
         * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_condition-keys.html#condition-keys-requestedregion">
         * <code>aws:RequestedRegion</code> </a> condition key.</p> <p>A replication
         * configuration must include at least one rule, and can contain a maximum of
         * 1,000. Each rule identifies a subset of objects to replicate by filtering the
         * objects in the source bucket. To choose additional subsets of objects to
         * replicate, add a rule for each subset.</p> <p>To specify a subset of the objects
         * in the source bucket to apply a replication rule to, add the Filter element as a
         * child of the Rule element. You can filter objects based on an object key prefix,
         * one or more object tags, or both. When you add the Filter element in the
         * configuration, you must also add the following elements:
         * <code>DeleteMarkerReplication</code>, <code>Status</code>, and
         * <code>Priority</code>.</p>  <p>If you are using an earlier version of the
         * replication configuration, Amazon S3 handles replication of delete markers
         * differently. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/replication-add-config.html#replication-backward-compat-considerations">Backward
         * Compatibility</a>.</p>  <p>For information about enabling versioning on a
         * bucket, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/Versioning.html">Using
         * Versioning</a>.</p> <dl> <dt>Handling Replication of Encrypted Objects</dt> <dd>
         * <p>By default, Amazon S3 doesn't replicate objects that are stored at rest using
         * server-side encryption with KMS keys. To replicate Amazon Web Services
         * KMS-encrypted objects, add the following: <code>SourceSelectionCriteria</code>,
         * <code>SseKmsEncryptedObjects</code>, <code>Status</code>,
         * <code>EncryptionConfiguration</code>, and <code>ReplicaKmsKeyID</code>. For
         * information about replication configuration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/replication-config-for-kms-objects.html">Replicating
         * Objects Created with SSE Using KMS keys</a>.</p> <p>For information on
         * <code>PutBucketReplication</code> errors, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#ReplicationErrorCodeList">List
         * of replication-related error codes</a> </p> </dd> <dt>Permissions</dt> <dd>
         * <p>To create a <code>PutBucketReplication</code> request, you must have
         * <code>s3:PutReplicationConfiguration</code> permissions for the bucket. </p>
         * <p>By default, a resource owner, in this case the Amazon Web Services account
         * that created the bucket, can perform this operation. The resource owner can also
         * grant others permissions to perform the operation. For more information about
         * permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p>  <p>To perform
         * this operation, the user or role performing the action must have the <a
         * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_roles_use_passrole.html">iam:PassRole</a>
         * permission.</p>  </dd> </dl> <p>The following operations are related to
         * <code>PutBucketReplication</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketReplication.html">GetBucketReplication</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketReplication.html">DeleteBucketReplication</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketReplication">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketReplicationOutcome PutBucketReplication(const Model::PutBucketReplicationRequest& request) const;

        /**
         * A Callable wrapper for PutBucketReplication that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketReplicationRequestT = Model::PutBucketReplicationRequest>
        Model::PutBucketReplicationOutcomeCallable PutBucketReplicationCallable(const PutBucketReplicationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketReplication, request);
        }

        /**
         * An Async wrapper for PutBucketReplication that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketReplicationRequestT = Model::PutBucketReplicationRequest>
        void PutBucketReplicationAsync(const PutBucketReplicationRequestT& request, const PutBucketReplicationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketReplication, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the request payment configuration for a bucket. By default, the bucket
         * owner pays for downloads from the bucket. This configuration parameter enables
         * the bucket owner (only) to specify that the person requesting the download will
         * be charged for the download. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/RequesterPaysBuckets.html">Requester
         * Pays Buckets</a>.</p> <p>The following operations are related to
         * <code>PutBucketRequestPayment</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketRequestPayment.html">GetBucketRequestPayment</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketRequestPayment">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketRequestPaymentOutcome PutBucketRequestPayment(const Model::PutBucketRequestPaymentRequest& request) const;

        /**
         * A Callable wrapper for PutBucketRequestPayment that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketRequestPaymentRequestT = Model::PutBucketRequestPaymentRequest>
        Model::PutBucketRequestPaymentOutcomeCallable PutBucketRequestPaymentCallable(const PutBucketRequestPaymentRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketRequestPayment, request);
        }

        /**
         * An Async wrapper for PutBucketRequestPayment that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketRequestPaymentRequestT = Model::PutBucketRequestPaymentRequest>
        void PutBucketRequestPaymentAsync(const PutBucketRequestPaymentRequestT& request, const PutBucketRequestPaymentResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketRequestPayment, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the tags for a bucket.</p> <p>Use tags to organize your Amazon Web
         * Services bill to reflect your own cost structure. To do this, sign up to get
         * your Amazon Web Services account bill with tag key values included. Then, to see
         * the cost of combined resources, organize your billing information according to
         * resources with the same tag key values. For example, you can tag several
         * resources with a specific application name, and then organize your billing
         * information to see the total cost of that application across several services.
         * For more information, see <a
         * href="https://docs.aws.amazon.com/awsaccountbilling/latest/aboutv2/cost-alloc-tags.html">Cost
         * Allocation and Tagging</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/CostAllocTagging.html">Using
         * Cost Allocation in Amazon S3 Bucket Tags</a>.</p>  <p> When this operation
         * sets the tags for a bucket, it will overwrite any current tags the bucket
         * already has. You cannot use this operation to add tags to an existing list of
         * tags.</p>  <p>To use this operation, you must have permissions to perform
         * the <code>s3:PutBucketTagging</code> action. The bucket owner has this
         * permission by default and can grant this permission to others. For more
         * information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a>.</p> <p>
         * <code>PutBucketTagging</code> has the following special errors. For more Amazon
         * S3 errors see, <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html">Error
         * Responses</a>.</p> <ul> <li> <p> <code>InvalidTag</code> - The tag provided was
         * not a valid tag. This error can occur if the tag did not pass input validation.
         * For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/CostAllocTagging.html">Using
         * Cost Allocation in Amazon S3 Bucket Tags</a>.</p> </li> <li> <p>
         * <code>MalformedXML</code> - The XML provided does not match the schema.</p>
         * </li> <li> <p> <code>OperationAborted</code> - A conflicting conditional action
         * is currently in progress against this resource. Please try again.</p> </li> <li>
         * <p> <code>InternalError</code> - The service was unable to apply the provided
         * tag to the bucket.</p> </li> </ul> <p>The following operations are related to
         * <code>PutBucketTagging</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketTagging.html">GetBucketTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucketTagging.html">DeleteBucketTagging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketTagging">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketTaggingOutcome PutBucketTagging(const Model::PutBucketTaggingRequest& request) const;

        /**
         * A Callable wrapper for PutBucketTagging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketTaggingRequestT = Model::PutBucketTaggingRequest>
        Model::PutBucketTaggingOutcomeCallable PutBucketTaggingCallable(const PutBucketTaggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketTagging, request);
        }

        /**
         * An Async wrapper for PutBucketTagging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketTaggingRequestT = Model::PutBucketTaggingRequest>
        void PutBucketTaggingAsync(const PutBucketTaggingRequestT& request, const PutBucketTaggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketTagging, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         *  <p>When you enable versioning on a bucket for the first time, it might
         * take a short amount of time for the change to be fully propagated. While this
         * change is propagating, you may encounter intermittent <code>HTTP 404
         * NoSuchKey</code> errors for requests to objects created or updated after
         * enabling versioning. We recommend that you wait for 15 minutes after enabling
         * versioning before issuing write operations (<code>PUT</code> or
         * <code>DELETE</code>) on objects in the bucket. </p>  <p>Sets the
         * versioning state of an existing bucket.</p> <p>You can set the versioning state
         * with one of the following values:</p> <p> <b>Enabled</b>—Enables versioning for
         * the objects in the bucket. All objects added to the bucket receive a unique
         * version ID.</p> <p> <b>Suspended</b>—Disables versioning for the objects in the
         * bucket. All objects added to the bucket receive the version ID null.</p> <p>If
         * the versioning state has never been set on a bucket, it has no versioning state;
         * a <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketVersioning.html">GetBucketVersioning</a>
         * request does not return a versioning state value.</p> <p>In order to enable MFA
         * Delete, you must be the bucket owner. If you are the bucket owner and want to
         * enable MFA Delete in the bucket versioning configuration, you must include the
         * <code>x-amz-mfa request</code> header and the <code>Status</code> and the
         * <code>MfaDelete</code> request elements in a request to set the versioning state
         * of the bucket.</p>  <p>If you have an object expiration lifecycle
         * configuration in your non-versioned bucket and you want to maintain the same
         * permanent delete behavior when you enable versioning, you must add a noncurrent
         * expiration policy. The noncurrent expiration lifecycle configuration will manage
         * the deletes of the noncurrent object versions in the version-enabled bucket. (A
         * version-enabled bucket maintains one current and zero or more noncurrent object
         * versions.) For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lifecycle-mgmt.html#lifecycle-and-other-bucket-config">Lifecycle
         * and Versioning</a>.</p>  <p>The following operations are related to
         * <code>PutBucketVersioning</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateBucket.html">CreateBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteBucket.html">DeleteBucket</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketVersioning.html">GetBucketVersioning</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketVersioning">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketVersioningOutcome PutBucketVersioning(const Model::PutBucketVersioningRequest& request) const;

        /**
         * A Callable wrapper for PutBucketVersioning that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketVersioningRequestT = Model::PutBucketVersioningRequest>
        Model::PutBucketVersioningOutcomeCallable PutBucketVersioningCallable(const PutBucketVersioningRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketVersioning, request);
        }

        /**
         * An Async wrapper for PutBucketVersioning that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketVersioningRequestT = Model::PutBucketVersioningRequest>
        void PutBucketVersioningAsync(const PutBucketVersioningRequestT& request, const PutBucketVersioningResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketVersioning, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the configuration of the website that is specified in the
         * <code>website</code> subresource. To configure a bucket as a website, you can
         * add this subresource on the bucket with website configuration information such
         * as the file name of the index document and any redirect rules. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/WebsiteHosting.html">Hosting
         * Websites on Amazon S3</a>.</p> <p>This PUT action requires the
         * <code>S3:PutBucketWebsite</code> permission. By default, only the bucket owner
         * can configure the website attached to a bucket; however, bucket owners can allow
         * other users to set the website configuration by writing a bucket policy that
         * grants them the <code>S3:PutBucketWebsite</code> permission.</p> <p>To redirect
         * all website requests sent to the bucket's website endpoint, you add a website
         * configuration with the following elements. Because all requests are sent to
         * another website, you don't need to provide index document name for the
         * bucket.</p> <ul> <li> <p> <code>WebsiteConfiguration</code> </p> </li> <li> <p>
         * <code>RedirectAllRequestsTo</code> </p> </li> <li> <p> <code>HostName</code>
         * </p> </li> <li> <p> <code>Protocol</code> </p> </li> </ul> <p>If you want
         * granular control over redirects, you can use the following elements to add
         * routing rules that describe conditions for redirecting requests and information
         * about the redirect destination. In this case, the website configuration must
         * provide an index document for the bucket, because some requests might not be
         * redirected. </p> <ul> <li> <p> <code>WebsiteConfiguration</code> </p> </li> <li>
         * <p> <code>IndexDocument</code> </p> </li> <li> <p> <code>Suffix</code> </p>
         * </li> <li> <p> <code>ErrorDocument</code> </p> </li> <li> <p> <code>Key</code>
         * </p> </li> <li> <p> <code>RoutingRules</code> </p> </li> <li> <p>
         * <code>RoutingRule</code> </p> </li> <li> <p> <code>Condition</code> </p> </li>
         * <li> <p> <code>HttpErrorCodeReturnedEquals</code> </p> </li> <li> <p>
         * <code>KeyPrefixEquals</code> </p> </li> <li> <p> <code>Redirect</code> </p>
         * </li> <li> <p> <code>Protocol</code> </p> </li> <li> <p> <code>HostName</code>
         * </p> </li> <li> <p> <code>ReplaceKeyPrefixWith</code> </p> </li> <li> <p>
         * <code>ReplaceKeyWith</code> </p> </li> <li> <p> <code>HttpRedirectCode</code>
         * </p> </li> </ul> <p>Amazon S3 has a limitation of 50 routing rules per website
         * configuration. If you require more than 50 routing rules, you can use object
         * redirect. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/how-to-page-redirect.html">Configuring
         * an Object Redirect</a> in the <i>Amazon S3 User Guide</i>.</p> <p>The maximum
         * request length is limited to 128 KB.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutBucketWebsite">AWS
         * API Reference</a></p>
         */
        virtual Model::PutBucketWebsiteOutcome PutBucketWebsite(const Model::PutBucketWebsiteRequest& request) const;

        /**
         * A Callable wrapper for PutBucketWebsite that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutBucketWebsiteRequestT = Model::PutBucketWebsiteRequest>
        Model::PutBucketWebsiteOutcomeCallable PutBucketWebsiteCallable(const PutBucketWebsiteRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutBucketWebsite, request);
        }

        /**
         * An Async wrapper for PutBucketWebsite that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutBucketWebsiteRequestT = Model::PutBucketWebsiteRequest>
        void PutBucketWebsiteAsync(const PutBucketWebsiteRequestT& request, const PutBucketWebsiteResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutBucketWebsite, request, handler, context);
        }

        /**
         * <p>Adds an object to a bucket.</p>  <ul> <li> <p>Amazon S3 never adds
         * partial objects; if you receive a success response, Amazon S3 added the entire
         * object to the bucket. You cannot use <code>PutObject</code> to only update a
         * single piece of metadata for an existing object. You must put the entire object
         * with updated metadata if you want to update some values.</p> </li> <li> <p>If
         * your bucket uses the bucket owner enforced setting for Object Ownership, ACLs
         * are disabled and no longer affect permissions. All objects written to the bucket
         * by any account will be owned by the bucket owner.</p> </li> <li> <p>
         * <b>Directory buckets</b> - For directory buckets, you must make requests for
         * this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul>  <p>Amazon S3 is a distributed system. If it receives
         * multiple write requests for the same object simultaneously, it overwrites all
         * but the last object written. However, Amazon S3 provides features that can
         * modify this behavior:</p> <ul> <li> <p> <b>S3 Object Lock</b> - To prevent
         * objects from being deleted or overwritten, you can use <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lock.html">Amazon
         * S3 Object Lock</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>This
         * functionality is not supported for directory buckets.</p>  </li> <li> <p>
         * <b>S3 Versioning</b> - When you enable versioning for a bucket, if Amazon S3
         * receives multiple write requests for the same object simultaneously, it stores
         * all versions of the objects. For each write request that is made to the same
         * object, Amazon S3 automatically generates a unique version ID of that object
         * being stored in Amazon S3. You can retrieve, replace, or delete any version of
         * the object. For more information about versioning, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/AddingObjectstoVersioningEnabledBuckets.html">Adding
         * Objects to Versioning-Enabled Buckets</a> in the <i>Amazon S3 User Guide</i>.
         * For information about returning the versioning state of a bucket, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketVersioning.html">GetBucketVersioning</a>.
         * </p>  <p>This functionality is not supported for directory buckets.</p>
         *  </li> </ul> <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General
         * purpose bucket permissions</b> - The following permissions are required in your
         * policies when your <code>PutObject</code> request includes specific headers.</p>
         * <ul> <li> <p> <b> <code>s3:PutObject</code> </b> - To successfully complete the
         * <code>PutObject</code> request, you must always have the
         * <code>s3:PutObject</code> permission on a bucket to add an object to it.</p>
         * </li> <li> <p> <b> <code>s3:PutObjectAcl</code> </b> - To successfully change
         * the objects ACL of your <code>PutObject</code> request, you must have the
         * <code>s3:PutObjectAcl</code>.</p> </li> <li> <p> <b>
         * <code>s3:PutObjectTagging</code> </b> - To successfully set the tag-set with
         * your <code>PutObject</code> request, you must have the
         * <code>s3:PutObjectTagging</code>.</p> </li> </ul> </li> <li> <p> <b>Directory
         * bucket permissions</b> - To grant access to this API operation on a directory
         * bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> <p>If the object is encrypted with SSE-KMS,
         * you must also have the <code>kms:GenerateDataKey</code> and
         * <code>kms:Decrypt</code> permissions in IAM identity-based policies and KMS key
         * policies for the KMS key.</p> </li> </ul> </dd> <dt>Data integrity with
         * Content-MD5</dt> <dd> <ul> <li> <p> <b>General purpose bucket</b> - To ensure
         * that data is not corrupted traversing the network, use the
         * <code>Content-MD5</code> header. When you use this header, Amazon S3 checks the
         * object against the provided MD5 value and, if they do not match, Amazon S3
         * returns an error. Alternatively, when the object's ETag is its MD5 digest, you
         * can calculate the MD5 while putting the object to Amazon S3 and compare the
         * returned ETag to the calculated MD5 value.</p> </li> <li> <p> <b>Directory
         * bucket</b> - This functionality is not supported for directory buckets.</p>
         * </li> </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets
         * </b> - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>For more information about related Amazon S3 APIs, see the
         * following:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html">DeleteObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutObject">AWS API
         * Reference</a></p>
         */
        virtual Model::PutObjectOutcome PutObject(const Model::PutObjectRequest& request) const;

        /**
         * A Callable wrapper for PutObject that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        virtual Model::PutObjectOutcomeCallable PutObjectCallable(const Model::PutObjectRequest& request) const;

        /**
         * An Async wrapper for PutObject that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        virtual void PutObjectAsync(const Model::PutObjectRequest& request, const PutObjectResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const;

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Uses the <code>acl</code> subresource to set the access control list (ACL)
         * permissions for a new or existing object in an S3 bucket. You must have the
         * <code>WRITE_ACP</code> permission to set the ACL of an object. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html#permissions">What
         * permissions can I grant?</a> in the <i>Amazon S3 User Guide</i>.</p> <p>This
         * functionality is not supported for Amazon S3 on Outposts.</p> <p>Depending on
         * your application needs, you can choose to set the ACL on an object using either
         * the request body or the headers. For example, if you have an existing
         * application that updates a bucket ACL using the request body, you can continue
         * to use that approach. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
         * Control List (ACL) Overview</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p>If your bucket uses the bucket owner enforced setting for S3
         * Object Ownership, ACLs are disabled and no longer affect permissions. You must
         * use policies to grant access to your bucket and the objects in it. Requests to
         * set ACLs or update ACLs fail and return the
         * <code>AccessControlListNotSupported</code> error code. Requests to read ACLs are
         * still supported. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html">Controlling
         * object ownership</a> in the <i>Amazon S3 User Guide</i>.</p>  <dl>
         * <dt>Permissions</dt> <dd> <p>You can set access permissions using one of the
         * following methods:</p> <ul> <li> <p>Specify a canned ACL with the
         * <code>x-amz-acl</code> request header. Amazon S3 supports a set of predefined
         * ACLs, known as canned ACLs. Each canned ACL has a predefined set of grantees and
         * permissions. Specify the canned ACL name as the value of <code>x-amz-ac</code>l.
         * If you use this header, you cannot use other access control-specific headers in
         * your request. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html#CannedACL">Canned
         * ACL</a>.</p> </li> <li> <p>Specify access permissions explicitly with the
         * <code>x-amz-grant-read</code>, <code>x-amz-grant-read-acp</code>,
         * <code>x-amz-grant-write-acp</code>, and <code>x-amz-grant-full-control</code>
         * headers. When using these headers, you specify explicit access permissions and
         * grantees (Amazon Web Services accounts or Amazon S3 groups) who will receive the
         * permission. If you use these ACL-specific headers, you cannot use
         * <code>x-amz-acl</code> header to set a canned ACL. These parameters map to the
         * set of permissions that Amazon S3 supports in an ACL. For more information, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
         * Control List (ACL) Overview</a>.</p> <p>You specify each grantee as a type=value
         * pair, where the type is one of the following:</p> <ul> <li> <p> <code>id</code>
         * – if the value specified is the canonical user ID of an Amazon Web Services
         * account</p> </li> <li> <p> <code>uri</code> – if you are granting permissions to
         * a predefined group</p> </li> <li> <p> <code>emailAddress</code> – if the value
         * specified is the email address of an Amazon Web Services account</p> 
         * <p>Using email addresses to specify a grantee is only supported in the following
         * Amazon Web Services Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li>
         * <li> <p>US West (N. California)</p> </li> <li> <p> US West (Oregon)</p> </li>
         * <li> <p> Asia Pacific (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p>
         * </li> <li> <p>Asia Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li>
         * <li> <p>South America (São Paulo)</p> </li> </ul> <p>For a list of all the
         * Amazon S3 supported Regions and endpoints, see <a
         * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
         * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
         * </li> </ul> <p>For example, the following <code>x-amz-grant-read</code> header
         * grants list objects permission to the two Amazon Web Services accounts
         * identified by their email addresses.</p> <p> <code>x-amz-grant-read:
         * emailAddress="xyz@amazon.com", emailAddress="abc@amazon.com" </code> </p> </li>
         * </ul> <p>You can use either a canned ACL or specify access permissions
         * explicitly. You cannot do both.</p> </dd> <dt>Grantee Values</dt> <dd> <p>You
         * can specify the person (grantee) to whom you're assigning access rights (using
         * request elements) in the following ways:</p> <ul> <li> <p>By the person's
         * ID:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="CanonicalUser"&gt;&lt;ID&gt;&lt;&gt;ID&lt;&gt;&lt;/ID&gt;&lt;DisplayName&gt;&lt;&gt;GranteesEmail&lt;&gt;&lt;/DisplayName&gt;
         * &lt;/Grantee&gt;</code> </p> <p>DisplayName is optional and ignored in the
         * request.</p> </li> <li> <p>By URI:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="Group"&gt;&lt;URI&gt;&lt;&gt;http://acs.amazonaws.com/groups/global/AuthenticatedUsers&lt;&gt;&lt;/URI&gt;&lt;/Grantee&gt;</code>
         * </p> </li> <li> <p>By Email address:</p> <p> <code>&lt;Grantee
         * xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         * xsi:type="AmazonCustomerByEmail"&gt;&lt;EmailAddress&gt;&lt;&gt;Grantees@email.com&lt;&gt;&lt;/EmailAddress&gt;lt;/Grantee&gt;</code>
         * </p> <p>The grantee is resolved to the CanonicalUser and, in a response to a GET
         * Object acl request, appears as the CanonicalUser.</p>  <p>Using email
         * addresses to specify a grantee is only supported in the following Amazon Web
         * Services Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li> <li> <p>US
         * West (N. California)</p> </li> <li> <p> US West (Oregon)</p> </li> <li> <p> Asia
         * Pacific (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p> </li> <li>
         * <p>Asia Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li> <li>
         * <p>South America (São Paulo)</p> </li> </ul> <p>For a list of all the Amazon S3
         * supported Regions and endpoints, see <a
         * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
         * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
         * </li> </ul> </dd> <dt>Versioning</dt> <dd> <p>The ACL of an object is set at the
         * object version level. By default, PUT sets the ACL of the current version of an
         * object. To set the ACL of a different version, use the <code>versionId</code>
         * subresource.</p> </dd> </dl> <p>The following operations are related to
         * <code>PutObjectAcl</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutObjectAcl">AWS API
         * Reference</a></p>
         */
        virtual Model::PutObjectAclOutcome PutObjectAcl(const Model::PutObjectAclRequest& request) const;

        /**
         * A Callable wrapper for PutObjectAcl that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutObjectAclRequestT = Model::PutObjectAclRequest>
        Model::PutObjectAclOutcomeCallable PutObjectAclCallable(const PutObjectAclRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutObjectAcl, request);
        }

        /**
         * An Async wrapper for PutObjectAcl that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutObjectAclRequestT = Model::PutObjectAclRequest>
        void PutObjectAclAsync(const PutObjectAclRequestT& request, const PutObjectAclResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutObjectAcl, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Applies a legal hold configuration to the specified object. For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Locking
         * Objects</a>.</p> <p>This functionality is not supported for Amazon S3 on
         * Outposts.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutObjectLegalHold">AWS
         * API Reference</a></p>
         */
        virtual Model::PutObjectLegalHoldOutcome PutObjectLegalHold(const Model::PutObjectLegalHoldRequest& request) const;

        /**
         * A Callable wrapper for PutObjectLegalHold that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutObjectLegalHoldRequestT = Model::PutObjectLegalHoldRequest>
        Model::PutObjectLegalHoldOutcomeCallable PutObjectLegalHoldCallable(const PutObjectLegalHoldRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutObjectLegalHold, request);
        }

        /**
         * An Async wrapper for PutObjectLegalHold that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutObjectLegalHoldRequestT = Model::PutObjectLegalHoldRequest>
        void PutObjectLegalHoldAsync(const PutObjectLegalHoldRequestT& request, const PutObjectLegalHoldResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutObjectLegalHold, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Places an Object Lock configuration on the specified bucket. The rule
         * specified in the Object Lock configuration will be applied by default to every
         * new object placed in the specified bucket. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Locking
         * Objects</a>. </p>  <ul> <li> <p>The <code>DefaultRetention</code> settings
         * require both a mode and a period.</p> </li> <li> <p>The
         * <code>DefaultRetention</code> period can be either <code>Days</code> or
         * <code>Years</code> but you must select one. You cannot specify <code>Days</code>
         * and <code>Years</code> at the same time.</p> </li> <li> <p>You can enable Object
         * Lock for new or existing buckets. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lock-configure.html">Configuring
         * Object Lock</a>.</p> </li> </ul> <p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutObjectLockConfiguration">AWS
         * API Reference</a></p>
         */
        virtual Model::PutObjectLockConfigurationOutcome PutObjectLockConfiguration(const Model::PutObjectLockConfigurationRequest& request) const;

        /**
         * A Callable wrapper for PutObjectLockConfiguration that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutObjectLockConfigurationRequestT = Model::PutObjectLockConfigurationRequest>
        Model::PutObjectLockConfigurationOutcomeCallable PutObjectLockConfigurationCallable(const PutObjectLockConfigurationRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutObjectLockConfiguration, request);
        }

        /**
         * An Async wrapper for PutObjectLockConfiguration that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutObjectLockConfigurationRequestT = Model::PutObjectLockConfigurationRequest>
        void PutObjectLockConfigurationAsync(const PutObjectLockConfigurationRequestT& request, const PutObjectLockConfigurationResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutObjectLockConfiguration, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Places an Object Retention configuration on an object. For more information,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Locking
         * Objects</a>. Users or accounts require the <code>s3:PutObjectRetention</code>
         * permission in order to place an Object Retention configuration on objects.
         * Bypassing a Governance Retention configuration requires the
         * <code>s3:BypassGovernanceRetention</code> permission. </p> <p>This functionality
         * is not supported for Amazon S3 on Outposts.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutObjectRetention">AWS
         * API Reference</a></p>
         */
        virtual Model::PutObjectRetentionOutcome PutObjectRetention(const Model::PutObjectRetentionRequest& request) const;

        /**
         * A Callable wrapper for PutObjectRetention that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutObjectRetentionRequestT = Model::PutObjectRetentionRequest>
        Model::PutObjectRetentionOutcomeCallable PutObjectRetentionCallable(const PutObjectRetentionRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutObjectRetention, request);
        }

        /**
         * An Async wrapper for PutObjectRetention that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutObjectRetentionRequestT = Model::PutObjectRetentionRequest>
        void PutObjectRetentionAsync(const PutObjectRetentionRequestT& request, const PutObjectRetentionResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutObjectRetention, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Sets the supplied tag-set to an object that already exists in a bucket. A tag
         * is a key-value pair. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-tagging.html">Object
         * Tagging</a>.</p> <p>You can associate tags with an object by sending a PUT
         * request against the tagging subresource that is associated with the object. You
         * can retrieve tags by sending a GET request. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectTagging.html">GetObjectTagging</a>.</p>
         * <p>For tagging-related restrictions related to characters and encodings, see <a
         * href="https://docs.aws.amazon.com/awsaccountbilling/latest/aboutv2/allocation-tag-restrictions.html">Tag
         * Restrictions</a>. Note that Amazon S3 limits the maximum number of tags to 10
         * tags per object.</p> <p>To use this operation, you must have permission to
         * perform the <code>s3:PutObjectTagging</code> action. By default, the bucket
         * owner has this permission and can grant this permission to others.</p> <p>To put
         * tags of any other version, use the <code>versionId</code> query parameter. You
         * also need permission for the <code>s3:PutObjectVersionTagging</code> action.</p>
         * <p> <code>PutObjectTagging</code> has the following special errors. For more
         * Amazon S3 errors see, <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html">Error
         * Responses</a>.</p> <ul> <li> <p> <code>InvalidTag</code> - The tag provided was
         * not a valid tag. This error can occur if the tag did not pass input validation.
         * For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-tagging.html">Object
         * Tagging</a>.</p> </li> <li> <p> <code>MalformedXML</code> - The XML provided
         * does not match the schema.</p> </li> <li> <p> <code>OperationAborted</code> - A
         * conflicting conditional action is currently in progress against this resource.
         * Please try again.</p> </li> <li> <p> <code>InternalError</code> - The service
         * was unable to apply the provided tag to the object.</p> </li> </ul> <p>The
         * following operations are related to <code>PutObjectTagging</code>:</p> <ul> <li>
         * <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectTagging.html">GetObjectTagging</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObjectTagging.html">DeleteObjectTagging</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutObjectTagging">AWS
         * API Reference</a></p>
         */
        virtual Model::PutObjectTaggingOutcome PutObjectTagging(const Model::PutObjectTaggingRequest& request) const;

        /**
         * A Callable wrapper for PutObjectTagging that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutObjectTaggingRequestT = Model::PutObjectTaggingRequest>
        Model::PutObjectTaggingOutcomeCallable PutObjectTaggingCallable(const PutObjectTaggingRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutObjectTagging, request);
        }

        /**
         * An Async wrapper for PutObjectTagging that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutObjectTaggingRequestT = Model::PutObjectTaggingRequest>
        void PutObjectTaggingAsync(const PutObjectTaggingRequestT& request, const PutObjectTaggingResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutObjectTagging, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Creates or modifies the <code>PublicAccessBlock</code> configuration for an
         * Amazon S3 bucket. To use this operation, you must have the
         * <code>s3:PutBucketPublicAccessBlock</code> permission. For more information
         * about Amazon S3 permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a>.</p>  <p>When Amazon S3 evaluates the
         * <code>PublicAccessBlock</code> configuration for a bucket or an object, it
         * checks the <code>PublicAccessBlock</code> configuration for both the bucket (or
         * the bucket that contains the object) and the bucket owner's account. If the
         * <code>PublicAccessBlock</code> configurations are different between the bucket
         * and the account, Amazon S3 uses the most restrictive combination of the
         * bucket-level and account-level settings.</p>  <p>For more
         * information about when Amazon S3 considers a bucket or an object public, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html#access-control-block-public-access-policy-status">The
         * Meaning of "Public"</a>.</p> <p>The following operations are related to
         * <code>PutPublicAccessBlock</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetPublicAccessBlock.html">GetPublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeletePublicAccessBlock.html">DeletePublicAccessBlock</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketPolicyStatus.html">GetBucketPolicyStatus</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/access-control-block-public-access.html">Using
         * Amazon S3 Block Public Access</a> </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PutPublicAccessBlock">AWS
         * API Reference</a></p>
         */
        virtual Model::PutPublicAccessBlockOutcome PutPublicAccessBlock(const Model::PutPublicAccessBlockRequest& request) const;

        /**
         * A Callable wrapper for PutPublicAccessBlock that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename PutPublicAccessBlockRequestT = Model::PutPublicAccessBlockRequest>
        Model::PutPublicAccessBlockOutcomeCallable PutPublicAccessBlockCallable(const PutPublicAccessBlockRequestT& request) const
        {
            return SubmitCallable(&S3Client::PutPublicAccessBlock, request);
        }

        /**
         * An Async wrapper for PutPublicAccessBlock that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename PutPublicAccessBlockRequestT = Model::PutPublicAccessBlockRequest>
        void PutPublicAccessBlockAsync(const PutPublicAccessBlockRequestT& request, const PutPublicAccessBlockResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::PutPublicAccessBlock, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Restores an archived copy of an object back into Amazon S3</p> <p>This
         * functionality is not supported for Amazon S3 on Outposts.</p> <p>This action
         * performs the following types of requests: </p> <ul> <li> <p> <code>restore an
         * archive</code> - Restore an archived object</p> </li> </ul> <p>For more
         * information about the <code>S3</code> structure in the request body, see the
         * following:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html">PutObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/S3_ACLs_UsingACLs.html">Managing
         * Access with ACLs</a> in the <i>Amazon S3 User Guide</i> </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html">Protecting
         * Data Using Server-Side Encryption</a> in the <i>Amazon S3 User Guide</i> </p>
         * </li> </ul> <dl> <dt>Permissions</dt> <dd> <p>To use this operation, you must
         * have permissions to perform the <code>s3:RestoreObject</code> action. The bucket
         * owner has this permission by default and can grant this permission to others.
         * For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-with-s3-actions.html#using-with-s3-actions-related-to-bucket-subresources">Permissions
         * Related to Bucket Subresource Operations</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-access-control.html">Managing
         * Access Permissions to Your Amazon S3 Resources</a> in the <i>Amazon S3 User
         * Guide</i>.</p> </dd> <dt>Restoring objects</dt> <dd> <p>Objects that you archive
         * to the S3 Glacier Flexible Retrieval Flexible Retrieval or S3 Glacier Deep
         * Archive storage class, and S3 Intelligent-Tiering Archive or S3
         * Intelligent-Tiering Deep Archive tiers, are not accessible in real time. For
         * objects in the S3 Glacier Flexible Retrieval Flexible Retrieval or S3 Glacier
         * Deep Archive storage classes, you must first initiate a restore request, and
         * then wait until a temporary copy of the object is available. If you want a
         * permanent copy of the object, create a copy of it in the Amazon S3 Standard
         * storage class in your S3 bucket. To access an archived object, you must restore
         * the object for the duration (number of days) that you specify. For objects in
         * the Archive Access or Deep Archive Access tiers of S3 Intelligent-Tiering, you
         * must first initiate a restore request, and then wait until the object is moved
         * into the Frequent Access tier.</p> <p>To restore a specific object version, you
         * can provide a version ID. If you don't provide a version ID, Amazon S3 restores
         * the current version.</p> <p>When restoring an archived object, you can specify
         * one of the following data access tier options in the <code>Tier</code> element
         * of the request body: </p> <ul> <li> <p> <code>Expedited</code> - Expedited
         * retrievals allow you to quickly access your data stored in the S3 Glacier
         * Flexible Retrieval Flexible Retrieval storage class or S3 Intelligent-Tiering
         * Archive tier when occasional urgent requests for restoring archives are
         * required. For all but the largest archived objects (250 MB+), data accessed
         * using Expedited retrievals is typically made available within 1–5 minutes.
         * Provisioned capacity ensures that retrieval capacity for Expedited retrievals is
         * available when you need it. Expedited retrievals and provisioned capacity are
         * not available for objects stored in the S3 Glacier Deep Archive storage class or
         * S3 Intelligent-Tiering Deep Archive tier.</p> </li> <li> <p>
         * <code>Standard</code> - Standard retrievals allow you to access any of your
         * archived objects within several hours. This is the default option for retrieval
         * requests that do not specify the retrieval option. Standard retrievals typically
         * finish within 3–5 hours for objects stored in the S3 Glacier Flexible Retrieval
         * Flexible Retrieval storage class or S3 Intelligent-Tiering Archive tier. They
         * typically finish within 12 hours for objects stored in the S3 Glacier Deep
         * Archive storage class or S3 Intelligent-Tiering Deep Archive tier. Standard
         * retrievals are free for objects stored in S3 Intelligent-Tiering.</p> </li> <li>
         * <p> <code>Bulk</code> - Bulk retrievals free for objects stored in the S3
         * Glacier Flexible Retrieval and S3 Intelligent-Tiering storage classes, enabling
         * you to retrieve large amounts, even petabytes, of data at no cost. Bulk
         * retrievals typically finish within 5–12 hours for objects stored in the S3
         * Glacier Flexible Retrieval Flexible Retrieval storage class or S3
         * Intelligent-Tiering Archive tier. Bulk retrievals are also the lowest-cost
         * retrieval option when restoring objects from S3 Glacier Deep Archive. They
         * typically finish within 48 hours for objects stored in the S3 Glacier Deep
         * Archive storage class or S3 Intelligent-Tiering Deep Archive tier. </p> </li>
         * </ul> <p>For more information about archive retrieval options and provisioned
         * capacity for <code>Expedited</code> data access, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/restoring-objects.html">Restoring
         * Archived Objects</a> in the <i>Amazon S3 User Guide</i>. </p> <p>You can use
         * Amazon S3 restore speed upgrade to change the restore speed to a faster speed
         * while it is in progress. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/restoring-objects.html#restoring-objects-upgrade-tier.title.html">
         * Upgrading the speed of an in-progress restore</a> in the <i>Amazon S3 User
         * Guide</i>. </p> <p>To get the status of object restoration, you can send a
         * <code>HEAD</code> request. Operations return the <code>x-amz-restore</code>
         * header, which provides information about the restoration status, in the
         * response. You can use Amazon S3 event notifications to notify you when a restore
         * is initiated or completed. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Configuring
         * Amazon S3 Event Notifications</a> in the <i>Amazon S3 User Guide</i>.</p>
         * <p>After restoring an archived object, you can update the restoration period by
         * reissuing the request with a new period. Amazon S3 updates the restoration
         * period relative to the current time and charges only for the request-there are
         * no data transfer charges. You cannot update the restoration period when Amazon
         * S3 is actively processing your current restore request for the object.</p> <p>If
         * your bucket has a lifecycle configuration with a rule that includes an
         * expiration action, the object expiration overrides the life span that you
         * specify in a restore request. For example, if you restore an object copy for 10
         * days, but the object is scheduled to expire in 3 days, Amazon S3 deletes the
         * object in 3 days. For more information about lifecycle configuration, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html">PutBucketLifecycleConfiguration</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lifecycle-mgmt.html">Object
         * Lifecycle Management</a> in <i>Amazon S3 User Guide</i>.</p> </dd>
         * <dt>Responses</dt> <dd> <p>A successful action returns either the <code>200
         * OK</code> or <code>202 Accepted</code> status code. </p> <ul> <li> <p>If the
         * object is not previously restored, then Amazon S3 returns <code>202
         * Accepted</code> in the response. </p> </li> <li> <p>If the object is previously
         * restored, Amazon S3 returns <code>200 OK</code> in the response. </p> </li>
         * </ul> <ul> <li> <p>Special errors:</p> <ul> <li> <p> <i>Code:
         * RestoreAlreadyInProgress</i> </p> </li> <li> <p> <i>Cause: Object restore is
         * already in progress.</i> </p> </li> <li> <p> <i>HTTP Status Code: 409
         * Conflict</i> </p> </li> <li> <p> <i>SOAP Fault Code Prefix: Client</i> </p>
         * </li> </ul> </li> <li> <ul> <li> <p> <i>Code:
         * GlacierExpeditedRetrievalNotAvailable</i> </p> </li> <li> <p> <i>Cause:
         * expedited retrievals are currently not available. Try again later. (Returned if
         * there is insufficient capacity to process the Expedited request. This error
         * applies only to Expedited retrievals and not to S3 Standard or Bulk
         * retrievals.)</i> </p> </li> <li> <p> <i>HTTP Status Code: 503</i> </p> </li>
         * <li> <p> <i>SOAP Fault Code Prefix: N/A</i> </p> </li> </ul> </li> </ul> </dd>
         * </dl> <p>The following operations are related to <code>RestoreObject</code>:</p>
         * <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html">PutBucketLifecycleConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketNotificationConfiguration.html">GetBucketNotificationConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/RestoreObject">AWS
         * API Reference</a></p>
         */
        virtual Model::RestoreObjectOutcome RestoreObject(const Model::RestoreObjectRequest& request) const;

        /**
         * A Callable wrapper for RestoreObject that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename RestoreObjectRequestT = Model::RestoreObjectRequest>
        Model::RestoreObjectOutcomeCallable RestoreObjectCallable(const RestoreObjectRequestT& request) const
        {
            return SubmitCallable(&S3Client::RestoreObject, request);
        }

        /**
         * An Async wrapper for RestoreObject that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename RestoreObjectRequestT = Model::RestoreObjectRequest>
        void RestoreObjectAsync(const RestoreObjectRequestT& request, const RestoreObjectResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::RestoreObject, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>This action filters the contents of an Amazon S3 object based on a simple
         * structured query language (SQL) statement. In the request, along with the SQL
         * expression, you must also specify a data serialization format (JSON, CSV, or
         * Apache Parquet) of the object. Amazon S3 uses this format to parse object data
         * into records, and returns only records that match the specified SQL expression.
         * You must also specify the data serialization format for the response.</p>
         * <p>This functionality is not supported for Amazon S3 on Outposts.</p> <p>For
         * more information about Amazon S3 Select, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/selecting-content-from-objects.html">Selecting
         * Content from Objects</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-glacier-select-sql-reference-select.html">SELECT
         * Command</a> in the <i>Amazon S3 User Guide</i>.</p> <p/> <dl>
         * <dt>Permissions</dt> <dd> <p>You must have the <code>s3:GetObject</code>
         * permission for this operation. Amazon S3 Select does not support anonymous
         * access. For more information about permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/using-with-s3-actions.html">Specifying
         * Permissions in a Policy</a> in the <i>Amazon S3 User Guide</i>.</p> </dd>
         * <dt>Object Data Formats</dt> <dd> <p>You can use Amazon S3 Select to query
         * objects that have the following format properties:</p> <ul> <li> <p> <i>CSV,
         * JSON, and Parquet</i> - Objects must be in CSV, JSON, or Parquet format.</p>
         * </li> <li> <p> <i>UTF-8</i> - UTF-8 is the only encoding type Amazon S3 Select
         * supports.</p> </li> <li> <p> <i>GZIP or BZIP2</i> - CSV and JSON files can be
         * compressed using GZIP or BZIP2. GZIP and BZIP2 are the only compression formats
         * that Amazon S3 Select supports for CSV and JSON files. Amazon S3 Select supports
         * columnar compression for Parquet using GZIP or Snappy. Amazon S3 Select does not
         * support whole-object compression for Parquet objects.</p> </li> <li> <p>
         * <i>Server-side encryption</i> - Amazon S3 Select supports querying objects that
         * are protected with server-side encryption.</p> <p>For objects that are encrypted
         * with customer-provided encryption keys (SSE-C), you must use HTTPS, and you must
         * use the headers that are documented in the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>.
         * For more information about SSE-C, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ServerSideEncryptionCustomerKeys.html">Server-Side
         * Encryption (Using Customer-Provided Encryption Keys)</a> in the <i>Amazon S3
         * User Guide</i>.</p> <p>For objects that are encrypted with Amazon S3 managed
         * keys (SSE-S3) and Amazon Web Services KMS keys (SSE-KMS), server-side encryption
         * is handled transparently, so you don't need to specify anything. For more
         * information about server-side encryption, including SSE-S3 and SSE-KMS, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html">Protecting
         * Data Using Server-Side Encryption</a> in the <i>Amazon S3 User Guide</i>.</p>
         * </li> </ul> </dd> <dt>Working with the Response Body</dt> <dd> <p>Given the
         * response size is unknown, Amazon S3 Select streams the response as a series of
         * messages and includes a <code>Transfer-Encoding</code> header with
         * <code>chunked</code> as its value in the response. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTSelectObjectAppendix.html">Appendix:
         * SelectObjectContent Response</a>.</p> </dd> <dt>GetObject Support</dt> <dd>
         * <p>The <code>SelectObjectContent</code> action does not support the following
         * <code>GetObject</code> functionality. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>.</p>
         * <ul> <li> <p> <code>Range</code>: Although you can specify a scan range for an
         * Amazon S3 Select request (see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_SelectObjectContent.html#AmazonS3-SelectObjectContent-request-ScanRange">SelectObjectContentRequest
         * - ScanRange</a> in the request parameters), you cannot specify the range of
         * bytes of an object to return. </p> </li> <li> <p>The <code>GLACIER</code>,
         * <code>DEEP_ARCHIVE</code>, and <code>REDUCED_REDUNDANCY</code> storage classes,
         * or the <code>ARCHIVE_ACCESS</code> and <code>DEEP_ARCHIVE_ACCESS</code> access
         * tiers of the <code>INTELLIGENT_TIERING</code> storage class: You cannot query
         * objects in the <code>GLACIER</code>, <code>DEEP_ARCHIVE</code>, or
         * <code>REDUCED_REDUNDANCY</code> storage classes, nor objects in the
         * <code>ARCHIVE_ACCESS</code> or <code>DEEP_ARCHIVE_ACCESS</code> access tiers of
         * the <code>INTELLIGENT_TIERING</code> storage class. For more information about
         * storage classes, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/storage-class-intro.html">Using
         * Amazon S3 storage classes</a> in the <i>Amazon S3 User Guide</i>.</p> </li>
         * </ul> </dd> <dt>Special Errors</dt> <dd> <p>For a list of special errors for
         * this operation, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html#SelectObjectContentErrorCodeList">List
         * of SELECT Object Content Error Codes</a> </p> </dd> </dl> <p>The following
         * operations are related to <code>SelectObjectContent</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetBucketLifecycleConfiguration.html">GetBucketLifecycleConfiguration</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html">PutBucketLifecycleConfiguration</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/SelectObjectContent">AWS
         * API Reference</a></p>
         */
        virtual Model::SelectObjectContentOutcome SelectObjectContent(Model::SelectObjectContentRequest& request) const;

        /**
         * A Callable wrapper for SelectObjectContent that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename SelectObjectContentRequestT = Model::SelectObjectContentRequest>
        Model::SelectObjectContentOutcomeCallable SelectObjectContentCallable(SelectObjectContentRequestT& request) const
        {
            return SubmitCallable(&S3Client::SelectObjectContent, request);
        }

        /**
         * An Async wrapper for SelectObjectContent that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename SelectObjectContentRequestT = Model::SelectObjectContentRequest>
        void SelectObjectContentAsync(SelectObjectContentRequestT& request, const SelectObjectContentResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::SelectObjectContent, request, handler, context);
        }

        /**
         * <p>Uploads a part in a multipart upload.</p>  <p>In this operation, you
         * provide new data as a part of an object in your request. However, you have an
         * option to specify your existing Amazon S3 object as a data source for the part
         * you are uploading. To upload a part from an existing object, you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>
         * operation. </p>  <p>You must initiate a multipart upload (see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>)
         * before you can upload any part. In response to your initiate request, Amazon S3
         * returns an upload ID, a unique identifier that you must include in your upload
         * part request.</p> <p>Part numbers can be any number from 1 to 10,000, inclusive.
         * A part number uniquely identifies a part and also defines its position within
         * the object being created. If you upload a new part using the same part number
         * that was used with a previous part, the previously uploaded part is
         * overwritten.</p> <p>For information about maximum and minimum part sizes and
         * other multipart upload specifications, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/qfacts.html">Multipart
         * upload limits</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>After you
         * initiate multipart upload and upload one or more parts, you must either complete
         * or abort multipart upload in order to stop getting charged for storage of the
         * uploaded parts. Only after you either complete or abort multipart upload, Amazon
         * S3 frees up the parts storage and stops charging you for the parts storage.</p>
         *  <p>For more information on multipart uploads, go to <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html">Multipart
         * Upload Overview</a> in the <i>Amazon S3 User Guide </i>.</p>  <p>
         * <b>Directory buckets</b> - For directory buckets, you must make requests for
         * this API operation to the Zonal endpoint. These endpoints support
         * virtual-hosted-style requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Permissions</dt> <dd> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - To perform a multipart upload with encryption using an Key
         * Management Service key, the requester must have permission to the
         * <code>kms:Decrypt</code> and <code>kms:GenerateDataKey</code> actions on the
         * key. The requester must also have permissions for the
         * <code>kms:GenerateDataKey</code> action for the
         * <code>CreateMultipartUpload</code> API. Then, the requester needs permissions
         * for the <code>kms:Decrypt</code> action on the <code>UploadPart</code> and
         * <code>UploadPartCopy</code> APIs.</p> <p>These permissions are required because
         * Amazon S3 must decrypt and read data from the encrypted file parts before it
         * completes the multipart upload. For more information about KMS permissions, see
         * <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/UsingKMSEncryption.html">Protecting
         * data using server-side encryption with KMS</a> in the <i>Amazon S3 User
         * Guide</i>. For information about the permissions required to use the multipart
         * upload API, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuAndPermissions.html">Multipart
         * upload and permissions</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html#mpuAndPermissions">Multipart
         * upload API and permissions</a> in the <i>Amazon S3 User Guide</i>.</p> </li>
         * <li> <p> <b>Directory bucket permissions</b> - To grant access to this API
         * operation on a directory bucket, we recommend that you use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a> API operation for session-based authorization.
         * Specifically, you grant the <code>s3express:CreateSession</code> permission to
         * the directory bucket in a bucket policy or an IAM identity-based policy. Then,
         * you make the <code>CreateSession</code> API call on the bucket to obtain a
         * session token. With the session token in your request header, you can make API
         * requests to this operation. After the session token expires, you make another
         * <code>CreateSession</code> API call to generate a new session token for use.
         * Amazon Web Services CLI or SDKs create session and refresh the session token
         * automatically to avoid service interruptions when a session expires. For more
         * information about authorization, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html">
         * <code>CreateSession</code> </a>.</p> <p>If the object is encrypted with SSE-KMS,
         * you must also have the <code>kms:GenerateDataKey</code> and
         * <code>kms:Decrypt</code> permissions in IAM identity-based policies and KMS key
         * policies for the KMS key.</p> </li> </ul> </dd> <dt>Data integrity</dt> <dd> <p>
         * <b>General purpose bucket</b> - To ensure that data is not corrupted traversing
         * the network, specify the <code>Content-MD5</code> header in the upload part
         * request. Amazon S3 checks the part data against the provided MD5 value. If they
         * do not match, Amazon S3 returns an error. If the upload request is signed with
         * Signature Version 4, then Amazon Web Services S3 uses the
         * <code>x-amz-content-sha256</code> header as a checksum instead of
         * <code>Content-MD5</code>. For more information see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-auth-using-authorization-header.html">Authenticating
         * Requests: Using the Authorization Header (Amazon Web Services Signature Version
         * 4)</a>. </p>  <p> <b>Directory buckets</b> - MD5 is not supported by
         * directory buckets. You can use checksum algorithms to check object
         * integrity.</p>  </dd> <dt>Encryption</dt> <dd> <ul> <li> <p> <b>General
         * purpose bucket</b> - Server-side encryption is for data encryption at rest.
         * Amazon S3 encrypts your data as it writes it to disks in its data centers and
         * decrypts it when you access it. You have mutually exclusive options to protect
         * data using server-side encryption in Amazon S3, depending on how you choose to
         * manage the encryption keys. Specifically, the encryption key options are Amazon
         * S3 managed keys (SSE-S3), Amazon Web Services KMS keys (SSE-KMS), and
         * Customer-Provided Keys (SSE-C). Amazon S3 encrypts data with server-side
         * encryption using Amazon S3 managed keys (SSE-S3) by default. You can optionally
         * tell Amazon S3 to encrypt data at rest using server-side encryption with other
         * key options. The option you use depends on whether you want to use KMS keys
         * (SSE-KMS) or provide your own encryption key (SSE-C).</p> <p>Server-side
         * encryption is supported by the S3 Multipart Upload operations. Unless you are
         * using a customer-provided encryption key (SSE-C), you don't need to specify the
         * encryption parameters in each UploadPart request. Instead, you only need to
         * specify the server-side encryption parameters in the initial Initiate Multipart
         * request. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>.</p>
         * <p>If you request server-side encryption using a customer-provided encryption
         * key (SSE-C) in your initiate multipart upload request, you must provide
         * identical encryption information in each part upload using the following request
         * headers.</p> <ul> <li> <p>x-amz-server-side-encryption-customer-algorithm</p>
         * </li> <li> <p>x-amz-server-side-encryption-customer-key</p> </li> <li>
         * <p>x-amz-server-side-encryption-customer-key-MD5</p> </li> </ul> <p> For more
         * information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/UsingServerSideEncryption.html">Using
         * Server-Side Encryption</a> in the <i>Amazon S3 User Guide</i>.</p> </li> <li>
         * <p> <b>Directory buckets </b> - For directory buckets, there are only two
         * supported options for server-side encryption: server-side encryption with Amazon
         * S3 managed keys (SSE-S3) (<code>AES256</code>) and server-side encryption with
         * KMS keys (SSE-KMS) (<code>aws:kms</code>).</p> </li> </ul> </dd> <dt>Special
         * errors</dt> <dd> <ul> <li> <p>Error Code: <code>NoSuchUpload</code> </p> <ul>
         * <li> <p>Description: The specified multipart upload does not exist. The upload
         * ID might be invalid, or the multipart upload might have been aborted or
         * completed.</p> </li> <li> <p>HTTP Status Code: 404 Not Found </p> </li> <li>
         * <p>SOAP Fault Code Prefix: Client</p> </li> </ul> </li> </ul> </dd> <dt>HTTP
         * Host header syntax</dt> <dd> <p> <b>Directory buckets </b> - The HTTP Host
         * header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>UploadPart</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListMultipartUploads.html">ListMultipartUploads</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/UploadPart">AWS API
         * Reference</a></p>
         */
        virtual Model::UploadPartOutcome UploadPart(const Model::UploadPartRequest& request) const;

        /**
         * A Callable wrapper for UploadPart that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UploadPartRequestT = Model::UploadPartRequest>
        Model::UploadPartOutcomeCallable UploadPartCallable(const UploadPartRequestT& request) const
        {
            return SubmitCallable(&S3Client::UploadPart, request);
        }

        /**
         * An Async wrapper for UploadPart that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UploadPartRequestT = Model::UploadPartRequest>
        void UploadPartAsync(const UploadPartRequestT& request, const UploadPartResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::UploadPart, request, handler, context);
        }

        /**
         * <p>Uploads a part by copying data from an existing object as data source. To
         * specify the data source, you add the request header
         * <code>x-amz-copy-source</code> in your request. To specify a byte range, you add
         * the request header <code>x-amz-copy-source-range</code> in your request. </p>
         * <p>For information about maximum and minimum part sizes and other multipart
         * upload specifications, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/qfacts.html">Multipart
         * upload limits</a> in the <i>Amazon S3 User Guide</i>. </p>  <p>Instead of
         * copying data from an existing object as part data, you might use the <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * action to upload new data as a part of an object in your request.</p> 
         * <p>You must initiate a multipart upload before you can upload any part. In
         * response to your initiate request, Amazon S3 returns the upload ID, a unique
         * identifier that you must include in your upload part request.</p> <p>For
         * conceptual information about multipart uploads, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/uploadobjusingmpu.html">Uploading
         * Objects Using Multipart Upload</a> in the <i>Amazon S3 User Guide</i>. For
         * information about copying objects using a single atomic action vs. a multipart
         * upload, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ObjectOperations.html">Operations
         * on Objects</a> in the <i>Amazon S3 User Guide</i>.</p>  <p> <b>Directory
         * buckets</b> - For directory buckets, you must make requests for this API
         * operation to the Zonal endpoint. These endpoints support virtual-hosted-style
         * requests in the format
         * <code>https://<i>bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com/<i>key-name</i>
         * </code>. Path-style requests are not supported. For more information about
         * endpoints in Availability Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-Regions-and-Zones.html">Regional
         * and Zonal endpoints for directory buckets in Availability Zones</a> in the
         * <i>Amazon S3 User Guide</i>. For more information about endpoints in Local
         * Zones, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-lzs-for-directory-buckets.html">Available
         * Local Zone for directory buckets</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <dl> <dt>Authentication and authorization</dt> <dd> <p>All
         * <code>UploadPartCopy</code> requests must be authenticated and signed by using
         * IAM credentials (access key ID and secret access key for the IAM identities).
         * All headers with the <code>x-amz-</code> prefix, including
         * <code>x-amz-copy-source</code>, must be signed. For more information, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/RESTAuthentication.html">REST
         * Authentication</a>.</p> <p> <b>Directory buckets</b> - You must use IAM
         * credentials to authenticate and authorize your access to the
         * <code>UploadPartCopy</code> API operation, instead of using the temporary
         * security credentials through the <code>CreateSession</code> API operation.</p>
         * <p>Amazon Web Services CLI or SDKs handles authentication and authorization on
         * your behalf.</p> </dd> <dt>Permissions</dt> <dd> <p>You must have
         * <code>READ</code> access to the source object and <code>WRITE</code> access to
         * the destination bucket.</p> <ul> <li> <p> <b>General purpose bucket
         * permissions</b> - You must have the permissions in a policy based on the bucket
         * types of your source bucket and destination bucket in an
         * <code>UploadPartCopy</code> operation.</p> <ul> <li> <p>If the source object is
         * in a general purpose bucket, you must have the <b> <code>s3:GetObject</code>
         * </b> permission to read the source object that is being copied. </p> </li> <li>
         * <p>If the destination bucket is a general purpose bucket, you must have the <b>
         * <code>s3:PutObject</code> </b> permission to write the object copy to the
         * destination bucket. </p> </li> <li> <p>To perform a multipart upload with
         * encryption using an Key Management Service key, the requester must have
         * permission to the <code>kms:Decrypt</code> and <code>kms:GenerateDataKey</code>
         * actions on the key. The requester must also have permissions for the
         * <code>kms:GenerateDataKey</code> action for the
         * <code>CreateMultipartUpload</code> API. Then, the requester needs permissions
         * for the <code>kms:Decrypt</code> action on the <code>UploadPart</code> and
         * <code>UploadPartCopy</code> APIs. These permissions are required because Amazon
         * S3 must decrypt and read data from the encrypted file parts before it completes
         * the multipart upload. For more information about KMS permissions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/UsingKMSEncryption.html">Protecting
         * data using server-side encryption with KMS</a> in the <i>Amazon S3 User
         * Guide</i>. For information about the permissions required to use the multipart
         * upload API, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuAndPermissions.html">Multipart
         * upload and permissions</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html#mpuAndPermissions">Multipart
         * upload API and permissions</a> in the <i>Amazon S3 User Guide</i>.</p> </li>
         * </ul> </li> <li> <p> <b>Directory bucket permissions</b> - You must have
         * permissions in a bucket policy or an IAM identity-based policy based on the
         * source and destination bucket types in an <code>UploadPartCopy</code>
         * operation.</p> <ul> <li> <p>If the source object that you want to copy is in a
         * directory bucket, you must have the <b> <code>s3express:CreateSession</code>
         * </b> permission in the <code>Action</code> element of a policy to read the
         * object. By default, the session is in the <code>ReadWrite</code> mode. If you
         * want to restrict the access, you can explicitly set the
         * <code>s3express:SessionMode</code> condition key to <code>ReadOnly</code> on the
         * copy source bucket.</p> </li> <li> <p>If the copy destination is a directory
         * bucket, you must have the <b> <code>s3express:CreateSession</code> </b>
         * permission in the <code>Action</code> element of a policy to write the object to
         * the destination. The <code>s3express:SessionMode</code> condition key cannot be
         * set to <code>ReadOnly</code> on the copy destination. </p> </li> </ul> <p>If the
         * object is encrypted with SSE-KMS, you must also have the
         * <code>kms:GenerateDataKey</code> and <code>kms:Decrypt</code> permissions in IAM
         * identity-based policies and KMS key policies for the KMS key.</p> <p>For example
         * policies, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-example-bucket-policies.html">Example
         * bucket policies for S3 Express One Zone</a> and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-security-iam-identity-policies.html">Amazon
         * Web Services Identity and Access Management (IAM) identity-based policies for S3
         * Express One Zone</a> in the <i>Amazon S3 User Guide</i>.</p> </li> </ul> </dd>
         * <dt>Encryption</dt> <dd> <ul> <li> <p> <b>General purpose buckets </b> - For
         * information about using server-side encryption with customer-provided encryption
         * keys with the <code>UploadPartCopy</code> operation, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>
         * and <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>.
         * </p> </li> <li> <p> <b>Directory buckets </b> - For directory buckets, there are
         * only two supported options for server-side encryption: server-side encryption
         * with Amazon S3 managed keys (SSE-S3) (<code>AES256</code>) and server-side
         * encryption with KMS keys (SSE-KMS) (<code>aws:kms</code>). For more information,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-express-serv-side-encryption.html">Protecting
         * data with server-side encryption</a> in the <i>Amazon S3 User Guide</i>.</p>
         *  <p>For directory buckets, when you perform a
         * <code>CreateMultipartUpload</code> operation and an <code>UploadPartCopy</code>
         * operation, the request headers you provide in the
         * <code>CreateMultipartUpload</code> request must match the default encryption
         * configuration of the destination bucket. </p>  <p>S3 Bucket Keys aren't
         * supported, when you copy SSE-KMS encrypted objects from general purpose buckets
         * to directory buckets, from directory buckets to general purpose buckets, or
         * between directory buckets, through <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>.
         * In this case, Amazon S3 makes a call to KMS every time a copy request is made
         * for a KMS-encrypted object.</p> </li> </ul> </dd> <dt>Special errors</dt> <dd>
         * <ul> <li> <p>Error Code: <code>NoSuchUpload</code> </p> <ul> <li>
         * <p>Description: The specified multipart upload does not exist. The upload ID
         * might be invalid, or the multipart upload might have been aborted or
         * completed.</p> </li> <li> <p>HTTP Status Code: 404 Not Found</p> </li> </ul>
         * </li> <li> <p>Error Code: <code>InvalidRequest</code> </p> <ul> <li>
         * <p>Description: The specified copy source is not supported as a byte-range copy
         * source.</p> </li> <li> <p>HTTP Status Code: 400 Bad Request</p> </li> </ul>
         * </li> </ul> </dd> <dt>HTTP Host header syntax</dt> <dd> <p> <b>Directory buckets
         * </b> - The HTTP Host header syntax is <code>
         * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.</p>
         * </dd> </dl> <p>The following operations are related to
         * <code>UploadPartCopy</code>:</p> <ul> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html">CreateMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html">UploadPart</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html">CompleteMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html">AbortMultipartUpload</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListParts.html">ListParts</a>
         * </p> </li> <li> <p> <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListMultipartUploads.html">ListMultipartUploads</a>
         * </p> </li> </ul><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/UploadPartCopy">AWS
         * API Reference</a></p>
         */
        virtual Model::UploadPartCopyOutcome UploadPartCopy(const Model::UploadPartCopyRequest& request) const;

        /**
         * A Callable wrapper for UploadPartCopy that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename UploadPartCopyRequestT = Model::UploadPartCopyRequest>
        Model::UploadPartCopyOutcomeCallable UploadPartCopyCallable(const UploadPartCopyRequestT& request) const
        {
            return SubmitCallable(&S3Client::UploadPartCopy, request);
        }

        /**
         * An Async wrapper for UploadPartCopy that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename UploadPartCopyRequestT = Model::UploadPartCopyRequest>
        void UploadPartCopyAsync(const UploadPartCopyRequestT& request, const UploadPartCopyResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::UploadPartCopy, request, handler, context);
        }

        /**
         *  <p>This operation is not supported for directory buckets.</p> 
         * <p>Passes transformed objects to a <code>GetObject</code> operation when using
         * Object Lambda access points. For information about Object Lambda access points,
         * see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/transforming-objects.html">Transforming
         * objects with Object Lambda access points</a> in the <i>Amazon S3 User
         * Guide</i>.</p> <p>This operation supports metadata that can be returned by <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html">GetObject</a>,
         * in addition to <code>RequestRoute</code>, <code>RequestToken</code>,
         * <code>StatusCode</code>, <code>ErrorCode</code>, and <code>ErrorMessage</code>.
         * The <code>GetObject</code> response metadata is supported so that the
         * <code>WriteGetObjectResponse</code> caller, typically an Lambda function, can
         * provide the same metadata when it internally invokes <code>GetObject</code>.
         * When <code>WriteGetObjectResponse</code> is called by a customer-owned Lambda
         * function, the metadata returned to the end user <code>GetObject</code> call
         * might differ from what Amazon S3 would normally return.</p> <p>You can include
         * any number of metadata headers. When including a metadata header, it should be
         * prefaced with <code>x-amz-meta</code>. For example,
         * <code>x-amz-meta-my-custom-header: MyCustomValue</code>. The primary use case
         * for this is to forward <code>GetObject</code> metadata.</p> <p>Amazon Web
         * Services provides some prebuilt Lambda functions that you can use with S3 Object
         * Lambda to detect and redact personally identifiable information (PII) and
         * decompress S3 objects. These Lambda functions are available in the Amazon Web
         * Services Serverless Application Repository, and can be selected through the
         * Amazon Web Services Management Console when you create your Object Lambda access
         * point.</p> <p>Example 1: PII Access Control - This Lambda function uses Amazon
         * Comprehend, a natural language processing (NLP) service using machine learning
         * to find insights and relationships in text. It automatically detects personally
         * identifiable information (PII) such as names, addresses, dates, credit card
         * numbers, and social security numbers from documents in your Amazon S3 bucket.
         * </p> <p>Example 2: PII Redaction - This Lambda function uses Amazon Comprehend,
         * a natural language processing (NLP) service using machine learning to find
         * insights and relationships in text. It automatically redacts personally
         * identifiable information (PII) such as names, addresses, dates, credit card
         * numbers, and social security numbers from documents in your Amazon S3 bucket.
         * </p> <p>Example 3: Decompression - The Lambda function
         * S3ObjectLambdaDecompression, is equipped to decompress objects stored in S3 in
         * one of six compressed file formats including bzip2, gzip, snappy, zlib,
         * zstandard and ZIP. </p> <p>For information on how to view and use these
         * functions, see <a
         * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/olap-examples.html">Using
         * Amazon Web Services built Lambda functions</a> in the <i>Amazon S3 User
         * Guide</i>.</p><p><h3>See Also:</h3>   <a
         * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/WriteGetObjectResponse">AWS
         * API Reference</a></p>
         */
        virtual Model::WriteGetObjectResponseOutcome WriteGetObjectResponse(const Model::WriteGetObjectResponseRequest& request) const;

        /**
         * A Callable wrapper for WriteGetObjectResponse that returns a future to the operation so that it can be executed in parallel to other requests.
         */
        template<typename WriteGetObjectResponseRequestT = Model::WriteGetObjectResponseRequest>
        Model::WriteGetObjectResponseOutcomeCallable WriteGetObjectResponseCallable(const WriteGetObjectResponseRequestT& request) const
        {
            return SubmitCallable(&S3Client::WriteGetObjectResponse, request);
        }

        /**
         * An Async wrapper for WriteGetObjectResponse that queues the request into a thread executor and triggers associated callback when operation has finished.
         */
        template<typename WriteGetObjectResponseRequestT = Model::WriteGetObjectResponseRequest>
        void WriteGetObjectResponseAsync(const WriteGetObjectResponseRequestT& request, const WriteGetObjectResponseResponseReceivedHandler& handler, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            return SubmitAsync(&S3Client::WriteGetObjectResponse, request, handler, context);
        }


        Aws::String GeneratePresignedUrl(const Aws::String& bucket,
                                         const Aws::String& key,
                                         Aws::Http::HttpMethod method,
                                         uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);

        Aws::String GeneratePresignedUrl(const Aws::String& bucket,
                                         const Aws::String& key,
                                         Aws::Http::HttpMethod method,
                                         const Http::HeaderValueCollection& customizedHeaders,
                                         uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);

        /**
         * Server Side Encryption Headers and Algorithm
         * Method    Algorithm    Required Headers
         * SSE-S3    AES256       x-amz-server-side-encryption:AES256
         * SSE-KMS   aws:kms      x-amz-server-side--encryption:aws:kms, x-amz-server-side-encryption-aws-kms-key-id:<kmsMasterKeyId>
         * SS3-C     AES256       x-amz-server-side-encryption-customer-algorithm:AES256, x-amz-server-side-encryption-customer-key:<base64EncodedKey>, x-amz-server-side-encryption-customer-key-MD5:<Base64EncodedMD5ofNonBase64EncodedKey>
         */
        /**
         * Generate presigned URL with Sever Side Encryption(SSE) and with S3 managed keys.
         * https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html (algo: AES256)
         */
        Aws::String GeneratePresignedUrlWithSSES3(const Aws::String& bucket,
                                                  const Aws::String& key,
                                                  Aws::Http::HttpMethod method,
                                                  uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);
        /**
         * Generate presigned URL with Sever Side Encryption(SSE) and with S3 managed keys.
         * https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html (algo: AES256)
         * Header: "x-amz-server-side-encryption" will be added internally, don't customize it.
         */
        Aws::String GeneratePresignedUrlWithSSES3(const Aws::String& bucket,
                                                  const Aws::String& key,
                                                  Aws::Http::HttpMethod method,
                                                  Http::HeaderValueCollection customizedHeaders,
                                                  uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);

        /**
         * Generate presigned URL with Server Side Encryption(SSE) and with KMS master key id.
         * if kmsMasterKeyId is empty, we will end up use the default one generated by KMS for you. You can find it via AWS IAM console, it's the one aliased as "aws/s3".
         * https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html (algo: aws:kms)
         */
        Aws::String GeneratePresignedUrlWithSSEKMS(const Aws::String& bucket,
                                                   const Aws::String& key,
                                                   Aws::Http::HttpMethod method,
                                                   const Aws::String& kmsMasterKeyId = "",
                                                   uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);
        /**
         * Generate presigned URL with Server Side Encryption(SSE) and with KMS master key id.
         * if kmsMasterKeyId is empty, we will end up use the default one generated by KMS for you. You can find it via AWS IAM console, it's the one aliased as "aws/s3".
         * https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html (algo: aws:kms)
         * Headers: "x-amz-server-side-encryption" and "x-amz-server-side-encryption-aws-kms-key-id" will be added internally, don't customize them.
         */
        Aws::String GeneratePresignedUrlWithSSEKMS(const Aws::String& bucket,
                                                   const Aws::String& key,
                                                   Aws::Http::HttpMethod method,
                                                   Http::HeaderValueCollection customizedHeaders,
                                                   const Aws::String& kmsMasterKeyId = "",
                                                   uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);

        /**
         * Generate presigned URL with Sever Side Encryption(SSE) and with customer supplied Key.
         * https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html (algo: AES256)
         */
        Aws::String GeneratePresignedUrlWithSSEC(const Aws::String& bucket,
                                                 const Aws::String& key,
                                                 Aws::Http::HttpMethod method,
                                                 const Aws::String& base64EncodedAES256Key,
                                                 uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);
        /**
         * Generate presigned URL with Sever Side Encryption(SSE) and with customer supplied Key.
         * https://docs.aws.amazon.com/AmazonS3/latest/dev/serv-side-encryption.html (algo: AES256)
         * Headers: "x-amz-server-side-encryption-customer-algorithm","x-amz-server-side-encryption-customer-key" and "x-amz-server-side-encryption-customer-key-MD5" will be added internally, don't customize them.
         */
        Aws::String GeneratePresignedUrlWithSSEC(const Aws::String& bucket,
                                                 const Aws::String& key,
                                                 Aws::Http::HttpMethod method,
                                                 Http::HeaderValueCollection customizedHeaders,
                                                 const Aws::String& base64EncodedAES256Key,
                                                 uint64_t expirationInSeconds = MAX_EXPIRATION_SECONDS);


        virtual bool MultipartUploadSupported() const;

        void OverrideEndpoint(const Aws::String& endpoint);
        std::shared_ptr<S3EndpointProviderBase>& accessEndpointProvider();

    private:
        friend class Aws::Client::ClientWithAsyncTemplateMethods<S3Client>;
        void init(const S3ClientConfiguration& clientConfiguration);
        S3ClientConfiguration m_clientConfiguration;
        std::shared_ptr<S3EndpointProviderBase> m_endpointProvider;
    };

  } // namespace S3
} // namespace Aws
