/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <utility>

namespace Aws
{
namespace S3
{
namespace Model
{
  /**
   * <p>The container for the records event.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/RecordsEvent">AWS API
   * Reference</a></p>
   */
  class RecordsEvent
  {
  public:
    AWS_S3_API RecordsEvent() = default;
    AWS_S3_API RecordsEvent(Aws::Vector<unsigned char>&& value) { m_payload = std::move(value); }

    ///@{
    /**
     * <p>The byte array of partial, one or more result records. S3 Select doesn't
     * guarantee that a record will be self-contained in one record frame. To ensure
     * continuous streaming of data, S3 Select might split the same record across
     * multiple record frames instead of aggregating the results in memory. Some S3
     * clients (for example, the SDK for Java) handle this behavior by creating a
     * <code>ByteStream</code> out of the response by default. Other clients might not
     * handle this behavior by default. In those cases, you must aggregate the results
     * on the client side and parse the response.</p>
     */
    inline const Aws::Vector<unsigned char>& GetPayload() const { return m_payload; }
    inline Aws::Vector<unsigned char>&& GetPayloadWithOwnership() { return std::move(m_payload); }
    inline void SetPayload(const Aws::Vector<unsigned char>& value) { m_payloadHasBeenSet = true; m_payload = value; }
    inline void SetPayload(Aws::Vector<unsigned char>&& value) { m_payloadHasBeenSet = true; m_payload = std::move(value); }
    inline RecordsEvent& WithPayload(const Aws::Vector<unsigned char>& value) { SetPayload(value); return *this;}
    inline RecordsEvent& WithPayload(Aws::Vector<unsigned char>&& value) { SetPayload(std::move(value)); return *this;}
    ///@}

  private:

    Aws::Vector<unsigned char> m_payload;
    bool m_payloadHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
