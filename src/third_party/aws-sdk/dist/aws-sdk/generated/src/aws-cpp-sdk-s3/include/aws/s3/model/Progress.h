/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>

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
   * <p>This data type contains information about progress of an
   * operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Progress">AWS API
   * Reference</a></p>
   */
  class Progress
  {
  public:
    AWS_S3_API Progress();
    AWS_S3_API Progress(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Progress& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The current number of object bytes scanned.</p>
     */
    inline long long GetBytesScanned() const{ return m_bytesScanned; }
    inline bool BytesScannedHasBeenSet() const { return m_bytesScannedHasBeenSet; }
    inline void SetBytesScanned(long long value) { m_bytesScannedHasBeenSet = true; m_bytesScanned = value; }
    inline Progress& WithBytesScanned(long long value) { SetBytesScanned(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The current number of uncompressed object bytes processed.</p>
     */
    inline long long GetBytesProcessed() const{ return m_bytesProcessed; }
    inline bool BytesProcessedHasBeenSet() const { return m_bytesProcessedHasBeenSet; }
    inline void SetBytesProcessed(long long value) { m_bytesProcessedHasBeenSet = true; m_bytesProcessed = value; }
    inline Progress& WithBytesProcessed(long long value) { SetBytesProcessed(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The current number of bytes of records payload data returned.</p>
     */
    inline long long GetBytesReturned() const{ return m_bytesReturned; }
    inline bool BytesReturnedHasBeenSet() const { return m_bytesReturnedHasBeenSet; }
    inline void SetBytesReturned(long long value) { m_bytesReturnedHasBeenSet = true; m_bytesReturned = value; }
    inline Progress& WithBytesReturned(long long value) { SetBytesReturned(value); return *this;}
    ///@}
  private:

    long long m_bytesScanned;
    bool m_bytesScannedHasBeenSet = false;

    long long m_bytesProcessed;
    bool m_bytesProcessedHasBeenSet = false;

    long long m_bytesReturned;
    bool m_bytesReturnedHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
