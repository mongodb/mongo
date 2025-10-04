/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  /**
   * <p>The code for the Lambda function. You can either specify an object in Amazon
   * S3, upload a .zip file archive deployment package directly, or specify the URI
   * of a container image.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/FunctionCode">AWS
   * API Reference</a></p>
   */
  class FunctionCode
  {
  public:
    AWS_LAMBDA_API FunctionCode();
    AWS_LAMBDA_API FunctionCode(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API FunctionCode& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The base64-encoded contents of the deployment package. Amazon Web Services
     * SDK and CLI clients handle the encoding for you.</p>
     */
    inline const Aws::Utils::CryptoBuffer& GetZipFile() const{ return m_zipFile; }
    inline bool ZipFileHasBeenSet() const { return m_zipFileHasBeenSet; }
    inline void SetZipFile(const Aws::Utils::CryptoBuffer& value) { m_zipFileHasBeenSet = true; m_zipFile = value; }
    inline void SetZipFile(Aws::Utils::CryptoBuffer&& value) { m_zipFileHasBeenSet = true; m_zipFile = std::move(value); }
    inline FunctionCode& WithZipFile(const Aws::Utils::CryptoBuffer& value) { SetZipFile(value); return *this;}
    inline FunctionCode& WithZipFile(Aws::Utils::CryptoBuffer&& value) { SetZipFile(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>An Amazon S3 bucket in the same Amazon Web Services Region as your function.
     * The bucket can be in a different Amazon Web Services account.</p>
     */
    inline const Aws::String& GetS3Bucket() const{ return m_s3Bucket; }
    inline bool S3BucketHasBeenSet() const { return m_s3BucketHasBeenSet; }
    inline void SetS3Bucket(const Aws::String& value) { m_s3BucketHasBeenSet = true; m_s3Bucket = value; }
    inline void SetS3Bucket(Aws::String&& value) { m_s3BucketHasBeenSet = true; m_s3Bucket = std::move(value); }
    inline void SetS3Bucket(const char* value) { m_s3BucketHasBeenSet = true; m_s3Bucket.assign(value); }
    inline FunctionCode& WithS3Bucket(const Aws::String& value) { SetS3Bucket(value); return *this;}
    inline FunctionCode& WithS3Bucket(Aws::String&& value) { SetS3Bucket(std::move(value)); return *this;}
    inline FunctionCode& WithS3Bucket(const char* value) { SetS3Bucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon S3 key of the deployment package.</p>
     */
    inline const Aws::String& GetS3Key() const{ return m_s3Key; }
    inline bool S3KeyHasBeenSet() const { return m_s3KeyHasBeenSet; }
    inline void SetS3Key(const Aws::String& value) { m_s3KeyHasBeenSet = true; m_s3Key = value; }
    inline void SetS3Key(Aws::String&& value) { m_s3KeyHasBeenSet = true; m_s3Key = std::move(value); }
    inline void SetS3Key(const char* value) { m_s3KeyHasBeenSet = true; m_s3Key.assign(value); }
    inline FunctionCode& WithS3Key(const Aws::String& value) { SetS3Key(value); return *this;}
    inline FunctionCode& WithS3Key(Aws::String&& value) { SetS3Key(std::move(value)); return *this;}
    inline FunctionCode& WithS3Key(const char* value) { SetS3Key(value); return *this;}
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
    inline FunctionCode& WithS3ObjectVersion(const Aws::String& value) { SetS3ObjectVersion(value); return *this;}
    inline FunctionCode& WithS3ObjectVersion(Aws::String&& value) { SetS3ObjectVersion(std::move(value)); return *this;}
    inline FunctionCode& WithS3ObjectVersion(const char* value) { SetS3ObjectVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>URI of a <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-images.html">container
     * image</a> in the Amazon ECR registry.</p>
     */
    inline const Aws::String& GetImageUri() const{ return m_imageUri; }
    inline bool ImageUriHasBeenSet() const { return m_imageUriHasBeenSet; }
    inline void SetImageUri(const Aws::String& value) { m_imageUriHasBeenSet = true; m_imageUri = value; }
    inline void SetImageUri(Aws::String&& value) { m_imageUriHasBeenSet = true; m_imageUri = std::move(value); }
    inline void SetImageUri(const char* value) { m_imageUriHasBeenSet = true; m_imageUri.assign(value); }
    inline FunctionCode& WithImageUri(const Aws::String& value) { SetImageUri(value); return *this;}
    inline FunctionCode& WithImageUri(Aws::String&& value) { SetImageUri(std::move(value)); return *this;}
    inline FunctionCode& WithImageUri(const char* value) { SetImageUri(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the Key Management Service (KMS) customer managed key that's used
     * to encrypt your function's .zip deployment package. If you don't provide a
     * customer managed key, Lambda uses an <a
     * href="https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#aws-owned-cmk">Amazon
     * Web Services owned key</a>.</p>
     */
    inline const Aws::String& GetSourceKMSKeyArn() const{ return m_sourceKMSKeyArn; }
    inline bool SourceKMSKeyArnHasBeenSet() const { return m_sourceKMSKeyArnHasBeenSet; }
    inline void SetSourceKMSKeyArn(const Aws::String& value) { m_sourceKMSKeyArnHasBeenSet = true; m_sourceKMSKeyArn = value; }
    inline void SetSourceKMSKeyArn(Aws::String&& value) { m_sourceKMSKeyArnHasBeenSet = true; m_sourceKMSKeyArn = std::move(value); }
    inline void SetSourceKMSKeyArn(const char* value) { m_sourceKMSKeyArnHasBeenSet = true; m_sourceKMSKeyArn.assign(value); }
    inline FunctionCode& WithSourceKMSKeyArn(const Aws::String& value) { SetSourceKMSKeyArn(value); return *this;}
    inline FunctionCode& WithSourceKMSKeyArn(Aws::String&& value) { SetSourceKMSKeyArn(std::move(value)); return *this;}
    inline FunctionCode& WithSourceKMSKeyArn(const char* value) { SetSourceKMSKeyArn(value); return *this;}
    ///@}
  private:

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

    Aws::String m_sourceKMSKeyArn;
    bool m_sourceKMSKeyArnHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
