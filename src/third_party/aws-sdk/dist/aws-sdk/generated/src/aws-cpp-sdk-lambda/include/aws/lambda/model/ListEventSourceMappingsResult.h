/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/EventSourceMappingConfiguration.h>
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
namespace Lambda
{
namespace Model
{
  class ListEventSourceMappingsResult
  {
  public:
    AWS_LAMBDA_API ListEventSourceMappingsResult();
    AWS_LAMBDA_API ListEventSourceMappingsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListEventSourceMappingsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A pagination token that's returned when the response doesn't contain all
     * event source mappings.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListEventSourceMappingsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListEventSourceMappingsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListEventSourceMappingsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of event source mappings.</p>
     */
    inline const Aws::Vector<EventSourceMappingConfiguration>& GetEventSourceMappings() const{ return m_eventSourceMappings; }
    inline void SetEventSourceMappings(const Aws::Vector<EventSourceMappingConfiguration>& value) { m_eventSourceMappings = value; }
    inline void SetEventSourceMappings(Aws::Vector<EventSourceMappingConfiguration>&& value) { m_eventSourceMappings = std::move(value); }
    inline ListEventSourceMappingsResult& WithEventSourceMappings(const Aws::Vector<EventSourceMappingConfiguration>& value) { SetEventSourceMappings(value); return *this;}
    inline ListEventSourceMappingsResult& WithEventSourceMappings(Aws::Vector<EventSourceMappingConfiguration>&& value) { SetEventSourceMappings(std::move(value)); return *this;}
    inline ListEventSourceMappingsResult& AddEventSourceMappings(const EventSourceMappingConfiguration& value) { m_eventSourceMappings.push_back(value); return *this; }
    inline ListEventSourceMappingsResult& AddEventSourceMappings(EventSourceMappingConfiguration&& value) { m_eventSourceMappings.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListEventSourceMappingsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListEventSourceMappingsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListEventSourceMappingsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_nextMarker;

    Aws::Vector<EventSourceMappingConfiguration> m_eventSourceMappings;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
