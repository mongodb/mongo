/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
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
  class UpdateFunctionCodeRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API UpdateFunctionCodeRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateFunctionCode"; }

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
    inline UpdateFunctionCodeRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline UpdateFunctionCodeRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded contents of the deployment package. Amazon Web Services
     * SDK and CLI clients handle the encoding for you. Use only with a function
     * defined with a .zip file archive deployment package.</p>
     */
    inline const Aws::Utils::CryptoBuffer& GetZipFile() const{ return m_zipFile; }
    inline bool ZipFileHasBeenSet() const { return m_zipFileHasBeenSet; }
    inline void SetZipFile(const Aws::Utils::CryptoBuffer& value) { m_zipFileHasBeenSet = true; m_zipFile = value; }
    inline void SetZipFile(Aws::Utils::CryptoBuffer&& value) { m_zipFileHasBeenSet = true; m_zipFile = std::move(value); }
    inline UpdateFunctionCodeRequest& WithZipFile(const Aws::Utils::CryptoBuffer& value) { SetZipFile(value); return *this;}
    inline UpdateFunctionCodeRequest& WithZipFile(Aws::Utils::CryptoBuffer&& value) { SetZipFile(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>An Amazon S3 bucket in the same Amazon Web Services Region as your function.
     * The bucket can be in a different Amazon Web Services account. Use only with a
     * function defined with a .zip file archive deployment package.</p>
     */
    inline const Aws::String& GetS3Bucket() const{ return m_s3Bucket; }
    inline bool S3BucketHasBeenSet() const { return m_s3BucketHasBeenSet; }
    inline void SetS3Bucket(const Aws::String& value) { m_s3BucketHasBeenSet = true; m_s3Bucket = value; }
    inline void SetS3Bucket(Aws::String&& value) { m_s3BucketHasBeenSet = true; m_s3Bucket = std::move(value); }
    inline void SetS3Bucket(const char* value) { m_s3BucketHasBeenSet = true; m_s3Bucket.assign(value); }
    inline UpdateFunctionCodeRequest& WithS3Bucket(const Aws::String& value) { SetS3Bucket(value); return *this;}
    inline UpdateFunctionCodeRequest& WithS3Bucket(Aws::String&& value) { SetS3Bucket(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithS3Bucket(const char* value) { SetS3Bucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon S3 key of the deployment package. Use only with a function defined
     * with a .zip file archive deployment package.</p>
     */
    inline const Aws::String& GetS3Key() const{ return m_s3Key; }
    inline bool S3KeyHasBeenSet() const { return m_s3KeyHasBeenSet; }
    inline void SetS3Key(const Aws::String& value) { m_s3KeyHasBeenSet = true; m_s3Key = value; }
    inline void SetS3Key(Aws::String&& value) { m_s3KeyHasBeenSet = true; m_s3Key = std::move(value); }
    inline void SetS3Key(const char* value) { m_s3KeyHasBeenSet = true; m_s3Key.assign(value); }
    inline UpdateFunctionCodeRequest& WithS3Key(const Aws::String& value) { SetS3Key(value); return *this;}
    inline UpdateFunctionCodeRequest& WithS3Key(Aws::String&& value) { SetS3Key(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithS3Key(const char* value) { SetS3Key(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>For versioned objects, the version of the deployment package object to
     * use.</p>
     */
    inline const Aws::String& GetS3ObjectVersion() const{ return m_s3ObjectVersion; }
    inline bool S3ObjectVersionHasBeenSet() const { return m_s3ObjectVersionHasBeenSet; }
    inline void SetS3ObjectVersion(const Aws::String& value) { m_s3ObjectVersionHasBeenSet = true; m_s3ObjectVersion = value; }
    inline void SetS3ObjectVersion(Aws::String&& value) { m_s3ObjectVersionHasBeenSet = true; m_s3ObjectVersion = std::move(value); }
    inline void SetS3ObjectVersion(const char* value) { m_s3ObjectVersionHasBeenSet = true; m_s3ObjectVersion.assign(value); }
    inline UpdateFunctionCodeRequest& WithS3ObjectVersion(const Aws::String& value) { SetS3ObjectVersion(value); return *this;}
    inline UpdateFunctionCodeRequest& WithS3ObjectVersion(Aws::String&& value) { SetS3ObjectVersion(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithS3ObjectVersion(const char* value) { SetS3ObjectVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>URI of a container image in the Amazon ECR registry. Do not use for a
     * function defined with a .zip file archive.</p>
     */
    inline const Aws::String& GetImageUri() const{ return m_imageUri; }
    inline bool ImageUriHasBeenSet() const { return m_imageUriHasBeenSet; }
    inline void SetImageUri(const Aws::String& value) { m_imageUriHasBeenSet = true; m_imageUri = value; }
    inline void SetImageUri(Aws::String&& value) { m_imageUriHasBeenSet = true; m_imageUri = std::move(value); }
    inline void SetImageUri(const char* value) { m_imageUriHasBeenSet = true; m_imageUri.assign(value); }
    inline UpdateFunctionCodeRequest& WithImageUri(const Aws::String& value) { SetImageUri(value); return *this;}
    inline UpdateFunctionCodeRequest& WithImageUri(Aws::String&& value) { SetImageUri(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithImageUri(const char* value) { SetImageUri(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Set to true to publish a new version of the function after updating the code.
     * This has the same effect as calling <a>PublishVersion</a> separately.</p>
     */
    inline bool GetPublish() const{ return m_publish; }
    inline bool PublishHasBeenSet() const { return m_publishHasBeenSet; }
    inline void SetPublish(bool value) { m_publishHasBeenSet = true; m_publish = value; }
    inline UpdateFunctionCodeRequest& WithPublish(bool value) { SetPublish(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Set to true to validate the request parameters and access permissions without
     * modifying the function code.</p>
     */
    inline bool GetDryRun() const{ return m_dryRun; }
    inline bool DryRunHasBeenSet() const { return m_dryRunHasBeenSet; }
    inline void SetDryRun(bool value) { m_dryRunHasBeenSet = true; m_dryRun = value; }
    inline UpdateFunctionCodeRequest& WithDryRun(bool value) { SetDryRun(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Update the function only if the revision ID matches the ID that's specified.
     * Use this option to avoid modifying a function that has changed since you last
     * read it.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline bool RevisionIdHasBeenSet() const { return m_revisionIdHasBeenSet; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionIdHasBeenSet = true; m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionIdHasBeenSet = true; m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionIdHasBeenSet = true; m_revisionId.assign(value); }
    inline UpdateFunctionCodeRequest& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline UpdateFunctionCodeRequest& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
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
    inline UpdateFunctionCodeRequest& WithArchitectures(const Aws::Vector<Architecture>& value) { SetArchitectures(value); return *this;}
    inline UpdateFunctionCodeRequest& WithArchitectures(Aws::Vector<Architecture>&& value) { SetArchitectures(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& AddArchitectures(const Architecture& value) { m_architecturesHasBeenSet = true; m_architectures.push_back(value); return *this; }
    inline UpdateFunctionCodeRequest& AddArchitectures(Architecture&& value) { m_architecturesHasBeenSet = true; m_architectures.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The ARN of the Key Management Service (KMS) customer managed key that's used
     * to encrypt your function's .zip deployment package. If you don't provide a
     * customer managed key, Lambda uses an Amazon Web Services managed key.</p>
     */
    inline const Aws::String& GetSourceKMSKeyArn() const{ return m_sourceKMSKeyArn; }
    inline bool SourceKMSKeyArnHasBeenSet() const { return m_sourceKMSKeyArnHasBeenSet; }
    inline void SetSourceKMSKeyArn(const Aws::String& value) { m_sourceKMSKeyArnHasBeenSet = true; m_sourceKMSKeyArn = value; }
    inline void SetSourceKMSKeyArn(Aws::String&& value) { m_sourceKMSKeyArnHasBeenSet = true; m_sourceKMSKeyArn = std::move(value); }
    inline void SetSourceKMSKeyArn(const char* value) { m_sourceKMSKeyArnHasBeenSet = true; m_sourceKMSKeyArn.assign(value); }
    inline UpdateFunctionCodeRequest& WithSourceKMSKeyArn(const Aws::String& value) { SetSourceKMSKeyArn(value); return *this;}
    inline UpdateFunctionCodeRequest& WithSourceKMSKeyArn(Aws::String&& value) { SetSourceKMSKeyArn(std::move(value)); return *this;}
    inline UpdateFunctionCodeRequest& WithSourceKMSKeyArn(const char* value) { SetSourceKMSKeyArn(value); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::Utils::CryptoBuffer m_zipFile;
    bool m_zipFileHasBeenSet = false;

    Aws::String m_s3Bucket;
    bool m_s3BucketHasBeenSet = false;

    Aws::String m_s3Key;
    bool m_s3KeyHasBeenSet = false;

    Aws::String m_s3ObjectVersion;
    bool m_s3ObjectVersionHasBeenSet = false;

    Aws::String m_imageUri;
    bool m_imageUriHasBeenSet = false;

    bool m_publish;
    bool m_publishHasBeenSet = false;

    bool m_dryRun;
    bool m_dryRunHasBeenSet = false;

    Aws::String m_revisionId;
    bool m_revisionIdHasBeenSet = false;

    Aws::Vector<Architecture> m_architectures;
    bool m_architecturesHasBeenSet = false;

    Aws::String m_sourceKMSKeyArn;
    bool m_sourceKMSKeyArnHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
