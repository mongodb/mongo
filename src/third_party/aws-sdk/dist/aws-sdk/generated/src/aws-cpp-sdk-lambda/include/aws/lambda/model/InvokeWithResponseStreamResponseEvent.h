/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/InvokeResponseStreamUpdate.h>
#include <aws/lambda/model/InvokeWithResponseStreamCompleteEvent.h>
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
   * <p>An object that includes a chunk of the response payload. When the stream has
   * ended, Lambda includes a <code>InvokeComplete</code> object.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/InvokeWithResponseStreamResponseEvent">AWS
   * API Reference</a></p>
   */
  class InvokeWithResponseStreamResponseEvent
  {
  public:
    AWS_LAMBDA_API InvokeWithResponseStreamResponseEvent();
    AWS_LAMBDA_API InvokeWithResponseStreamResponseEvent(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API InvokeWithResponseStreamResponseEvent& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>A chunk of the streamed response payload.</p>
     */
    inline const InvokeResponseStreamUpdate& GetPayloadChunk() const{ return m_payloadChunk; }
    inline bool PayloadChunkHasBeenSet() const { return m_payloadChunkHasBeenSet; }
    inline void SetPayloadChunk(const InvokeResponseStreamUpdate& value) { m_payloadChunkHasBeenSet = true; m_payloadChunk = value; }
    inline void SetPayloadChunk(InvokeResponseStreamUpdate&& value) { m_payloadChunkHasBeenSet = true; m_payloadChunk = std::move(value); }
    inline InvokeWithResponseStreamResponseEvent& WithPayloadChunk(const InvokeResponseStreamUpdate& value) { SetPayloadChunk(value); return *this;}
    inline InvokeWithResponseStreamResponseEvent& WithPayloadChunk(InvokeResponseStreamUpdate&& value) { SetPayloadChunk(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>An object that's returned when the stream has ended and all the payload
     * chunks have been returned.</p>
     */
    inline const InvokeWithResponseStreamCompleteEvent& GetInvokeComplete() const{ return m_invokeComplete; }
    inline bool InvokeCompleteHasBeenSet() const { return m_invokeCompleteHasBeenSet; }
    inline void SetInvokeComplete(const InvokeWithResponseStreamCompleteEvent& value) { m_invokeCompleteHasBeenSet = true; m_invokeComplete = value; }
    inline void SetInvokeComplete(InvokeWithResponseStreamCompleteEvent&& value) { m_invokeCompleteHasBeenSet = true; m_invokeComplete = std::move(value); }
    inline InvokeWithResponseStreamResponseEvent& WithInvokeComplete(const InvokeWithResponseStreamCompleteEvent& value) { SetInvokeComplete(value); return *this;}
    inline InvokeWithResponseStreamResponseEvent& WithInvokeComplete(InvokeWithResponseStreamCompleteEvent&& value) { SetInvokeComplete(std::move(value)); return *this;}
    ///@}
  private:

    InvokeResponseStreamUpdate m_payloadChunk;
    bool m_payloadChunkHasBeenSet = false;

    InvokeWithResponseStreamCompleteEvent m_invokeComplete;
    bool m_invokeCompleteHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
