/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/kinesis/model/EncryptionType.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/PutRecordsResultEntry.h>
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
   * <p> <code>PutRecords</code> results.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/PutRecordsOutput">AWS
   * API Reference</a></p>
   */
  class PutRecordsResult
  {
  public:
    AWS_KINESIS_API PutRecordsResult();
    AWS_KINESIS_API PutRecordsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API PutRecordsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The number of unsuccessfully processed records in a <code>PutRecords</code>
     * request.</p>
     */
    inline int GetFailedRecordCount() const{ return m_failedRecordCount; }
    inline void SetFailedRecordCount(int value) { m_failedRecordCount = value; }
    inline PutRecordsResult& WithFailedRecordCount(int value) { SetFailedRecordCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An array of successfully and unsuccessfully processed record results. A
     * record that is successfully added to a stream includes
     * <code>SequenceNumber</code> and <code>ShardId</code> in the result. A record
     * that fails to be added to a stream includes <code>ErrorCode</code> and
     * <code>ErrorMessage</code> in the result.</p>
     */
    inline const Aws::Vector<PutRecordsResultEntry>& GetRecords() const{ return m_records; }
    inline void SetRecords(const Aws::Vector<PutRecordsResultEntry>& value) { m_records = value; }
    inline void SetRecords(Aws::Vector<PutRecordsResultEntry>&& value) { m_records = std::move(value); }
    inline PutRecordsResult& WithRecords(const Aws::Vector<PutRecordsResultEntry>& value) { SetRecords(value); return *this;}
    inline PutRecordsResult& WithRecords(Aws::Vector<PutRecordsResultEntry>&& value) { SetRecords(std::move(value)); return *this;}
    inline PutRecordsResult& AddRecords(const PutRecordsResultEntry& value) { m_records.push_back(value); return *this; }
    inline PutRecordsResult& AddRecords(PutRecordsResultEntry&& value) { m_records.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The encryption type used on the records. This parameter can be one of the
     * following values:</p> <ul> <li> <p> <code>NONE</code>: Do not encrypt the
     * records.</p> </li> <li> <p> <code>KMS</code>: Use server-side encryption on the
     * records using a customer-managed Amazon Web Services KMS key.</p> </li> </ul>
     */
    inline const EncryptionType& GetEncryptionType() const{ return m_encryptionType; }
    inline void SetEncryptionType(const EncryptionType& value) { m_encryptionType = value; }
    inline void SetEncryptionType(EncryptionType&& value) { m_encryptionType = std::move(value); }
    inline PutRecordsResult& WithEncryptionType(const EncryptionType& value) { SetEncryptionType(value); return *this;}
    inline PutRecordsResult& WithEncryptionType(EncryptionType&& value) { SetEncryptionType(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutRecordsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutRecordsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutRecordsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_failedRecordCount;

    Aws::Vector<PutRecordsResultEntry> m_records;

    EncryptionType m_encryptionType;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
