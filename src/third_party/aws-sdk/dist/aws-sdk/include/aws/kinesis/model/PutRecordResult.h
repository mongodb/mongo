/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/EncryptionType.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{
  /**
   * <p>Represents the output for <code>PutRecord</code>.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/PutRecordOutput">AWS
   * API Reference</a></p>
   */
  class PutRecordResult
  {
  public:
    AWS_KINESIS_API PutRecordResult();
    AWS_KINESIS_API PutRecordResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API PutRecordResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The shard ID of the shard where the data record was placed.</p>
     */
    inline const Aws::String& GetShardId() const{ return m_shardId; }
    inline void SetShardId(const Aws::String& value) { m_shardId = value; }
    inline void SetShardId(Aws::String&& value) { m_shardId = std::move(value); }
    inline void SetShardId(const char* value) { m_shardId.assign(value); }
    inline PutRecordResult& WithShardId(const Aws::String& value) { SetShardId(value); return *this;}
    inline PutRecordResult& WithShardId(Aws::String&& value) { SetShardId(std::move(value)); return *this;}
    inline PutRecordResult& WithShardId(const char* value) { SetShardId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The sequence number identifier that was assigned to the put data record. The
     * sequence number for the record is unique across all records in the stream. A
     * sequence number is the identifier associated with every record put into the
     * stream.</p>
     */
    inline const Aws::String& GetSequenceNumber() const{ return m_sequenceNumber; }
    inline void SetSequenceNumber(const Aws::String& value) { m_sequenceNumber = value; }
    inline void SetSequenceNumber(Aws::String&& value) { m_sequenceNumber = std::move(value); }
    inline void SetSequenceNumber(const char* value) { m_sequenceNumber.assign(value); }
    inline PutRecordResult& WithSequenceNumber(const Aws::String& value) { SetSequenceNumber(value); return *this;}
    inline PutRecordResult& WithSequenceNumber(Aws::String&& value) { SetSequenceNumber(std::move(value)); return *this;}
    inline PutRecordResult& WithSequenceNumber(const char* value) { SetSequenceNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The encryption type to use on the record. This parameter can be one of the
     * following values:</p> <ul> <li> <p> <code>NONE</code>: Do not encrypt the
     * records in the stream.</p> </li> <li> <p> <code>KMS</code>: Use server-side
     * encryption on the records in the stream using a customer-managed Amazon Web
     * Services KMS key.</p> </li> </ul>
     */
    inline const EncryptionType& GetEncryptionType() const{ return m_encryptionType; }
    inline void SetEncryptionType(const EncryptionType& value) { m_encryptionType = value; }
    inline void SetEncryptionType(EncryptionType&& value) { m_encryptionType = std::move(value); }
    inline PutRecordResult& WithEncryptionType(const EncryptionType& value) { SetEncryptionType(value); return *this;}
    inline PutRecordResult& WithEncryptionType(EncryptionType&& value) { SetEncryptionType(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutRecordResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutRecordResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutRecordResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_shardId;

    Aws::String m_sequenceNumber;

    EncryptionType m_encryptionType;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
