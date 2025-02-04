/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/model/SubscribeToShardEvent.h>
#include <aws/kinesis/model/ResourceNotFoundException.h>
#include <aws/kinesis/model/ResourceInUseException.h>
#include <aws/kinesis/model/KMSDisabledException.h>
#include <aws/kinesis/model/KMSInvalidStateException.h>
#include <aws/kinesis/model/KMSAccessDeniedException.h>
#include <aws/kinesis/model/KMSNotFoundException.h>
#include <aws/kinesis/model/KMSOptInRequired.h>
#include <aws/kinesis/model/KMSThrottlingException.h>
#include <aws/kinesis/model/InternalFailureException.h>
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
   * <p>This is a tagged union for all of the types of events an enhanced fan-out
   * consumer can receive over HTTP/2 after a call to
   * <a>SubscribeToShard</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/SubscribeToShardEventStream">AWS
   * API Reference</a></p>
   */
  class SubscribeToShardEventStream
  {
  public:
    AWS_KINESIS_API SubscribeToShardEventStream();
    AWS_KINESIS_API SubscribeToShardEventStream(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API SubscribeToShardEventStream& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>After you call <a>SubscribeToShard</a>, Kinesis Data Streams sends events of
     * this type to your consumer. For an example of how to handle these events, see <a
     * href="/streams/latest/dev/building-enhanced-consumers-api.html">Enhanced Fan-Out
     * Using the Kinesis Data Streams API</a>.</p>
     */
    inline const SubscribeToShardEvent& GetSubscribeToShardEvent() const{ return m_subscribeToShardEvent; }
    inline bool SubscribeToShardEventHasBeenSet() const { return m_subscribeToShardEventHasBeenSet; }
    inline void SetSubscribeToShardEvent(const SubscribeToShardEvent& value) { m_subscribeToShardEventHasBeenSet = true; m_subscribeToShardEvent = value; }
    inline void SetSubscribeToShardEvent(SubscribeToShardEvent&& value) { m_subscribeToShardEventHasBeenSet = true; m_subscribeToShardEvent = std::move(value); }
    inline SubscribeToShardEventStream& WithSubscribeToShardEvent(const SubscribeToShardEvent& value) { SetSubscribeToShardEvent(value); return *this;}
    inline SubscribeToShardEventStream& WithSubscribeToShardEvent(SubscribeToShardEvent&& value) { SetSubscribeToShardEvent(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResourceNotFoundException& GetResourceNotFoundException() const{ return m_resourceNotFoundException; }
    inline bool ResourceNotFoundExceptionHasBeenSet() const { return m_resourceNotFoundExceptionHasBeenSet; }
    inline void SetResourceNotFoundException(const ResourceNotFoundException& value) { m_resourceNotFoundExceptionHasBeenSet = true; m_resourceNotFoundException = value; }
    inline void SetResourceNotFoundException(ResourceNotFoundException&& value) { m_resourceNotFoundExceptionHasBeenSet = true; m_resourceNotFoundException = std::move(value); }
    inline SubscribeToShardEventStream& WithResourceNotFoundException(const ResourceNotFoundException& value) { SetResourceNotFoundException(value); return *this;}
    inline SubscribeToShardEventStream& WithResourceNotFoundException(ResourceNotFoundException&& value) { SetResourceNotFoundException(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResourceInUseException& GetResourceInUseException() const{ return m_resourceInUseException; }
    inline bool ResourceInUseExceptionHasBeenSet() const { return m_resourceInUseExceptionHasBeenSet; }
    inline void SetResourceInUseException(const ResourceInUseException& value) { m_resourceInUseExceptionHasBeenSet = true; m_resourceInUseException = value; }
    inline void SetResourceInUseException(ResourceInUseException&& value) { m_resourceInUseExceptionHasBeenSet = true; m_resourceInUseException = std::move(value); }
    inline SubscribeToShardEventStream& WithResourceInUseException(const ResourceInUseException& value) { SetResourceInUseException(value); return *this;}
    inline SubscribeToShardEventStream& WithResourceInUseException(ResourceInUseException&& value) { SetResourceInUseException(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const KMSDisabledException& GetKMSDisabledException() const{ return m_kMSDisabledException; }
    inline bool KMSDisabledExceptionHasBeenSet() const { return m_kMSDisabledExceptionHasBeenSet; }
    inline void SetKMSDisabledException(const KMSDisabledException& value) { m_kMSDisabledExceptionHasBeenSet = true; m_kMSDisabledException = value; }
    inline void SetKMSDisabledException(KMSDisabledException&& value) { m_kMSDisabledExceptionHasBeenSet = true; m_kMSDisabledException = std::move(value); }
    inline SubscribeToShardEventStream& WithKMSDisabledException(const KMSDisabledException& value) { SetKMSDisabledException(value); return *this;}
    inline SubscribeToShardEventStream& WithKMSDisabledException(KMSDisabledException&& value) { SetKMSDisabledException(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const KMSInvalidStateException& GetKMSInvalidStateException() const{ return m_kMSInvalidStateException; }
    inline bool KMSInvalidStateExceptionHasBeenSet() const { return m_kMSInvalidStateExceptionHasBeenSet; }
    inline void SetKMSInvalidStateException(const KMSInvalidStateException& value) { m_kMSInvalidStateExceptionHasBeenSet = true; m_kMSInvalidStateException = value; }
    inline void SetKMSInvalidStateException(KMSInvalidStateException&& value) { m_kMSInvalidStateExceptionHasBeenSet = true; m_kMSInvalidStateException = std::move(value); }
    inline SubscribeToShardEventStream& WithKMSInvalidStateException(const KMSInvalidStateException& value) { SetKMSInvalidStateException(value); return *this;}
    inline SubscribeToShardEventStream& WithKMSInvalidStateException(KMSInvalidStateException&& value) { SetKMSInvalidStateException(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const KMSAccessDeniedException& GetKMSAccessDeniedException() const{ return m_kMSAccessDeniedException; }
    inline bool KMSAccessDeniedExceptionHasBeenSet() const { return m_kMSAccessDeniedExceptionHasBeenSet; }
    inline void SetKMSAccessDeniedException(const KMSAccessDeniedException& value) { m_kMSAccessDeniedExceptionHasBeenSet = true; m_kMSAccessDeniedException = value; }
    inline void SetKMSAccessDeniedException(KMSAccessDeniedException&& value) { m_kMSAccessDeniedExceptionHasBeenSet = true; m_kMSAccessDeniedException = std::move(value); }
    inline SubscribeToShardEventStream& WithKMSAccessDeniedException(const KMSAccessDeniedException& value) { SetKMSAccessDeniedException(value); return *this;}
    inline SubscribeToShardEventStream& WithKMSAccessDeniedException(KMSAccessDeniedException&& value) { SetKMSAccessDeniedException(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const KMSNotFoundException& GetKMSNotFoundException() const{ return m_kMSNotFoundException; }
    inline bool KMSNotFoundExceptionHasBeenSet() const { return m_kMSNotFoundExceptionHasBeenSet; }
    inline void SetKMSNotFoundException(const KMSNotFoundException& value) { m_kMSNotFoundExceptionHasBeenSet = true; m_kMSNotFoundException = value; }
    inline void SetKMSNotFoundException(KMSNotFoundException&& value) { m_kMSNotFoundExceptionHasBeenSet = true; m_kMSNotFoundException = std::move(value); }
    inline SubscribeToShardEventStream& WithKMSNotFoundException(const KMSNotFoundException& value) { SetKMSNotFoundException(value); return *this;}
    inline SubscribeToShardEventStream& WithKMSNotFoundException(KMSNotFoundException&& value) { SetKMSNotFoundException(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const KMSOptInRequired& GetKMSOptInRequired() const{ return m_kMSOptInRequired; }
    inline bool KMSOptInRequiredHasBeenSet() const { return m_kMSOptInRequiredHasBeenSet; }
    inline void SetKMSOptInRequired(const KMSOptInRequired& value) { m_kMSOptInRequiredHasBeenSet = true; m_kMSOptInRequired = value; }
    inline void SetKMSOptInRequired(KMSOptInRequired&& value) { m_kMSOptInRequiredHasBeenSet = true; m_kMSOptInRequired = std::move(value); }
    inline SubscribeToShardEventStream& WithKMSOptInRequired(const KMSOptInRequired& value) { SetKMSOptInRequired(value); return *this;}
    inline SubscribeToShardEventStream& WithKMSOptInRequired(KMSOptInRequired&& value) { SetKMSOptInRequired(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const KMSThrottlingException& GetKMSThrottlingException() const{ return m_kMSThrottlingException; }
    inline bool KMSThrottlingExceptionHasBeenSet() const { return m_kMSThrottlingExceptionHasBeenSet; }
    inline void SetKMSThrottlingException(const KMSThrottlingException& value) { m_kMSThrottlingExceptionHasBeenSet = true; m_kMSThrottlingException = value; }
    inline void SetKMSThrottlingException(KMSThrottlingException&& value) { m_kMSThrottlingExceptionHasBeenSet = true; m_kMSThrottlingException = std::move(value); }
    inline SubscribeToShardEventStream& WithKMSThrottlingException(const KMSThrottlingException& value) { SetKMSThrottlingException(value); return *this;}
    inline SubscribeToShardEventStream& WithKMSThrottlingException(KMSThrottlingException&& value) { SetKMSThrottlingException(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The processing of the request failed because of an unknown error, exception,
     * or failure.</p>
     */
    inline const InternalFailureException& GetInternalFailureException() const{ return m_internalFailureException; }
    inline bool InternalFailureExceptionHasBeenSet() const { return m_internalFailureExceptionHasBeenSet; }
    inline void SetInternalFailureException(const InternalFailureException& value) { m_internalFailureExceptionHasBeenSet = true; m_internalFailureException = value; }
    inline void SetInternalFailureException(InternalFailureException&& value) { m_internalFailureExceptionHasBeenSet = true; m_internalFailureException = std::move(value); }
    inline SubscribeToShardEventStream& WithInternalFailureException(const InternalFailureException& value) { SetInternalFailureException(value); return *this;}
    inline SubscribeToShardEventStream& WithInternalFailureException(InternalFailureException&& value) { SetInternalFailureException(std::move(value)); return *this;}
    ///@}
  private:

    SubscribeToShardEvent m_subscribeToShardEvent;
    bool m_subscribeToShardEventHasBeenSet = false;

    ResourceNotFoundException m_resourceNotFoundException;
    bool m_resourceNotFoundExceptionHasBeenSet = false;

    ResourceInUseException m_resourceInUseException;
    bool m_resourceInUseExceptionHasBeenSet = false;

    KMSDisabledException m_kMSDisabledException;
    bool m_kMSDisabledExceptionHasBeenSet = false;

    KMSInvalidStateException m_kMSInvalidStateException;
    bool m_kMSInvalidStateExceptionHasBeenSet = false;

    KMSAccessDeniedException m_kMSAccessDeniedException;
    bool m_kMSAccessDeniedExceptionHasBeenSet = false;

    KMSNotFoundException m_kMSNotFoundException;
    bool m_kMSNotFoundExceptionHasBeenSet = false;

    KMSOptInRequired m_kMSOptInRequired;
    bool m_kMSOptInRequiredHasBeenSet = false;

    KMSThrottlingException m_kMSThrottlingException;
    bool m_kMSThrottlingExceptionHasBeenSet = false;

    InternalFailureException m_internalFailureException;
    bool m_internalFailureExceptionHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
