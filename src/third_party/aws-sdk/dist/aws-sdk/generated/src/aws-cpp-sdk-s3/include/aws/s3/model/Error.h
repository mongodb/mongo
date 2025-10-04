﻿/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>Container for all error elements.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Error">AWS API
   * Reference</a></p>
   */
  class Error
  {
  public:
    AWS_S3_API Error();
    AWS_S3_API Error(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Error& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The error key.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline Error& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline Error& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline Error& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The version ID of the error.</p>  <p>This functionality is not
     * supported for directory buckets.</p> 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline Error& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline Error& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline Error& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error code is a string that uniquely identifies an error condition. It is
     * meant to be read and understood by programs that detect and handle errors by
     * type. The following is a list of Amazon S3 error codes. For more information,
     * see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html">Error
     * responses</a>.</p> <ul> <li> <ul> <li> <p> <i>Code:</i> AccessDenied </p> </li>
     * <li> <p> <i>Description:</i> Access Denied</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 403 Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> AccountProblem</p>
     * </li> <li> <p> <i>Description:</i> There is a problem with your Amazon Web
     * Services account that prevents the action from completing successfully. Contact
     * Amazon Web Services Support for further assistance.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 403 Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * AllAccessDisabled</p> </li> <li> <p> <i>Description:</i> All access to this
     * Amazon S3 resource has been disabled. Contact Amazon Web Services Support for
     * further assistance.</p> </li> <li> <p> <i>HTTP Status Code:</i> 403
     * Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> AmbiguousGrantByEmailAddress</p>
     * </li> <li> <p> <i>Description:</i> The email address you provided is associated
     * with more than one account.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> AuthorizationHeaderMalformed</p> </li>
     * <li> <p> <i>Description:</i> The authorization header you provided is
     * invalid.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li>
     * <li> <p> <i>HTTP Status Code:</i> N/A</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> BadDigest</p> </li> <li> <p> <i>Description:</i> The Content-MD5
     * you specified did not match what we received.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * BucketAlreadyExists</p> </li> <li> <p> <i>Description:</i> The requested bucket
     * name is not available. The bucket namespace is shared by all users of the
     * system. Please select a different name and try again.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 409 Conflict</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * BucketAlreadyOwnedByYou</p> </li> <li> <p> <i>Description:</i> The bucket you
     * tried to create already exists, and you own it. Amazon S3 returns this error in
     * all Amazon Web Services Regions except in the North Virginia Region. For legacy
     * compatibility, if you re-create an existing bucket that you already own in the
     * North Virginia Region, Amazon S3 returns 200 OK and resets the bucket access
     * control lists (ACLs).</p> </li> <li> <p> <i>Code:</i> 409 Conflict (in all
     * Regions except the North Virginia Region) </p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * BucketNotEmpty</p> </li> <li> <p> <i>Description:</i> The bucket you tried to
     * delete is not empty.</p> </li> <li> <p> <i>HTTP Status Code:</i> 409
     * Conflict</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> CredentialsNotSupported</p> </li>
     * <li> <p> <i>Description:</i> This request does not support credentials.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p>
     * <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> CrossLocationLoggingProhibited</p> </li> <li> <p>
     * <i>Description:</i> Cross-location logging not allowed. Buckets in one
     * geographic location cannot log information to a bucket in another location.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 403 Forbidden</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> EntityTooSmall</p> </li> <li> <p> <i>Description:</i> Your proposed
     * upload is smaller than the minimum allowed object size.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * EntityTooLarge</p> </li> <li> <p> <i>Description:</i> Your proposed upload
     * exceeds the maximum allowed object size.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> ExpiredToken</p>
     * </li> <li> <p> <i>Description:</i> The provided token has expired.</p> </li>
     * <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> IllegalVersioningConfigurationException </p> </li> <li> <p>
     * <i>Description:</i> Indicates that the versioning configuration specified in the
     * request is invalid.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> IncompleteBody</p> </li> <li> <p>
     * <i>Description:</i> You did not provide the number of bytes specified by the
     * Content-Length HTTP header</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> IncorrectNumberOfFilesInPostRequest</p>
     * </li> <li> <p> <i>Description:</i> POST requires exactly one file upload per
     * request.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li>
     * <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul>
     * <li> <p> <i>Code:</i> InlineDataTooLarge</p> </li> <li> <p> <i>Description:</i>
     * Inline data exceeds the maximum allowed size.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InternalError</p>
     * </li> <li> <p> <i>Description:</i> We encountered an internal error. Please try
     * again.</p> </li> <li> <p> <i>HTTP Status Code:</i> 500 Internal Server Error</p>
     * </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Server</p> </li> </ul> </li> <li>
     * <ul> <li> <p> <i>Code:</i> InvalidAccessKeyId</p> </li> <li> <p>
     * <i>Description:</i> The Amazon Web Services access key ID you provided does not
     * exist in our records.</p> </li> <li> <p> <i>HTTP Status Code:</i> 403
     * Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidAddressingHeader</p> </li>
     * <li> <p> <i>Description:</i> You must specify the Anonymous role.</p> </li> <li>
     * <p> <i>HTTP Status Code:</i> N/A</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidArgument</p> </li> <li> <p> <i>Description:</i> Invalid Argument</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p>
     * <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> InvalidBucketName</p> </li> <li> <p> <i>Description:</i> The
     * specified bucket is not valid.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400
     * Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidBucketState</p> </li> <li>
     * <p> <i>Description:</i> The request is not valid with the current state of the
     * bucket.</p> </li> <li> <p> <i>HTTP Status Code:</i> 409 Conflict</p> </li> <li>
     * <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li>
     * <p> <i>Code:</i> InvalidDigest</p> </li> <li> <p> <i>Description:</i> The
     * Content-MD5 you specified is not valid.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidEncryptionAlgorithmError</p> </li> <li> <p> <i>Description:</i> The
     * encryption request you specified is not valid. The valid value is AES256.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p>
     * <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> InvalidLocationConstraint</p> </li> <li> <p> <i>Description:</i>
     * The specified location constraint is not valid. For more information about
     * Regions, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/UsingBucket.html#access-bucket-intro">How
     * to Select a Region for Your Buckets</a>. </p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidObjectState</p> </li> <li> <p> <i>Description:</i> The action is not
     * valid for the current state of the object.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 403 Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidPart</p>
     * </li> <li> <p> <i>Description:</i> One or more of the specified parts could not
     * be found. The part might not have been uploaded, or the specified entity tag
     * might not have matched the part's entity tag.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidPartOrder</p> </li> <li> <p> <i>Description:</i> The list of parts was
     * not in ascending order. Parts list must be specified in order by part
     * number.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li>
     * <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul>
     * <li> <p> <i>Code:</i> InvalidPayer</p> </li> <li> <p> <i>Description:</i> All
     * access to this object has been disabled. Please contact Amazon Web Services
     * Support for further assistance.</p> </li> <li> <p> <i>HTTP Status Code:</i> 403
     * Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidPolicyDocument</p> </li> <li>
     * <p> <i>Description:</i> The content of the form does not meet the conditions
     * specified in the policy document.</p> </li> <li> <p> <i>HTTP Status Code:</i>
     * 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p>
     * </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidRange</p> </li> <li>
     * <p> <i>Description:</i> The requested range cannot be satisfied.</p> </li> <li>
     * <p> <i>HTTP Status Code:</i> 416 Requested Range Not Satisfiable</p> </li> <li>
     * <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li>
     * <p> <i>Code:</i> InvalidRequest</p> </li> <li> <p> <i>Description:</i> Please
     * use <code>AWS4-HMAC-SHA256</code>.</p> </li> <li> <p> <i>HTTP Status Code:</i>
     * 400 Bad Request</p> </li> <li> <p> <i>Code:</i> N/A</p> </li> </ul> </li> <li>
     * <ul> <li> <p> <i>Code:</i> InvalidRequest</p> </li> <li> <p> <i>Description:</i>
     * SOAP requests must be made over an HTTPS connection.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidRequest</p> </li> <li> <p> <i>Description:</i> Amazon S3 Transfer
     * Acceleration is not supported for buckets with non-DNS compliant names.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p>
     * <i>Code:</i> N/A</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidRequest</p> </li> <li> <p> <i>Description:</i> Amazon S3 Transfer
     * Acceleration is not supported for buckets with periods (.) in their names.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p>
     * <i>Code:</i> N/A</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidRequest</p> </li> <li> <p> <i>Description:</i> Amazon S3 Transfer
     * Accelerate endpoint only supports virtual style requests.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>Code:</i> N/A</p>
     * </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidRequest</p> </li> <li>
     * <p> <i>Description:</i> Amazon S3 Transfer Accelerate is not configured on this
     * bucket.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li>
     * <li> <p> <i>Code:</i> N/A</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidRequest</p> </li> <li> <p> <i>Description:</i> Amazon S3 Transfer
     * Accelerate is disabled on this bucket.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>Code:</i> N/A</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> InvalidRequest</p> </li> <li> <p>
     * <i>Description:</i> Amazon S3 Transfer Acceleration is not supported on this
     * bucket. Contact Amazon Web Services Support for more information.</p> </li> <li>
     * <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>Code:</i>
     * N/A</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidRequest</p>
     * </li> <li> <p> <i>Description:</i> Amazon S3 Transfer Acceleration cannot be
     * enabled on this bucket. Contact Amazon Web Services Support for more
     * information.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p>
     * </li> <li> <p> <i>Code:</i> N/A</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> InvalidSecurity</p> </li> <li> <p> <i>Description:</i> The provided
     * security credentials are not valid.</p> </li> <li> <p> <i>HTTP Status Code:</i>
     * 403 Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> InvalidSOAPRequest</p> </li> <li>
     * <p> <i>Description:</i> The SOAP request body is invalid.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidStorageClass</p> </li> <li> <p> <i>Description:</i> The storage class you
     * specified is not valid.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> InvalidTargetBucketForLogging</p> </li>
     * <li> <p> <i>Description:</i> The target bucket for logging does not exist, is
     * not owned by you, or does not have the appropriate grants for the log-delivery
     * group. </p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li>
     * <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul>
     * <li> <p> <i>Code:</i> InvalidToken</p> </li> <li> <p> <i>Description:</i> The
     * provided token is malformed or otherwise invalid.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * InvalidURI</p> </li> <li> <p> <i>Description:</i> Couldn't parse the specified
     * URI.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li>
     * <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li>
     * <p> <i>Code:</i> KeyTooLongError</p> </li> <li> <p> <i>Description:</i> Your key
     * is too long.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p>
     * </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li>
     * <ul> <li> <p> <i>Code:</i> MalformedACLError</p> </li> <li> <p>
     * <i>Description:</i> The XML you provided was not well-formed or did not validate
     * against our published schema.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400
     * Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> MalformedPOSTRequest </p> </li> <li>
     * <p> <i>Description:</i> The body of your POST request is not well-formed
     * multipart/form-data.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> MalformedXML</p> </li> <li> <p>
     * <i>Description:</i> This happens when the user sends malformed XML (XML that
     * doesn't conform to the published XSD) for the configuration. The error message
     * is, "The XML you provided was not well-formed or did not validate against our
     * published schema." </p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> MaxMessageLengthExceeded</p> </li> <li>
     * <p> <i>Description:</i> Your request was too big.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * MaxPostPreDataLengthExceededError</p> </li> <li> <p> <i>Description:</i> Your
     * POST request fields preceding the upload file were too large.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * MetadataTooLarge</p> </li> <li> <p> <i>Description:</i> Your metadata headers
     * exceed the maximum allowed metadata size.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * MethodNotAllowed</p> </li> <li> <p> <i>Description:</i> The specified method is
     * not allowed against this resource.</p> </li> <li> <p> <i>HTTP Status Code:</i>
     * 405 Method Not Allowed</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * MissingAttachment</p> </li> <li> <p> <i>Description:</i> A SOAP attachment was
     * expected, but none were found.</p> </li> <li> <p> <i>HTTP Status Code:</i>
     * N/A</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> MissingContentLength</p> </li> <li> <p>
     * <i>Description:</i> You must provide the Content-Length HTTP header.</p> </li>
     * <li> <p> <i>HTTP Status Code:</i> 411 Length Required</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> MissingRequestBodyError</p> </li> <li> <p> <i>Description:</i> This
     * happens when the user sends an empty XML document as a request. The error
     * message is, "Request body is empty." </p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * MissingSecurityElement</p> </li> <li> <p> <i>Description:</i> The SOAP 1.1
     * request is missing a security element.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * MissingSecurityHeader</p> </li> <li> <p> <i>Description:</i> Your request is
     * missing a required header.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> NoLoggingStatusForKey</p> </li> <li> <p>
     * <i>Description:</i> There is no such thing as a logging status subresource for a
     * key.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li>
     * <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li>
     * <p> <i>Code:</i> NoSuchBucket</p> </li> <li> <p> <i>Description:</i> The
     * specified bucket does not exist.</p> </li> <li> <p> <i>HTTP Status Code:</i> 404
     * Not Found</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> NoSuchBucketPolicy</p> </li> <li>
     * <p> <i>Description:</i> The specified bucket does not have a bucket policy.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 404 Not Found</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> NoSuchKey</p> </li> <li> <p> <i>Description:</i> The specified key
     * does not exist.</p> </li> <li> <p> <i>HTTP Status Code:</i> 404 Not Found</p>
     * </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li>
     * <ul> <li> <p> <i>Code:</i> NoSuchLifecycleConfiguration</p> </li> <li> <p>
     * <i>Description:</i> The lifecycle configuration does not exist. </p> </li> <li>
     * <p> <i>HTTP Status Code:</i> 404 Not Found</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * NoSuchUpload</p> </li> <li> <p> <i>Description:</i> The specified multipart
     * upload does not exist. The upload ID might be invalid, or the multipart upload
     * might have been aborted or completed.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 404 Not Found</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> NoSuchVersion </p>
     * </li> <li> <p> <i>Description:</i> Indicates that the version ID specified in
     * the request does not match an existing version.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 404 Not Found</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i> NotImplemented</p>
     * </li> <li> <p> <i>Description:</i> A header you provided implies functionality
     * that is not implemented.</p> </li> <li> <p> <i>HTTP Status Code:</i> 501 Not
     * Implemented</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Server</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> NotSignedUp</p> </li> <li> <p>
     * <i>Description:</i> Your account is not signed up for the Amazon S3 service. You
     * must sign up before you can use Amazon S3. You can sign up at the following URL:
     * <a href="http://aws.amazon.com/s3">Amazon S3</a> </p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 403 Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * OperationAborted</p> </li> <li> <p> <i>Description:</i> A conflicting
     * conditional action is currently in progress against this resource. Try
     * again.</p> </li> <li> <p> <i>HTTP Status Code:</i> 409 Conflict</p> </li> <li>
     * <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li>
     * <p> <i>Code:</i> PermanentRedirect</p> </li> <li> <p> <i>Description:</i> The
     * bucket you are attempting to access must be addressed using the specified
     * endpoint. Send all future requests to this endpoint.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 301 Moved Permanently</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * PreconditionFailed</p> </li> <li> <p> <i>Description:</i> At least one of the
     * preconditions you specified did not hold.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 412 Precondition Failed</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * Redirect</p> </li> <li> <p> <i>Description:</i> Temporary redirect.</p> </li>
     * <li> <p> <i>HTTP Status Code:</i> 307 Moved Temporarily</p> </li> <li> <p>
     * <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> RestoreAlreadyInProgress</p> </li> <li> <p> <i>Description:</i>
     * Object restore is already in progress.</p> </li> <li> <p> <i>HTTP Status
     * Code:</i> 409 Conflict</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i>
     * Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * RequestIsNotMultiPartContent</p> </li> <li> <p> <i>Description:</i> Bucket POST
     * must be of the enclosure-type multipart/form-data.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * RequestTimeout</p> </li> <li> <p> <i>Description:</i> Your socket connection to
     * the server was not read from or written to within the timeout period.</p> </li>
     * <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> RequestTimeTooSkewed</p> </li> <li> <p> <i>Description:</i> The
     * difference between the request time and the server's time is too large.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 403 Forbidden</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> RequestTorrentOfBucketError</p> </li> <li> <p> <i>Description:</i>
     * Requesting the torrent file of a bucket is not permitted.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * SignatureDoesNotMatch</p> </li> <li> <p> <i>Description:</i> The request
     * signature we calculated does not match the signature you provided. Check your
     * Amazon Web Services secret access key and signing method. For more information,
     * see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/RESTAuthentication.html">REST
     * Authentication</a> and <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/SOAPAuthentication.html">SOAP
     * Authentication</a> for details.</p> </li> <li> <p> <i>HTTP Status Code:</i> 403
     * Forbidden</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li>
     * </ul> </li> <li> <ul> <li> <p> <i>Code:</i> ServiceUnavailable</p> </li> <li>
     * <p> <i>Description:</i> Service is unable to handle request.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 503 Service Unavailable</p> </li> <li> <p> <i>SOAP
     * Fault Code Prefix:</i> Server</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> SlowDown</p> </li> <li> <p> <i>Description:</i> Reduce your request
     * rate.</p> </li> <li> <p> <i>HTTP Status Code:</i> 503 Slow Down</p> </li> <li>
     * <p> <i>SOAP Fault Code Prefix:</i> Server</p> </li> </ul> </li> <li> <ul> <li>
     * <p> <i>Code:</i> TemporaryRedirect</p> </li> <li> <p> <i>Description:</i> You
     * are being redirected to the bucket while DNS updates.</p> </li> <li> <p> <i>HTTP
     * Status Code:</i> 307 Moved Temporarily</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * TokenRefreshRequired</p> </li> <li> <p> <i>Description:</i> The provided token
     * must be refreshed.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad
     * Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul>
     * </li> <li> <ul> <li> <p> <i>Code:</i> TooManyBuckets</p> </li> <li> <p>
     * <i>Description:</i> You have attempted to create more buckets than allowed.</p>
     * </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p>
     * <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p>
     * <i>Code:</i> UnexpectedContent</p> </li> <li> <p> <i>Description:</i> This
     * request does not support content.</p> </li> <li> <p> <i>HTTP Status Code:</i>
     * 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p>
     * </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * UnresolvableGrantByEmailAddress</p> </li> <li> <p> <i>Description:</i> The email
     * address you provided does not match any account on record.</p> </li> <li> <p>
     * <i>HTTP Status Code:</i> 400 Bad Request</p> </li> <li> <p> <i>SOAP Fault Code
     * Prefix:</i> Client</p> </li> </ul> </li> <li> <ul> <li> <p> <i>Code:</i>
     * UserKeyMustBeSpecified</p> </li> <li> <p> <i>Description:</i> The bucket POST
     * must contain the specified field name. If it is specified, check the order of
     * the fields.</p> </li> <li> <p> <i>HTTP Status Code:</i> 400 Bad Request</p>
     * </li> <li> <p> <i>SOAP Fault Code Prefix:</i> Client</p> </li> </ul> </li> </ul>
     * <p/>
     */
    inline const Aws::String& GetCode() const{ return m_code; }
    inline bool CodeHasBeenSet() const { return m_codeHasBeenSet; }
    inline void SetCode(const Aws::String& value) { m_codeHasBeenSet = true; m_code = value; }
    inline void SetCode(Aws::String&& value) { m_codeHasBeenSet = true; m_code = std::move(value); }
    inline void SetCode(const char* value) { m_codeHasBeenSet = true; m_code.assign(value); }
    inline Error& WithCode(const Aws::String& value) { SetCode(value); return *this;}
    inline Error& WithCode(Aws::String&& value) { SetCode(std::move(value)); return *this;}
    inline Error& WithCode(const char* value) { SetCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error message contains a generic description of the error condition in
     * English. It is intended for a human audience. Simple programs display the
     * message directly to the end user if they encounter an error condition they don't
     * know how or don't care to handle. Sophisticated programs with more exhaustive
     * error handling and proper internationalization are more likely to ignore the
     * error message.</p>
     */
    inline const Aws::String& GetMessage() const{ return m_message; }
    inline bool MessageHasBeenSet() const { return m_messageHasBeenSet; }
    inline void SetMessage(const Aws::String& value) { m_messageHasBeenSet = true; m_message = value; }
    inline void SetMessage(Aws::String&& value) { m_messageHasBeenSet = true; m_message = std::move(value); }
    inline void SetMessage(const char* value) { m_messageHasBeenSet = true; m_message.assign(value); }
    inline Error& WithMessage(const Aws::String& value) { SetMessage(value); return *this;}
    inline Error& WithMessage(Aws::String&& value) { SetMessage(std::move(value)); return *this;}
    inline Error& WithMessage(const char* value) { SetMessage(value); return *this;}
    ///@}
  private:

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    Aws::String m_code;
    bool m_codeHasBeenSet = false;

    Aws::String m_message;
    bool m_messageHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
