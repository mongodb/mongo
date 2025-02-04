/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
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
namespace Kinesis
{
namespace Model
{

  /**
   * <p>Represents the result of an individual record from a <code>PutRecords</code>
   * request. A record that is successfully added to a stream includes
   * <code>SequenceNumber</code> and <code>ShardId</code> in the result. A record
   * that fails to be added to the stream includes <code>ErrorCode</code> and
   * <code>ErrorMessage</code> in the result.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/PutRecordsResultEntry">AWS
   * API Reference</a></p>
   */
  class PutRecordsResultEntry
  {
  public:
    AWS_KINESIS_API PutRecordsResultEntry();
    AWS_KINESIS_API PutRecordsResultEntry(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API PutRecordsResultEntry& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The sequence number for an individual record result.</p>
     */
    inline const Aws::String& GetSequenceNumber() const{ return m_sequenceNumber; }
    inline bool SequenceNumberHasBeenSet() const { return m_sequenceNumberHasBeenSet; }
    inline void SetSequenceNumber(const Aws::String& value) { m_sequenceNumberHasBeenSet = true; m_sequenceNumber = value; }
    inline void SetSequenceNumber(Aws::String&& value) { m_sequenceNumberHasBeenSet = true; m_sequenceNumber = std::move(value); }
    inline void SetSequenceNumber(const char* value) { m_sequenceNumberHasBeenSet = true; m_sequenceNumber.assign(value); }
    inline PutRecordsResultEntry& WithSequenceNumber(const Aws::String& value) { SetSequenceNumber(value); return *this;}
    inline PutRecordsResultEntry& WithSequenceNumber(Aws::String&& value) { SetSequenceNumber(std::move(value)); return *this;}
    inline PutRecordsResultEntry& WithSequenceNumber(const char* value) { SetSequenceNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The shard ID for an individual record result.</p>
     */
    inline const Aws::String& GetShardId() const{ return m_shardId; }
    inline bool ShardIdHasBeenSet() const { return m_shardIdHasBeenSet; }
    inline void SetShardId(const Aws::String& value) { m_shardIdHasBeenSet = true; m_shardId = value; }
    inline void SetShardId(Aws::String&& value) { m_shardIdHasBeenSet = true; m_shardId = std::move(value); }
    inline void SetShardId(const char* value) { m_shardIdHasBeenSet = true; m_shardId.assign(value); }
    inline PutRecordsResultEntry& WithShardId(const Aws::String& value) { SetShardId(value); return *this;}
    inline PutRecordsResultEntry& WithShardId(Aws::String&& value) { SetShardId(std::move(value)); return *this;}
    inline PutRecordsResultEntry& WithShardId(const char* value) { SetShardId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error code for an individual record result. <code>ErrorCodes</code> can
     * be either <code>ProvisionedThroughputExceededException</code> or
     * <code>InternalFailure</code>.</p>
     */
    inline const Aws::String& GetErrorCode() const{ return m_errorCode; }
    inline bool ErrorCodeHasBeenSet() const { return m_errorCodeHasBeenSet; }
    inline void SetErrorCode(const Aws::String& value) { m_errorCodeHasBeenSet = true; m_errorCode = value; }
    inline void SetErrorCode(Aws::String&& value) { m_errorCodeHasBeenSet = true; m_errorCode = std::move(value); }
    inline void SetErrorCode(const char* value) { m_errorCodeHasBeenSet = true; m_errorCode.assign(value); }
    inline PutRecordsResultEntry& WithErrorCode(const Aws::String& value) { SetErrorCode(value); return *this;}
    inline PutRecordsResultEntry& WithErrorCode(Aws::String&& value) { SetErrorCode(std::move(value)); return *this;}
    inline PutRecordsResultEntry& WithErrorCode(const char* value) { SetErrorCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error message for an individual record result. An <code>ErrorCode</code>
     * value of <code>ProvisionedThroughputExceededException</code> has an error
     * message that includes the account ID, stream name, and shard ID. An
     * <code>ErrorCode</code> value of <code>InternalFailure</code> has the error
     * message <code>"Internal Service Failure"</code>.</p>
     */
    inline const Aws::String& GetErrorMessage() const{ return m_errorMessage; }
    inline bool ErrorMessageHasBeenSet() const { return m_errorMessageHasBeenSet; }
    inline void SetErrorMessage(const Aws::String& value) { m_errorMessageHasBeenSet = true; m_errorMessage = value; }
    inline void SetErrorMessage(Aws::String&& value) { m_errorMessageHasBeenSet = true; m_errorMessage = std::move(value); }
    inline void SetErrorMessage(const char* value) { m_errorMessageHasBeenSet = true; m_errorMessage.assign(value); }
    inline PutRecordsResultEntry& WithErrorMessage(const Aws::String& value) { SetErrorMessage(value); return *this;}
    inline PutRecordsResultEntry& WithErrorMessage(Aws::String&& value) { SetErrorMessage(std::move(value)); return *this;}
    inline PutRecordsResultEntry& WithErrorMessage(const char* value) { SetErrorMessage(value); return *this;}
    ///@}
  private:

    Aws::String m_sequenceNumber;
    bool m_sequenceNumberHasBeenSet = false;

    Aws::String m_shardId;
    bool m_shardIdHasBeenSet = false;

    Aws::String m_errorCode;
    bool m_errorCodeHasBeenSet = false;

    Aws::String m_errorMessage;
    bool m_errorMessageHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
