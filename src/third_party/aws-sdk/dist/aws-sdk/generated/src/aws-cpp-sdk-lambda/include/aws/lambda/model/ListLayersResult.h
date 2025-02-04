/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/LayersListItem.h>
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
  class ListLayersResult
  {
  public:
    AWS_LAMBDA_API ListLayersResult();
    AWS_LAMBDA_API ListLayersResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListLayersResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A pagination token returned when the response doesn't contain all layers.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListLayersResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListLayersResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListLayersResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of function layers.</p>
     */
    inline const Aws::Vector<LayersListItem>& GetLayers() const{ return m_layers; }
    inline void SetLayers(const Aws::Vector<LayersListItem>& value) { m_layers = value; }
    inline void SetLayers(Aws::Vector<LayersListItem>&& value) { m_layers = std::move(value); }
    inline ListLayersResult& WithLayers(const Aws::Vector<LayersListItem>& value) { SetLayers(value); return *this;}
    inline ListLayersResult& WithLayers(Aws::Vector<LayersListItem>&& value) { SetLayers(std::move(value)); return *this;}
    inline ListLayersResult& AddLayers(const LayersListItem& value) { m_layers.push_back(value); return *this; }
    inline ListLayersResult& AddLayers(LayersListItem&& value) { m_layers.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListLayersResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListLayersResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListLayersResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_nextMarker;

    Aws::Vector<LayersListItem> m_layers;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
