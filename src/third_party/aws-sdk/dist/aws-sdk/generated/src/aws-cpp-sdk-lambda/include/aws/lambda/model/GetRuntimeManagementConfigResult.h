﻿/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/UpdateRuntimeOn.h>
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
  class GetRuntimeManagementConfigResult
  {
  public:
    AWS_LAMBDA_API GetRuntimeManagementConfigResult();
    AWS_LAMBDA_API GetRuntimeManagementConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetRuntimeManagementConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The current runtime update mode of the function.</p>
     */
    inline const UpdateRuntimeOn& GetUpdateRuntimeOn() const{ return m_updateRuntimeOn; }
    inline void SetUpdateRuntimeOn(const UpdateRuntimeOn& value) { m_updateRuntimeOn = value; }
    inline void SetUpdateRuntimeOn(UpdateRuntimeOn&& value) { m_updateRuntimeOn = std::move(value); }
    inline GetRuntimeManagementConfigResult& WithUpdateRuntimeOn(const UpdateRuntimeOn& value) { SetUpdateRuntimeOn(value); return *this;}
    inline GetRuntimeManagementConfigResult& WithUpdateRuntimeOn(UpdateRuntimeOn&& value) { SetUpdateRuntimeOn(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the runtime the function is configured to use. If the runtime
     * update mode is <b>Manual</b>, the ARN is returned, otherwise <code>null</code>
     * is returned.</p>
     */
    inline const Aws::String& GetRuntimeVersionArn() const{ return m_runtimeVersionArn; }
    inline void SetRuntimeVersionArn(const Aws::String& value) { m_runtimeVersionArn = value; }
    inline void SetRuntimeVersionArn(Aws::String&& value) { m_runtimeVersionArn = std::move(value); }
    inline void SetRuntimeVersionArn(const char* value) { m_runtimeVersionArn.assign(value); }
    inline GetRuntimeManagementConfigResult& WithRuntimeVersionArn(const Aws::String& value) { SetRuntimeVersionArn(value); return *this;}
    inline GetRuntimeManagementConfigResult& WithRuntimeVersionArn(Aws::String&& value) { SetRuntimeVersionArn(std::move(value)); return *this;}
    inline GetRuntimeManagementConfigResult& WithRuntimeVersionArn(const char* value) { SetRuntimeVersionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of your function.</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArn.assign(value); }
    inline GetRuntimeManagementConfigResult& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline GetRuntimeManagementConfigResult& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline GetRuntimeManagementConfigResult& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetRuntimeManagementConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetRuntimeManagementConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetRuntimeManagementConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    UpdateRuntimeOn m_updateRuntimeOn;

    Aws::String m_runtimeVersionArn;

    Aws::String m_functionArn;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
