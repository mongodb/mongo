/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  /**
   * <p>A chunk of the streamed response payload.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/InvokeResponseStreamUpdate">AWS
   * API Reference</a></p>
   */
  class InvokeResponseStreamUpdate
  {
  public:
    AWS_LAMBDA_API InvokeResponseStreamUpdate() = default;
    AWS_LAMBDA_API InvokeResponseStreamUpdate(Aws::Vector<unsigned char>&& value) { m_payload = std::move(value); }

    ///@{
    /**
     * <p>Data returned by your Lambda function.</p>
     */
    inline const Aws::Vector<unsigned char>& GetPayload() const { return m_payload; }
    inline Aws::Vector<unsigned char>&& GetPayloadWithOwnership() { return std::move(m_payload); }
    inline void SetPayload(const Aws::Vector<unsigned char>& value) { m_payloadHasBeenSet = true; m_payload = value; }
    inline void SetPayload(Aws::Vector<unsigned char>&& value) { m_payloadHasBeenSet = true; m_payload = std::move(value); }
    inline InvokeResponseStreamUpdate& WithPayload(const Aws::Vector<unsigned char>& value) { SetPayload(value); return *this;}
    inline InvokeResponseStreamUpdate& WithPayload(Aws::Vector<unsigned char>&& value) { SetPayload(std::move(value)); return *this;}
    ///@}

  private:

    Aws::Vector<unsigned char> m_payload;
    bool m_payloadHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
