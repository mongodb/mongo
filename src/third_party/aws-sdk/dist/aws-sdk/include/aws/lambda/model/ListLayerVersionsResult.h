/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/LayerVersionsListItem.h>
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
  class ListLayerVersionsResult
  {
  public:
    AWS_LAMBDA_API ListLayerVersionsResult();
    AWS_LAMBDA_API ListLayerVersionsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListLayerVersionsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A pagination token returned when the response doesn't contain all
     * versions.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListLayerVersionsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListLayerVersionsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListLayerVersionsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of versions.</p>
     */
    inline const Aws::Vector<LayerVersionsListItem>& GetLayerVersions() const{ return m_layerVersions; }
    inline void SetLayerVersions(const Aws::Vector<LayerVersionsListItem>& value) { m_layerVersions = value; }
    inline void SetLayerVersions(Aws::Vector<LayerVersionsListItem>&& value) { m_layerVersions = std::move(value); }
    inline ListLayerVersionsResult& WithLayerVersions(const Aws::Vector<LayerVersionsListItem>& value) { SetLayerVersions(value); return *this;}
    inline ListLayerVersionsResult& WithLayerVersions(Aws::Vector<LayerVersionsListItem>&& value) { SetLayerVersions(std::move(value)); return *this;}
    inline ListLayerVersionsResult& AddLayerVersions(const LayerVersionsListItem& value) { m_layerVersions.push_back(value); return *this; }
    inline ListLayerVersionsResult& AddLayerVersions(LayerVersionsListItem&& value) { m_layerVersions.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListLayerVersionsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListLayerVersionsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListLayerVersionsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_nextMarker;

    Aws::Vector<LayerVersionsListItem> m_layerVersions;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
