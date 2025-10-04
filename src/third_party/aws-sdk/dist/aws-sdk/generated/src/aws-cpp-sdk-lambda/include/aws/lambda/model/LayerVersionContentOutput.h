/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
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
   * <p>Details about a version of an <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-layers.html">Lambda
   * layer</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/LayerVersionContentOutput">AWS
   * API Reference</a></p>
   */
  class LayerVersionContentOutput
  {
  public:
    AWS_LAMBDA_API LayerVersionContentOutput();
    AWS_LAMBDA_API LayerVersionContentOutput(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API LayerVersionContentOutput& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>A link to the layer archive in Amazon S3 that is valid for 10 minutes.</p>
     */
    inline const Aws::String& GetLocation() const{ return m_location; }
    inline bool LocationHasBeenSet() const { return m_locationHasBeenSet; }
    inline void SetLocation(const Aws::String& value) { m_locationHasBeenSet = true; m_location = value; }
    inline void SetLocation(Aws::String&& value) { m_locationHasBeenSet = true; m_location = std::move(value); }
    inline void SetLocation(const char* value) { m_locationHasBeenSet = true; m_location.assign(value); }
    inline LayerVersionContentOutput& WithLocation(const Aws::String& value) { SetLocation(value); return *this;}
    inline LayerVersionContentOutput& WithLocation(Aws::String&& value) { SetLocation(std::move(value)); return *this;}
    inline LayerVersionContentOutput& WithLocation(const char* value) { SetLocation(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The SHA-256 hash of the layer archive.</p>
     */
    inline const Aws::String& GetCodeSha256() const{ return m_codeSha256; }
    inline bool CodeSha256HasBeenSet() const { return m_codeSha256HasBeenSet; }
    inline void SetCodeSha256(const Aws::String& value) { m_codeSha256HasBeenSet = true; m_codeSha256 = value; }
    inline void SetCodeSha256(Aws::String&& value) { m_codeSha256HasBeenSet = true; m_codeSha256 = std::move(value); }
    inline void SetCodeSha256(const char* value) { m_codeSha256HasBeenSet = true; m_codeSha256.assign(value); }
    inline LayerVersionContentOutput& WithCodeSha256(const Aws::String& value) { SetCodeSha256(value); return *this;}
    inline LayerVersionContentOutput& WithCodeSha256(Aws::String&& value) { SetCodeSha256(std::move(value)); return *this;}
    inline LayerVersionContentOutput& WithCodeSha256(const char* value) { SetCodeSha256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The size of the layer archive in bytes.</p>
     */
    inline long long GetCodeSize() const{ return m_codeSize; }
    inline bool CodeSizeHasBeenSet() const { return m_codeSizeHasBeenSet; }
    inline void SetCodeSize(long long value) { m_codeSizeHasBeenSet = true; m_codeSize = value; }
    inline LayerVersionContentOutput& WithCodeSize(long long value) { SetCodeSize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) for a signing profile version.</p>
     */
    inline const Aws::String& GetSigningProfileVersionArn() const{ return m_signingProfileVersionArn; }
    inline bool SigningProfileVersionArnHasBeenSet() const { return m_signingProfileVersionArnHasBeenSet; }
    inline void SetSigningProfileVersionArn(const Aws::String& value) { m_signingProfileVersionArnHasBeenSet = true; m_signingProfileVersionArn = value; }
    inline void SetSigningProfileVersionArn(Aws::String&& value) { m_signingProfileVersionArnHasBeenSet = true; m_signingProfileVersionArn = std::move(value); }
    inline void SetSigningProfileVersionArn(const char* value) { m_signingProfileVersionArnHasBeenSet = true; m_signingProfileVersionArn.assign(value); }
    inline LayerVersionContentOutput& WithSigningProfileVersionArn(const Aws::String& value) { SetSigningProfileVersionArn(value); return *this;}
    inline LayerVersionContentOutput& WithSigningProfileVersionArn(Aws::String&& value) { SetSigningProfileVersionArn(std::move(value)); return *this;}
    inline LayerVersionContentOutput& WithSigningProfileVersionArn(const char* value) { SetSigningProfileVersionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of a signing job.</p>
     */
    inline const Aws::String& GetSigningJobArn() const{ return m_signingJobArn; }
    inline bool SigningJobArnHasBeenSet() const { return m_signingJobArnHasBeenSet; }
    inline void SetSigningJobArn(const Aws::String& value) { m_signingJobArnHasBeenSet = true; m_signingJobArn = value; }
    inline void SetSigningJobArn(Aws::String&& value) { m_signingJobArnHasBeenSet = true; m_signingJobArn = std::move(value); }
    inline void SetSigningJobArn(const char* value) { m_signingJobArnHasBeenSet = true; m_signingJobArn.assign(value); }
    inline LayerVersionContentOutput& WithSigningJobArn(const Aws::String& value) { SetSigningJobArn(value); return *this;}
    inline LayerVersionContentOutput& WithSigningJobArn(Aws::String&& value) { SetSigningJobArn(std::move(value)); return *this;}
    inline LayerVersionContentOutput& WithSigningJobArn(const char* value) { SetSigningJobArn(value); return *this;}
    ///@}
  private:

    Aws::String m_location;
    bool m_locationHasBeenSet = false;

    Aws::String m_codeSha256;
    bool m_codeSha256HasBeenSet = false;

    long long m_codeSize;
    bool m_codeSizeHasBeenSet = false;

    Aws::String m_signingProfileVersionArn;
    bool m_signingProfileVersionArnHasBeenSet = false;

    Aws::String m_signingJobArn;
    bool m_signingJobArnHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
