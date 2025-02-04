/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/AliasRoutingConfiguration.h>
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
  /**
   * <p>Provides configuration information about a Lambda function <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias</a>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/AliasConfiguration">AWS
   * API Reference</a></p>
   */
  class GetAliasResult
  {
  public:
    AWS_LAMBDA_API GetAliasResult();
    AWS_LAMBDA_API GetAliasResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetAliasResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the alias.</p>
     */
    inline const Aws::String& GetAliasArn() const{ return m_aliasArn; }
    inline void SetAliasArn(const Aws::String& value) { m_aliasArn = value; }
    inline void SetAliasArn(Aws::String&& value) { m_aliasArn = std::move(value); }
    inline void SetAliasArn(const char* value) { m_aliasArn.assign(value); }
    inline GetAliasResult& WithAliasArn(const Aws::String& value) { SetAliasArn(value); return *this;}
    inline GetAliasResult& WithAliasArn(Aws::String&& value) { SetAliasArn(std::move(value)); return *this;}
    inline GetAliasResult& WithAliasArn(const char* value) { SetAliasArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the alias.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline void SetName(const Aws::String& value) { m_name = value; }
    inline void SetName(Aws::String&& value) { m_name = std::move(value); }
    inline void SetName(const char* value) { m_name.assign(value); }
    inline GetAliasResult& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline GetAliasResult& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline GetAliasResult& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The function version that the alias invokes.</p>
     */
    inline const Aws::String& GetFunctionVersion() const{ return m_functionVersion; }
    inline void SetFunctionVersion(const Aws::String& value) { m_functionVersion = value; }
    inline void SetFunctionVersion(Aws::String&& value) { m_functionVersion = std::move(value); }
    inline void SetFunctionVersion(const char* value) { m_functionVersion.assign(value); }
    inline GetAliasResult& WithFunctionVersion(const Aws::String& value) { SetFunctionVersion(value); return *this;}
    inline GetAliasResult& WithFunctionVersion(Aws::String&& value) { SetFunctionVersion(std::move(value)); return *this;}
    inline GetAliasResult& WithFunctionVersion(const char* value) { SetFunctionVersion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A description of the alias.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline void SetDescription(const Aws::String& value) { m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_description.assign(value); }
    inline GetAliasResult& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline GetAliasResult& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline GetAliasResult& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/lambda-traffic-shifting-using-aliases.html">routing
     * configuration</a> of the alias.</p>
     */
    inline const AliasRoutingConfiguration& GetRoutingConfig() const{ return m_routingConfig; }
    inline void SetRoutingConfig(const AliasRoutingConfiguration& value) { m_routingConfig = value; }
    inline void SetRoutingConfig(AliasRoutingConfiguration&& value) { m_routingConfig = std::move(value); }
    inline GetAliasResult& WithRoutingConfig(const AliasRoutingConfiguration& value) { SetRoutingConfig(value); return *this;}
    inline GetAliasResult& WithRoutingConfig(AliasRoutingConfiguration&& value) { SetRoutingConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique identifier that changes when you update the alias.</p>
     */
    inline const Aws::String& GetRevisionId() const{ return m_revisionId; }
    inline void SetRevisionId(const Aws::String& value) { m_revisionId = value; }
    inline void SetRevisionId(Aws::String&& value) { m_revisionId = std::move(value); }
    inline void SetRevisionId(const char* value) { m_revisionId.assign(value); }
    inline GetAliasResult& WithRevisionId(const Aws::String& value) { SetRevisionId(value); return *this;}
    inline GetAliasResult& WithRevisionId(Aws::String&& value) { SetRevisionId(std::move(value)); return *this;}
    inline GetAliasResult& WithRevisionId(const char* value) { SetRevisionId(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetAliasResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetAliasResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetAliasResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_aliasArn;

    Aws::String m_name;

    Aws::String m_functionVersion;

    Aws::String m_description;

    AliasRoutingConfiguration m_routingConfig;

    Aws::String m_revisionId;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
