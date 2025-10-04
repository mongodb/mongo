/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/s3/model/ObjectCannedACL.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/s3/model/StorageClass.h>
#include <aws/s3/model/RequestPayer.h>
#include <aws/s3/model/ObjectLockMode.h>
#include <aws/s3/model/ObjectLockLegalHoldStatus.h>
#include <aws/s3/model/ChecksumAlgorithm.h>
#include <utility>

namespace Aws
{
namespace Http
{
    class URI;
} //namespace Http
namespace S3
{
namespace Model
{

  /**
   */
  class CreateMultipartUploadRequest : public S3Request
  {
  public:
    AWS_S3_API CreateMultipartUploadRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateMultipartUpload"; }

    AWS_S3_API Aws::String SerializePayload() const override;

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;
    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_S3_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The canned ACL to apply to the object. Amazon S3 supports a set of predefined
     * ACLs, known as <i>canned ACLs</i>. Each canned ACL has a predefined set of
     * grantees and permissions. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html#CannedACL">Canned
     * ACL</a> in the <i>Amazon S3 User Guide</i>.</p> <p>By default, all objects are
     * private. Only the owner has full access control. When uploading an object, you
     * can grant access permissions to individual Amazon Web Services accounts or to
     * predefined groups defined by Amazon S3. These permissions are then added to the
     * access control list (ACL) on the new object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/S3_ACLs_UsingACLs.html">Using
     * ACLs</a>. One way to grant the permissions using the request headers is to
     * specify a canned ACL with the <code>x-amz-acl</code> request header.</p> 
     * <ul> <li> <p>This functionality is not supported for directory buckets.</p>
     * </li> <li> <p>This functionality is not supported for Amazon S3 on Outposts.</p>
     * </li> </ul> 
     */
    inline const ObjectCannedACL& GetACL() const{ return m_aCL; }
    inline bool ACLHasBeenSet() const { return m_aCLHasBeenSet; }
    inline void SetACL(const ObjectCannedACL& value) { m_aCLHasBeenSet = true; m_aCL = value; }
    inline void SetACL(ObjectCannedACL&& value) { m_aCLHasBeenSet = true; m_aCL = std::move(value); }
    inline CreateMultipartUploadRequest& WithACL(const ObjectCannedACL& value) { SetACL(value); return *this;}
    inline CreateMultipartUploadRequest& WithACL(ObjectCannedACL&& value) { SetACL(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the bucket where the multipart upload is initiated and where the
     * object is uploaded.</p> <p> <b>Directory buckets</b> - When you use this
     * operation with a directory bucket, you must use virtual-hosted-style requests in
     * the format <code>
     * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.
     * Path-style requests are not supported. Directory bucket names must be unique in
     * the chosen Zone (Availability Zone or Local Zone). Bucket names must follow the
     * format <code> <i>bucket-base-name</i>--<i>zone-id</i>--x-s3</code> (for example,
     * <code> <i>DOC-EXAMPLE-BUCKET</i>--<i>usw2-az1</i>--x-s3</code>). For information
     * about bucket naming restrictions, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-bucket-naming-rules.html">Directory
     * bucket naming rules</a> in the <i>Amazon S3 User Guide</i>.</p> <p> <b>Access
     * points</b> - When you use this action with an access point, you must provide the
     * alias of the access point in place of the bucket name or specify the access
     * point ARN. When using the access point ARN, you must direct requests to the
     * access point hostname. The access point hostname takes the form
     * <i>AccessPointName</i>-<i>AccountId</i>.s3-accesspoint.<i>Region</i>.amazonaws.com.
     * When using this action with an access point through the Amazon Web Services
     * SDKs, you provide the access point ARN in place of the bucket name. For more
     * information about access point ARNs, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-access-points.html">Using
     * access points</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>Access
     * points and Object Lambda access points are not supported by directory
     * buckets.</p>  <p> <b>S3 on Outposts</b> - When you use this action with
     * Amazon S3 on Outposts, you must direct requests to the S3 on Outposts hostname.
     * The S3 on Outposts hostname takes the form <code>
     * <i>AccessPointName</i>-<i>AccountId</i>.<i>outpostID</i>.s3-outposts.<i>Region</i>.amazonaws.com</code>.
     * When you use this action with S3 on Outposts through the Amazon Web Services
     * SDKs, you provide the Outposts access point ARN in place of the bucket name. For
     * more information about S3 on Outposts ARNs, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/S3onOutposts.html">What
     * is S3 on Outposts?</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline CreateMultipartUploadRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline CreateMultipartUploadRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies caching behavior along the request/reply chain.</p>
     */
    inline const Aws::String& GetCacheControl() const{ return m_cacheControl; }
    inline bool CacheControlHasBeenSet() const { return m_cacheControlHasBeenSet; }
    inline void SetCacheControl(const Aws::String& value) { m_cacheControlHasBeenSet = true; m_cacheControl = value; }
    inline void SetCacheControl(Aws::String&& value) { m_cacheControlHasBeenSet = true; m_cacheControl = std::move(value); }
    inline void SetCacheControl(const char* value) { m_cacheControlHasBeenSet = true; m_cacheControl.assign(value); }
    inline CreateMultipartUploadRequest& WithCacheControl(const Aws::String& value) { SetCacheControl(value); return *this;}
    inline CreateMultipartUploadRequest& WithCacheControl(Aws::String&& value) { SetCacheControl(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithCacheControl(const char* value) { SetCacheControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies presentational information for the object.</p>
     */
    inline const Aws::String& GetContentDisposition() const{ return m_contentDisposition; }
    inline bool ContentDispositionHasBeenSet() const { return m_contentDispositionHasBeenSet; }
    inline void SetContentDisposition(const Aws::String& value) { m_contentDispositionHasBeenSet = true; m_contentDisposition = value; }
    inline void SetContentDisposition(Aws::String&& value) { m_contentDispositionHasBeenSet = true; m_contentDisposition = std::move(value); }
    inline void SetContentDisposition(const char* value) { m_contentDispositionHasBeenSet = true; m_contentDisposition.assign(value); }
    inline CreateMultipartUploadRequest& WithContentDisposition(const Aws::String& value) { SetContentDisposition(value); return *this;}
    inline CreateMultipartUploadRequest& WithContentDisposition(Aws::String&& value) { SetContentDisposition(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithContentDisposition(const char* value) { SetContentDisposition(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies what content encodings have been applied to the object and thus
     * what decoding mechanisms must be applied to obtain the media-type referenced by
     * the Content-Type header field.</p>  <p>For directory buckets, only the
     * <code>aws-chunked</code> value is supported in this header field.</p> 
     */
    inline const Aws::String& GetContentEncoding() const{ return m_contentEncoding; }
    inline bool ContentEncodingHasBeenSet() const { return m_contentEncodingHasBeenSet; }
    inline void SetContentEncoding(const Aws::String& value) { m_contentEncodingHasBeenSet = true; m_contentEncoding = value; }
    inline void SetContentEncoding(Aws::String&& value) { m_contentEncodingHasBeenSet = true; m_contentEncoding = std::move(value); }
    inline void SetContentEncoding(const char* value) { m_contentEncodingHasBeenSet = true; m_contentEncoding.assign(value); }
    inline CreateMultipartUploadRequest& WithContentEncoding(const Aws::String& value) { SetContentEncoding(value); return *this;}
    inline CreateMultipartUploadRequest& WithContentEncoding(Aws::String&& value) { SetContentEncoding(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithContentEncoding(const char* value) { SetContentEncoding(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The language that the content is in.</p>
     */
    inline const Aws::String& GetContentLanguage() const{ return m_contentLanguage; }
    inline bool ContentLanguageHasBeenSet() const { return m_contentLanguageHasBeenSet; }
    inline void SetContentLanguage(const Aws::String& value) { m_contentLanguageHasBeenSet = true; m_contentLanguage = value; }
    inline void SetContentLanguage(Aws::String&& value) { m_contentLanguageHasBeenSet = true; m_contentLanguage = std::move(value); }
    inline void SetContentLanguage(const char* value) { m_contentLanguageHasBeenSet = true; m_contentLanguage.assign(value); }
    inline CreateMultipartUploadRequest& WithContentLanguage(const Aws::String& value) { SetContentLanguage(value); return *this;}
    inline CreateMultipartUploadRequest& WithContentLanguage(Aws::String&& value) { SetContentLanguage(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithContentLanguage(const char* value) { SetContentLanguage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A standard MIME type describing the format of the object data.</p>
     */
    inline const Aws::String& GetContentType() const{ return m_contentType; }
    inline bool ContentTypeHasBeenSet() const { return m_contentTypeHasBeenSet; }
    inline void SetContentType(const Aws::String& value) { m_contentTypeHasBeenSet = true; m_contentType = value; }
    inline void SetContentType(Aws::String&& value) { m_contentTypeHasBeenSet = true; m_contentType = std::move(value); }
    inline void SetContentType(const char* value) { m_contentTypeHasBeenSet = true; m_contentType.assign(value); }
    inline CreateMultipartUploadRequest& WithContentType(const Aws::String& value) { SetContentType(value); return *this;}
    inline CreateMultipartUploadRequest& WithContentType(Aws::String&& value) { SetContentType(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithContentType(const char* value) { SetContentType(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time at which the object is no longer cacheable.</p>
     */
    inline const Aws::Utils::DateTime& GetExpires() const{ return m_expires; }
    inline bool ExpiresHasBeenSet() const { return m_expiresHasBeenSet; }
    inline void SetExpires(const Aws::Utils::DateTime& value) { m_expiresHasBeenSet = true; m_expires = value; }
    inline void SetExpires(Aws::Utils::DateTime&& value) { m_expiresHasBeenSet = true; m_expires = std::move(value); }
    inline CreateMultipartUploadRequest& WithExpires(const Aws::Utils::DateTime& value) { SetExpires(value); return *this;}
    inline CreateMultipartUploadRequest& WithExpires(Aws::Utils::DateTime&& value) { SetExpires(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify access permissions explicitly to give the grantee READ, READ_ACP, and
     * WRITE_ACP permissions on the object.</p> <p>By default, all objects are private.
     * Only the owner has full access control. When uploading an object, you can use
     * this header to explicitly grant access permissions to specific Amazon Web
     * Services accounts or groups. This header maps to specific permissions that
     * Amazon S3 supports in an ACL. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
     * Control List (ACL) Overview</a> in the <i>Amazon S3 User Guide</i>.</p> <p>You
     * specify each grantee as a type=value pair, where the type is one of the
     * following:</p> <ul> <li> <p> <code>id</code> – if the value specified is the
     * canonical user ID of an Amazon Web Services account</p> </li> <li> <p>
     * <code>uri</code> – if you are granting permissions to a predefined group</p>
     * </li> <li> <p> <code>emailAddress</code> – if the value specified is the email
     * address of an Amazon Web Services account</p>  <p>Using email addresses to
     * specify a grantee is only supported in the following Amazon Web Services
     * Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li> <li> <p>US West (N.
     * California)</p> </li> <li> <p> US West (Oregon)</p> </li> <li> <p> Asia Pacific
     * (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p> </li> <li> <p>Asia
     * Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li> <li> <p>South
     * America (São Paulo)</p> </li> </ul> <p>For a list of all the Amazon S3 supported
     * Regions and endpoints, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
     * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
     * </li> </ul> <p>For example, the following <code>x-amz-grant-read</code> header
     * grants the Amazon Web Services accounts identified by account IDs permissions to
     * read object data and its metadata:</p> <p> <code>x-amz-grant-read:
     * id="11112222333", id="444455556666" </code> </p>  <ul> <li> <p>This
     * functionality is not supported for directory buckets.</p> </li> <li> <p>This
     * functionality is not supported for Amazon S3 on Outposts.</p> </li> </ul>
     * 
     */
    inline const Aws::String& GetGrantFullControl() const{ return m_grantFullControl; }
    inline bool GrantFullControlHasBeenSet() const { return m_grantFullControlHasBeenSet; }
    inline void SetGrantFullControl(const Aws::String& value) { m_grantFullControlHasBeenSet = true; m_grantFullControl = value; }
    inline void SetGrantFullControl(Aws::String&& value) { m_grantFullControlHasBeenSet = true; m_grantFullControl = std::move(value); }
    inline void SetGrantFullControl(const char* value) { m_grantFullControlHasBeenSet = true; m_grantFullControl.assign(value); }
    inline CreateMultipartUploadRequest& WithGrantFullControl(const Aws::String& value) { SetGrantFullControl(value); return *this;}
    inline CreateMultipartUploadRequest& WithGrantFullControl(Aws::String&& value) { SetGrantFullControl(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithGrantFullControl(const char* value) { SetGrantFullControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify access permissions explicitly to allow grantee to read the object
     * data and its metadata.</p> <p>By default, all objects are private. Only the
     * owner has full access control. When uploading an object, you can use this header
     * to explicitly grant access permissions to specific Amazon Web Services accounts
     * or groups. This header maps to specific permissions that Amazon S3 supports in
     * an ACL. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
     * Control List (ACL) Overview</a> in the <i>Amazon S3 User Guide</i>.</p> <p>You
     * specify each grantee as a type=value pair, where the type is one of the
     * following:</p> <ul> <li> <p> <code>id</code> – if the value specified is the
     * canonical user ID of an Amazon Web Services account</p> </li> <li> <p>
     * <code>uri</code> – if you are granting permissions to a predefined group</p>
     * </li> <li> <p> <code>emailAddress</code> – if the value specified is the email
     * address of an Amazon Web Services account</p>  <p>Using email addresses to
     * specify a grantee is only supported in the following Amazon Web Services
     * Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li> <li> <p>US West (N.
     * California)</p> </li> <li> <p> US West (Oregon)</p> </li> <li> <p> Asia Pacific
     * (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p> </li> <li> <p>Asia
     * Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li> <li> <p>South
     * America (São Paulo)</p> </li> </ul> <p>For a list of all the Amazon S3 supported
     * Regions and endpoints, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
     * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
     * </li> </ul> <p>For example, the following <code>x-amz-grant-read</code> header
     * grants the Amazon Web Services accounts identified by account IDs permissions to
     * read object data and its metadata:</p> <p> <code>x-amz-grant-read:
     * id="11112222333", id="444455556666" </code> </p>  <ul> <li> <p>This
     * functionality is not supported for directory buckets.</p> </li> <li> <p>This
     * functionality is not supported for Amazon S3 on Outposts.</p> </li> </ul>
     * 
     */
    inline const Aws::String& GetGrantRead() const{ return m_grantRead; }
    inline bool GrantReadHasBeenSet() const { return m_grantReadHasBeenSet; }
    inline void SetGrantRead(const Aws::String& value) { m_grantReadHasBeenSet = true; m_grantRead = value; }
    inline void SetGrantRead(Aws::String&& value) { m_grantReadHasBeenSet = true; m_grantRead = std::move(value); }
    inline void SetGrantRead(const char* value) { m_grantReadHasBeenSet = true; m_grantRead.assign(value); }
    inline CreateMultipartUploadRequest& WithGrantRead(const Aws::String& value) { SetGrantRead(value); return *this;}
    inline CreateMultipartUploadRequest& WithGrantRead(Aws::String&& value) { SetGrantRead(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithGrantRead(const char* value) { SetGrantRead(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify access permissions explicitly to allows grantee to read the object
     * ACL.</p> <p>By default, all objects are private. Only the owner has full access
     * control. When uploading an object, you can use this header to explicitly grant
     * access permissions to specific Amazon Web Services accounts or groups. This
     * header maps to specific permissions that Amazon S3 supports in an ACL. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
     * Control List (ACL) Overview</a> in the <i>Amazon S3 User Guide</i>.</p> <p>You
     * specify each grantee as a type=value pair, where the type is one of the
     * following:</p> <ul> <li> <p> <code>id</code> – if the value specified is the
     * canonical user ID of an Amazon Web Services account</p> </li> <li> <p>
     * <code>uri</code> – if you are granting permissions to a predefined group</p>
     * </li> <li> <p> <code>emailAddress</code> – if the value specified is the email
     * address of an Amazon Web Services account</p>  <p>Using email addresses to
     * specify a grantee is only supported in the following Amazon Web Services
     * Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li> <li> <p>US West (N.
     * California)</p> </li> <li> <p> US West (Oregon)</p> </li> <li> <p> Asia Pacific
     * (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p> </li> <li> <p>Asia
     * Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li> <li> <p>South
     * America (São Paulo)</p> </li> </ul> <p>For a list of all the Amazon S3 supported
     * Regions and endpoints, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
     * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
     * </li> </ul> <p>For example, the following <code>x-amz-grant-read</code> header
     * grants the Amazon Web Services accounts identified by account IDs permissions to
     * read object data and its metadata:</p> <p> <code>x-amz-grant-read:
     * id="11112222333", id="444455556666" </code> </p>  <ul> <li> <p>This
     * functionality is not supported for directory buckets.</p> </li> <li> <p>This
     * functionality is not supported for Amazon S3 on Outposts.</p> </li> </ul>
     * 
     */
    inline const Aws::String& GetGrantReadACP() const{ return m_grantReadACP; }
    inline bool GrantReadACPHasBeenSet() const { return m_grantReadACPHasBeenSet; }
    inline void SetGrantReadACP(const Aws::String& value) { m_grantReadACPHasBeenSet = true; m_grantReadACP = value; }
    inline void SetGrantReadACP(Aws::String&& value) { m_grantReadACPHasBeenSet = true; m_grantReadACP = std::move(value); }
    inline void SetGrantReadACP(const char* value) { m_grantReadACPHasBeenSet = true; m_grantReadACP.assign(value); }
    inline CreateMultipartUploadRequest& WithGrantReadACP(const Aws::String& value) { SetGrantReadACP(value); return *this;}
    inline CreateMultipartUploadRequest& WithGrantReadACP(Aws::String&& value) { SetGrantReadACP(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithGrantReadACP(const char* value) { SetGrantReadACP(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify access permissions explicitly to allows grantee to allow grantee to
     * write the ACL for the applicable object.</p> <p>By default, all objects are
     * private. Only the owner has full access control. When uploading an object, you
     * can use this header to explicitly grant access permissions to specific Amazon
     * Web Services accounts or groups. This header maps to specific permissions that
     * Amazon S3 supports in an ACL. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/acl-overview.html">Access
     * Control List (ACL) Overview</a> in the <i>Amazon S3 User Guide</i>.</p> <p>You
     * specify each grantee as a type=value pair, where the type is one of the
     * following:</p> <ul> <li> <p> <code>id</code> – if the value specified is the
     * canonical user ID of an Amazon Web Services account</p> </li> <li> <p>
     * <code>uri</code> – if you are granting permissions to a predefined group</p>
     * </li> <li> <p> <code>emailAddress</code> – if the value specified is the email
     * address of an Amazon Web Services account</p>  <p>Using email addresses to
     * specify a grantee is only supported in the following Amazon Web Services
     * Regions: </p> <ul> <li> <p>US East (N. Virginia)</p> </li> <li> <p>US West (N.
     * California)</p> </li> <li> <p> US West (Oregon)</p> </li> <li> <p> Asia Pacific
     * (Singapore)</p> </li> <li> <p>Asia Pacific (Sydney)</p> </li> <li> <p>Asia
     * Pacific (Tokyo)</p> </li> <li> <p>Europe (Ireland)</p> </li> <li> <p>South
     * America (São Paulo)</p> </li> </ul> <p>For a list of all the Amazon S3 supported
     * Regions and endpoints, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
     * and Endpoints</a> in the Amazon Web Services General Reference.</p> 
     * </li> </ul> <p>For example, the following <code>x-amz-grant-read</code> header
     * grants the Amazon Web Services accounts identified by account IDs permissions to
     * read object data and its metadata:</p> <p> <code>x-amz-grant-read:
     * id="11112222333", id="444455556666" </code> </p>  <ul> <li> <p>This
     * functionality is not supported for directory buckets.</p> </li> <li> <p>This
     * functionality is not supported for Amazon S3 on Outposts.</p> </li> </ul>
     * 
     */
    inline const Aws::String& GetGrantWriteACP() const{ return m_grantWriteACP; }
    inline bool GrantWriteACPHasBeenSet() const { return m_grantWriteACPHasBeenSet; }
    inline void SetGrantWriteACP(const Aws::String& value) { m_grantWriteACPHasBeenSet = true; m_grantWriteACP = value; }
    inline void SetGrantWriteACP(Aws::String&& value) { m_grantWriteACPHasBeenSet = true; m_grantWriteACP = std::move(value); }
    inline void SetGrantWriteACP(const char* value) { m_grantWriteACPHasBeenSet = true; m_grantWriteACP.assign(value); }
    inline CreateMultipartUploadRequest& WithGrantWriteACP(const Aws::String& value) { SetGrantWriteACP(value); return *this;}
    inline CreateMultipartUploadRequest& WithGrantWriteACP(Aws::String&& value) { SetGrantWriteACP(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithGrantWriteACP(const char* value) { SetGrantWriteACP(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Object key for which the multipart upload is to be initiated.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline CreateMultipartUploadRequest& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline CreateMultipartUploadRequest& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A map of metadata to store with the object in S3.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetMetadata() const{ return m_metadata; }
    inline bool MetadataHasBeenSet() const { return m_metadataHasBeenSet; }
    inline void SetMetadata(const Aws::Map<Aws::String, Aws::String>& value) { m_metadataHasBeenSet = true; m_metadata = value; }
    inline void SetMetadata(Aws::Map<Aws::String, Aws::String>&& value) { m_metadataHasBeenSet = true; m_metadata = std::move(value); }
    inline CreateMultipartUploadRequest& WithMetadata(const Aws::Map<Aws::String, Aws::String>& value) { SetMetadata(value); return *this;}
    inline CreateMultipartUploadRequest& WithMetadata(Aws::Map<Aws::String, Aws::String>&& value) { SetMetadata(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& AddMetadata(const Aws::String& key, const Aws::String& value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, value); return *this; }
    inline CreateMultipartUploadRequest& AddMetadata(Aws::String&& key, const Aws::String& value) { m_metadataHasBeenSet = true; m_metadata.emplace(std::move(key), value); return *this; }
    inline CreateMultipartUploadRequest& AddMetadata(const Aws::String& key, Aws::String&& value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, std::move(value)); return *this; }
    inline CreateMultipartUploadRequest& AddMetadata(Aws::String&& key, Aws::String&& value) { m_metadataHasBeenSet = true; m_metadata.emplace(std::move(key), std::move(value)); return *this; }
    inline CreateMultipartUploadRequest& AddMetadata(const char* key, Aws::String&& value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, std::move(value)); return *this; }
    inline CreateMultipartUploadRequest& AddMetadata(Aws::String&& key, const char* value) { m_metadataHasBeenSet = true; m_metadata.emplace(std::move(key), value); return *this; }
    inline CreateMultipartUploadRequest& AddMetadata(const char* key, const char* value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store this object in
     * Amazon S3 (for example, <code>AES256</code>, <code>aws:kms</code>).</p> <ul>
     * <li> <p> <b>Directory buckets </b> - For directory buckets, there are only two
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
     * server-side encryption with KMS for new object uploads</a>. </p> <p>In the Zonal
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
     * of the directory bucket. </p>  </li> </ul>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline bool ServerSideEncryptionHasBeenSet() const { return m_serverSideEncryptionHasBeenSet; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryptionHasBeenSet = true; m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryptionHasBeenSet = true; m_serverSideEncryption = std::move(value); }
    inline CreateMultipartUploadRequest& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline CreateMultipartUploadRequest& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>By default, Amazon S3 uses the STANDARD Storage Class to store newly created
     * objects. The STANDARD storage class provides high durability and high
     * availability. Depending on performance needs, you can specify a different
     * Storage Class. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html">Storage
     * Classes</a> in the <i>Amazon S3 User Guide</i>.</p>  <ul> <li> <p>For
     * directory buckets, only the S3 Express One Zone storage class is supported to
     * store newly created objects.</p> </li> <li> <p>Amazon S3 on Outposts only uses
     * the OUTPOSTS Storage Class.</p> </li> </ul> 
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline CreateMultipartUploadRequest& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline CreateMultipartUploadRequest& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If the bucket is configured as a website, redirects requests for this object
     * to another object in the same bucket or to an external URL. Amazon S3 stores the
     * value of this header in the object metadata.</p>  <p>This functionality is
     * not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetWebsiteRedirectLocation() const{ return m_websiteRedirectLocation; }
    inline bool WebsiteRedirectLocationHasBeenSet() const { return m_websiteRedirectLocationHasBeenSet; }
    inline void SetWebsiteRedirectLocation(const Aws::String& value) { m_websiteRedirectLocationHasBeenSet = true; m_websiteRedirectLocation = value; }
    inline void SetWebsiteRedirectLocation(Aws::String&& value) { m_websiteRedirectLocationHasBeenSet = true; m_websiteRedirectLocation = std::move(value); }
    inline void SetWebsiteRedirectLocation(const char* value) { m_websiteRedirectLocationHasBeenSet = true; m_websiteRedirectLocation.assign(value); }
    inline CreateMultipartUploadRequest& WithWebsiteRedirectLocation(const Aws::String& value) { SetWebsiteRedirectLocation(value); return *this;}
    inline CreateMultipartUploadRequest& WithWebsiteRedirectLocation(Aws::String&& value) { SetWebsiteRedirectLocation(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithWebsiteRedirectLocation(const char* value) { SetWebsiteRedirectLocation(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the algorithm to use when encrypting the object (for example,
     * AES256).</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerAlgorithm() const{ return m_sSECustomerAlgorithm; }
    inline bool SSECustomerAlgorithmHasBeenSet() const { return m_sSECustomerAlgorithmHasBeenSet; }
    inline void SetSSECustomerAlgorithm(const Aws::String& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = value; }
    inline void SetSSECustomerAlgorithm(Aws::String&& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = std::move(value); }
    inline void SetSSECustomerAlgorithm(const char* value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm.assign(value); }
    inline CreateMultipartUploadRequest& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline CreateMultipartUploadRequest& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the customer-provided encryption key for Amazon S3 to use in
     * encrypting data. This value is used to store the object and then it is
     * discarded; Amazon S3 does not store the encryption key. The key must be
     * appropriate for use with the algorithm specified in the
     * <code>x-amz-server-side-encryption-customer-algorithm</code> header.</p> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKey() const{ return m_sSECustomerKey; }
    inline bool SSECustomerKeyHasBeenSet() const { return m_sSECustomerKeyHasBeenSet; }
    inline void SetSSECustomerKey(const Aws::String& value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey = value; }
    inline void SetSSECustomerKey(Aws::String&& value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey = std::move(value); }
    inline void SetSSECustomerKey(const char* value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey.assign(value); }
    inline CreateMultipartUploadRequest& WithSSECustomerKey(const Aws::String& value) { SetSSECustomerKey(value); return *this;}
    inline CreateMultipartUploadRequest& WithSSECustomerKey(Aws::String&& value) { SetSSECustomerKey(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithSSECustomerKey(const char* value) { SetSSECustomerKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the 128-bit MD5 digest of the customer-provided encryption key
     * according to RFC 1321. Amazon S3 uses this header for a message integrity check
     * to ensure that the encryption key was transmitted without error.</p> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKeyMD5() const{ return m_sSECustomerKeyMD5; }
    inline bool SSECustomerKeyMD5HasBeenSet() const { return m_sSECustomerKeyMD5HasBeenSet; }
    inline void SetSSECustomerKeyMD5(const Aws::String& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = value; }
    inline void SetSSECustomerKeyMD5(Aws::String&& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = std::move(value); }
    inline void SetSSECustomerKeyMD5(const char* value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5.assign(value); }
    inline CreateMultipartUploadRequest& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline CreateMultipartUploadRequest& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the KMS key ID (Key ID, Key ARN, or Key Alias) to use for object
     * encryption. If the KMS key doesn't exist in the same account that's issuing the
     * command, you must use the full Key ARN not the Key ID.</p> <p> <b>General
     * purpose buckets</b> - If you specify <code>x-amz-server-side-encryption</code>
     * with <code>aws:kms</code> or <code>aws:kms:dsse</code>, this header specifies
     * the ID (Key ID, Key ARN, or Key Alias) of the KMS key to use. If you specify
     * <code>x-amz-server-side-encryption:aws:kms</code> or
     * <code>x-amz-server-side-encryption:aws:kms:dsse</code>, but do not provide
     * <code>x-amz-server-side-encryption-aws-kms-key-id</code>, Amazon S3 uses the
     * Amazon Web Services managed key (<code>aws/s3</code>) to protect the data.</p>
     * <p> <b>Directory buckets</b> - If you specify
     * <code>x-amz-server-side-encryption</code> with <code>aws:kms</code>, the <code>
     * x-amz-server-side-encryption-aws-kms-key-id</code> header is implicitly assigned
     * the ID of the KMS symmetric encryption customer managed key that's configured
     * for your directory bucket's default encryption setting. If you want to specify
     * the <code> x-amz-server-side-encryption-aws-kms-key-id</code> header explicitly,
     * you can only specify it with the ID (Key ID or Key ARN) of the KMS customer
     * managed key that's configured for your directory bucket's default encryption
     * setting. Otherwise, you get an HTTP <code>400 Bad Request</code> error. Only use
     * the key ID or key ARN. The key alias format of the KMS key isn't supported. Your
     * SSE-KMS configuration can only support 1 <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#customer-cmk">customer
     * managed key</a> per directory bucket for the lifetime of the bucket. The <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-managed-cmk">Amazon
     * Web Services managed key</a> (<code>aws/s3</code>) isn't supported. </p>
     */
    inline const Aws::String& GetSSEKMSKeyId() const{ return m_sSEKMSKeyId; }
    inline bool SSEKMSKeyIdHasBeenSet() const { return m_sSEKMSKeyIdHasBeenSet; }
    inline void SetSSEKMSKeyId(const Aws::String& value) { m_sSEKMSKeyIdHasBeenSet = true; m_sSEKMSKeyId = value; }
    inline void SetSSEKMSKeyId(Aws::String&& value) { m_sSEKMSKeyIdHasBeenSet = true; m_sSEKMSKeyId = std::move(value); }
    inline void SetSSEKMSKeyId(const char* value) { m_sSEKMSKeyIdHasBeenSet = true; m_sSEKMSKeyId.assign(value); }
    inline CreateMultipartUploadRequest& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline CreateMultipartUploadRequest& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the Amazon Web Services KMS Encryption Context to use for object
     * encryption. The value of this header is a Base64-encoded string of a UTF-8
     * encoded JSON, which contains the encryption context as key-value pairs.</p> <p>
     * <b>Directory buckets</b> - You can optionally provide an explicit encryption
     * context value. The value must match the default encryption context - the bucket
     * Amazon Resource Name (ARN). An additional encryption context value is not
     * supported. </p>
     */
    inline const Aws::String& GetSSEKMSEncryptionContext() const{ return m_sSEKMSEncryptionContext; }
    inline bool SSEKMSEncryptionContextHasBeenSet() const { return m_sSEKMSEncryptionContextHasBeenSet; }
    inline void SetSSEKMSEncryptionContext(const Aws::String& value) { m_sSEKMSEncryptionContextHasBeenSet = true; m_sSEKMSEncryptionContext = value; }
    inline void SetSSEKMSEncryptionContext(Aws::String&& value) { m_sSEKMSEncryptionContextHasBeenSet = true; m_sSEKMSEncryptionContext = std::move(value); }
    inline void SetSSEKMSEncryptionContext(const char* value) { m_sSEKMSEncryptionContextHasBeenSet = true; m_sSEKMSEncryptionContext.assign(value); }
    inline CreateMultipartUploadRequest& WithSSEKMSEncryptionContext(const Aws::String& value) { SetSSEKMSEncryptionContext(value); return *this;}
    inline CreateMultipartUploadRequest& WithSSEKMSEncryptionContext(Aws::String&& value) { SetSSEKMSEncryptionContext(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithSSEKMSEncryptionContext(const char* value) { SetSSEKMSEncryptionContext(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether Amazon S3 should use an S3 Bucket Key for object encryption
     * with server-side encryption using Key Management Service (KMS) keys
     * (SSE-KMS).</p> <p> <b>General purpose buckets</b> - Setting this header to
     * <code>true</code> causes Amazon S3 to use an S3 Bucket Key for object encryption
     * with SSE-KMS. Also, specifying this header with a PUT action doesn't affect
     * bucket-level settings for S3 Bucket Key.</p> <p> <b>Directory buckets</b> - S3
     * Bucket Keys are always enabled for <code>GET</code> and <code>PUT</code>
     * operations in a directory bucket and can’t be disabled. S3 Bucket Keys aren't
     * supported, when you copy SSE-KMS encrypted objects from general purpose buckets
     * to directory buckets, from directory buckets to general purpose buckets, or
     * between directory buckets, through <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html">CopyObject</a>,
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html">UploadPartCopy</a>,
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-buckets-objects-Batch-Ops">the
     * Copy operation in Batch Operations</a>, or <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/create-import-job">the
     * import jobs</a>. In this case, Amazon S3 makes a call to KMS every time a copy
     * request is made for a KMS-encrypted object.</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline bool BucketKeyEnabledHasBeenSet() const { return m_bucketKeyEnabledHasBeenSet; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabledHasBeenSet = true; m_bucketKeyEnabled = value; }
    inline CreateMultipartUploadRequest& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline CreateMultipartUploadRequest& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline CreateMultipartUploadRequest& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The tag-set for the object. The tag-set must be encoded as URL Query
     * parameters.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetTagging() const{ return m_tagging; }
    inline bool TaggingHasBeenSet() const { return m_taggingHasBeenSet; }
    inline void SetTagging(const Aws::String& value) { m_taggingHasBeenSet = true; m_tagging = value; }
    inline void SetTagging(Aws::String&& value) { m_taggingHasBeenSet = true; m_tagging = std::move(value); }
    inline void SetTagging(const char* value) { m_taggingHasBeenSet = true; m_tagging.assign(value); }
    inline CreateMultipartUploadRequest& WithTagging(const Aws::String& value) { SetTagging(value); return *this;}
    inline CreateMultipartUploadRequest& WithTagging(Aws::String&& value) { SetTagging(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithTagging(const char* value) { SetTagging(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the Object Lock mode that you want to apply to the uploaded
     * object.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const ObjectLockMode& GetObjectLockMode() const{ return m_objectLockMode; }
    inline bool ObjectLockModeHasBeenSet() const { return m_objectLockModeHasBeenSet; }
    inline void SetObjectLockMode(const ObjectLockMode& value) { m_objectLockModeHasBeenSet = true; m_objectLockMode = value; }
    inline void SetObjectLockMode(ObjectLockMode&& value) { m_objectLockModeHasBeenSet = true; m_objectLockMode = std::move(value); }
    inline CreateMultipartUploadRequest& WithObjectLockMode(const ObjectLockMode& value) { SetObjectLockMode(value); return *this;}
    inline CreateMultipartUploadRequest& WithObjectLockMode(ObjectLockMode&& value) { SetObjectLockMode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the date and time when you want the Object Lock to expire.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::Utils::DateTime& GetObjectLockRetainUntilDate() const{ return m_objectLockRetainUntilDate; }
    inline bool ObjectLockRetainUntilDateHasBeenSet() const { return m_objectLockRetainUntilDateHasBeenSet; }
    inline void SetObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { m_objectLockRetainUntilDateHasBeenSet = true; m_objectLockRetainUntilDate = value; }
    inline void SetObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { m_objectLockRetainUntilDateHasBeenSet = true; m_objectLockRetainUntilDate = std::move(value); }
    inline CreateMultipartUploadRequest& WithObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { SetObjectLockRetainUntilDate(value); return *this;}
    inline CreateMultipartUploadRequest& WithObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { SetObjectLockRetainUntilDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether you want to apply a legal hold to the uploaded object.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const ObjectLockLegalHoldStatus& GetObjectLockLegalHoldStatus() const{ return m_objectLockLegalHoldStatus; }
    inline bool ObjectLockLegalHoldStatusHasBeenSet() const { return m_objectLockLegalHoldStatusHasBeenSet; }
    inline void SetObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { m_objectLockLegalHoldStatusHasBeenSet = true; m_objectLockLegalHoldStatus = value; }
    inline void SetObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { m_objectLockLegalHoldStatusHasBeenSet = true; m_objectLockLegalHoldStatus = std::move(value); }
    inline CreateMultipartUploadRequest& WithObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { SetObjectLockLegalHoldStatus(value); return *this;}
    inline CreateMultipartUploadRequest& WithObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { SetObjectLockLegalHoldStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The account ID of the expected bucket owner. If the account ID that you
     * provide does not match the actual owner of the bucket, the request fails with
     * the HTTP status code <code>403 Forbidden</code> (access denied).</p>
     */
    inline const Aws::String& GetExpectedBucketOwner() const{ return m_expectedBucketOwner; }
    inline bool ExpectedBucketOwnerHasBeenSet() const { return m_expectedBucketOwnerHasBeenSet; }
    inline void SetExpectedBucketOwner(const Aws::String& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = value; }
    inline void SetExpectedBucketOwner(Aws::String&& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = std::move(value); }
    inline void SetExpectedBucketOwner(const char* value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner.assign(value); }
    inline CreateMultipartUploadRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline CreateMultipartUploadRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates the algorithm that you want Amazon S3 to use to create the checksum
     * for the object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const ChecksumAlgorithm& GetChecksumAlgorithm() const{ return m_checksumAlgorithm; }
    inline bool ChecksumAlgorithmHasBeenSet() const { return m_checksumAlgorithmHasBeenSet; }
    inline void SetChecksumAlgorithm(const ChecksumAlgorithm& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = value; }
    inline void SetChecksumAlgorithm(ChecksumAlgorithm&& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = std::move(value); }
    inline CreateMultipartUploadRequest& WithChecksumAlgorithm(const ChecksumAlgorithm& value) { SetChecksumAlgorithm(value); return *this;}
    inline CreateMultipartUploadRequest& WithChecksumAlgorithm(ChecksumAlgorithm&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline CreateMultipartUploadRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline CreateMultipartUploadRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline CreateMultipartUploadRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    ObjectCannedACL m_aCL;
    bool m_aCLHasBeenSet = false;

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_cacheControl;
    bool m_cacheControlHasBeenSet = false;

    Aws::String m_contentDisposition;
    bool m_contentDispositionHasBeenSet = false;

    Aws::String m_contentEncoding;
    bool m_contentEncodingHasBeenSet = false;

    Aws::String m_contentLanguage;
    bool m_contentLanguageHasBeenSet = false;

    Aws::String m_contentType;
    bool m_contentTypeHasBeenSet = false;

    Aws::Utils::DateTime m_expires;
    bool m_expiresHasBeenSet = false;

    Aws::String m_grantFullControl;
    bool m_grantFullControlHasBeenSet = false;

    Aws::String m_grantRead;
    bool m_grantReadHasBeenSet = false;

    Aws::String m_grantReadACP;
    bool m_grantReadACPHasBeenSet = false;

    Aws::String m_grantWriteACP;
    bool m_grantWriteACPHasBeenSet = false;

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_metadata;
    bool m_metadataHasBeenSet = false;

    ServerSideEncryption m_serverSideEncryption;
    bool m_serverSideEncryptionHasBeenSet = false;

    StorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;

    Aws::String m_websiteRedirectLocation;
    bool m_websiteRedirectLocationHasBeenSet = false;

    Aws::String m_sSECustomerAlgorithm;
    bool m_sSECustomerAlgorithmHasBeenSet = false;

    Aws::String m_sSECustomerKey;
    bool m_sSECustomerKeyHasBeenSet = false;

    Aws::String m_sSECustomerKeyMD5;
    bool m_sSECustomerKeyMD5HasBeenSet = false;

    Aws::String m_sSEKMSKeyId;
    bool m_sSEKMSKeyIdHasBeenSet = false;

    Aws::String m_sSEKMSEncryptionContext;
    bool m_sSEKMSEncryptionContextHasBeenSet = false;

    bool m_bucketKeyEnabled;
    bool m_bucketKeyEnabledHasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    Aws::String m_tagging;
    bool m_taggingHasBeenSet = false;

    ObjectLockMode m_objectLockMode;
    bool m_objectLockModeHasBeenSet = false;

    Aws::Utils::DateTime m_objectLockRetainUntilDate;
    bool m_objectLockRetainUntilDateHasBeenSet = false;

    ObjectLockLegalHoldStatus m_objectLockLegalHoldStatus;
    bool m_objectLockLegalHoldStatusHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    ChecksumAlgorithm m_checksumAlgorithm;
    bool m_checksumAlgorithmHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
