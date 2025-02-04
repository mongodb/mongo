/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class AddLayerVersionPermissionResult
  {
  public:
    AWS_LAMBDA_API AddLayerVersionPermissionResult();
    AWS_LAMBDA_API AddLayerVersionPermissionResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API AddLayerVersionPermissionResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The permission statement.</p>
     */
    inline const Aws::String& GetStatement() const{ return m_statement; }
    inline void SetStatement(const Aws::String& value) { m_statement = value; }
    inline void SetStatement(Aws::String&& value) { m_statement = std::move(value); }
    inline void SetStatement(const char* value) { m_statement.assign(value); }
    inline AddLayerVersionPermissionResult& WithStatement(const Aws::String& value) { SetStatement(value); return *this;}
    inline AddLayerVersionPermissionResult& WithStatement(Aws::String&& value) { SetStatement(std::move(value)); return *this;}
    inline AddLayerVersionPermissionResult& WithStatement(const char* value) { SetStatement(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique identifier for the current revision of the policy.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionId.assign(value); }
    inline AddLayerVersionPermissionResult& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline AddLayerVersionPermissionResult& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline AddLayerVersionPermissionResult& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline AddLayerVersionPermissionResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline AddLayerVersionPermissionResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline AddLayerVersionPermissionResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_statement;

    Aws::String m_revisionId;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
