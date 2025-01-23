/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/AliasConfiguration.h>
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
  class ListAliasesResult
  {
  public:
    AWS_LAMBDA_API ListAliasesResult();
    AWS_LAMBDA_API ListAliasesResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListAliasesResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The pagination token that's included if more results are available.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListAliasesResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListAliasesResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListAliasesResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of aliases.</p>
     */
    inline const Aws::Vector<AliasConfiguration>& GetAliases() const{ return m_aliases; }
    inline void SetAliases(const Aws::Vector<AliasConfiguration>& value) { m_aliases = value; }
    inline void SetAliases(Aws::Vector<AliasConfiguration>&& value) { m_aliases = std::move(value); }
    inline ListAliasesResult& WithAliases(const Aws::Vector<AliasConfiguration>& value) { SetAliases(value); return *this;}
    inline ListAliasesResult& WithAliases(Aws::Vector<AliasConfiguration>&& value) { SetAliases(std::move(value)); return *this;}
    inline ListAliasesResult& AddAliases(const AliasConfiguration& value) { m_aliases.push_back(value); return *this; }
    inline ListAliasesResult& AddAliases(AliasConfiguration&& value) { m_aliases.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListAliasesResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListAliasesResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListAliasesResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_nextMarker;

    Aws::Vector<AliasConfiguration> m_aliases;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
