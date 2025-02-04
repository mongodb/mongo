/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/DateTime.h>
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
   * <p>Container for elements related to a part.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Part">AWS API
   * Reference</a></p>
   */
  class Part
  {
  public:
    AWS_S3_API Part();
    AWS_S3_API Part(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Part& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Part number identifying the part. This is a positive integer between 1 and
     * 10,000.</p>
     */
    inline int GetPartNumber() const{ return m_partNumber; }
    inline bool PartNumberHasBeenSet() const { return m_partNumberHasBeenSet; }
    inline void SetPartNumber(int value) { m_partNumberHasBeenSet = true; m_partNumber = value; }
    inline Part& WithPartNumber(int value) { SetPartNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date and time at which the part was uploaded.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline bool LastModifiedHasBeenSet() const { return m_lastModifiedHasBeenSet; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModifiedHasBeenSet = true; m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModifiedHasBeenSet = true; m_lastModified = std::move(value); }
    inline Part& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline Part& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Entity tag returned when the part was uploaded.</p>
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline bool ETagHasBeenSet() const { return m_eTagHasBeenSet; }
    inline void SetETag(const Aws::String& value) { m_eTagHasBeenSet = true; m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTagHasBeenSet = true; m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTagHasBeenSet = true; m_eTag.assign(value); }
    inline Part& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline Part& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline Part& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Size in bytes of the uploaded part data.</p>
     */
    inline long long GetSize() const{ return m_size; }
    inline bool SizeHasBeenSet() const { return m_sizeHasBeenSet; }
    inline void SetSize(long long value) { m_sizeHasBeenSet = true; m_size = value; }
    inline Part& WithSize(long long value) { SetSize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This header specifies the
     * base64-encoded, 32-bit CRC-32 checksum of the object. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32() const{ return m_checksumCRC32; }
    inline bool ChecksumCRC32HasBeenSet() const { return m_checksumCRC32HasBeenSet; }
    inline void SetChecksumCRC32(const Aws::String& value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32 = value; }
    inline void SetChecksumCRC32(Aws::String&& value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32 = std::move(value); }
    inline void SetChecksumCRC32(const char* value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32.assign(value); }
    inline Part& WithChecksumCRC32(const Aws::String& value) { SetChecksumCRC32(value); return *this;}
    inline Part& WithChecksumCRC32(Aws::String&& value) { SetChecksumCRC32(std::move(value)); return *this;}
    inline Part& WithChecksumCRC32(const char* value) { SetChecksumCRC32(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32C checksum of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32C() const{ return m_checksumCRC32C; }
    inline bool ChecksumCRC32CHasBeenSet() const { return m_checksumCRC32CHasBeenSet; }
    inline void SetChecksumCRC32C(const Aws::String& value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C = value; }
    inline void SetChecksumCRC32C(Aws::String&& value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C = std::move(value); }
    inline void SetChecksumCRC32C(const char* value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C.assign(value); }
    inline Part& WithChecksumCRC32C(const Aws::String& value) { SetChecksumCRC32C(value); return *this;}
    inline Part& WithChecksumCRC32C(Aws::String&& value) { SetChecksumCRC32C(std::move(value)); return *this;}
    inline Part& WithChecksumCRC32C(const char* value) { SetChecksumCRC32C(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 160-bit SHA-1 digest of the object. This will only be
     * present if it was uploaded with the object. When you use the API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA1() const{ return m_checksumSHA1; }
    inline bool ChecksumSHA1HasBeenSet() const { return m_checksumSHA1HasBeenSet; }
    inline void SetChecksumSHA1(const Aws::String& value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1 = value; }
    inline void SetChecksumSHA1(Aws::String&& value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1 = std::move(value); }
    inline void SetChecksumSHA1(const char* value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1.assign(value); }
    inline Part& WithChecksumSHA1(const Aws::String& value) { SetChecksumSHA1(value); return *this;}
    inline Part& WithChecksumSHA1(Aws::String&& value) { SetChecksumSHA1(std::move(value)); return *this;}
    inline Part& WithChecksumSHA1(const char* value) { SetChecksumSHA1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This header specifies the
     * base64-encoded, 256-bit SHA-256 digest of the object. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA256() const{ return m_checksumSHA256; }
    inline bool ChecksumSHA256HasBeenSet() const { return m_checksumSHA256HasBeenSet; }
    inline void SetChecksumSHA256(const Aws::String& value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256 = value; }
    inline void SetChecksumSHA256(Aws::String&& value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256 = std::move(value); }
    inline void SetChecksumSHA256(const char* value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256.assign(value); }
    inline Part& WithChecksumSHA256(const Aws::String& value) { SetChecksumSHA256(value); return *this;}
    inline Part& WithChecksumSHA256(Aws::String&& value) { SetChecksumSHA256(std::move(value)); return *this;}
    inline Part& WithChecksumSHA256(const char* value) { SetChecksumSHA256(value); return *this;}
    ///@}
  private:

    int m_partNumber;
    bool m_partNumberHasBeenSet = false;

    Aws::Utils::DateTime m_lastModified;
    bool m_lastModifiedHasBeenSet = false;

    Aws::String m_eTag;
    bool m_eTagHasBeenSet = false;

    long long m_size;
    bool m_sizeHasBeenSet = false;

    Aws::String m_checksumCRC32;
    bool m_checksumCRC32HasBeenSet = false;

    Aws::String m_checksumCRC32C;
    bool m_checksumCRC32CHasBeenSet = false;

    Aws::String m_checksumSHA1;
    bool m_checksumSHA1HasBeenSet = false;

    Aws::String m_checksumSHA256;
    bool m_checksumSHA256HasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
