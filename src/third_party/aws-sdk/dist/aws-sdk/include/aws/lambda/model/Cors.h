/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
namespace Lambda
{
namespace Model
{

  /**
   * <p>The <a
   * href="https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS">cross-origin
   * resource sharing (CORS)</a> settings for your Lambda function URL. Use CORS to
   * grant access to your function URL from any origin. You can also use CORS to
   * control access for specific HTTP headers and methods in requests to your
   * function URL.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/Cors">AWS API
   * Reference</a></p>
   */
  class Cors
  {
  public:
    AWS_LAMBDA_API Cors();
    AWS_LAMBDA_API Cors(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Cors& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>Whether to allow cookies or other credentials in requests to your function
     * URL. The default is <code>false</code>.</p>
     */
    inline bool GetAllowCredentials() const{ return m_allowCredentials; }
    inline bool AllowCredentialsHasBeenSet() const { return m_allowCredentialsHasBeenSet; }
    inline void SetAllowCredentials(bool value) { m_allowCredentialsHasBeenSet = true; m_allowCredentials = value; }
    inline Cors& WithAllowCredentials(bool value) { SetAllowCredentials(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The HTTP headers that origins can include in requests to your function URL.
     * For example: <code>Date</code>, <code>Keep-Alive</code>,
     * <code>X-Custom-Header</code>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetAllowHeaders() const{ return m_allowHeaders; }
    inline bool AllowHeadersHasBeenSet() const { return m_allowHeadersHasBeenSet; }
    inline void SetAllowHeaders(const Aws::Vector<Aws::String>& value) { m_allowHeadersHasBeenSet = true; m_allowHeaders = value; }
    inline void SetAllowHeaders(Aws::Vector<Aws::String>&& value) { m_allowHeadersHasBeenSet = true; m_allowHeaders = std::move(value); }
    inline Cors& WithAllowHeaders(const Aws::Vector<Aws::String>& value) { SetAllowHeaders(value); return *this;}
    inline Cors& WithAllowHeaders(Aws::Vector<Aws::String>&& value) { SetAllowHeaders(std::move(value)); return *this;}
    inline Cors& AddAllowHeaders(const Aws::String& value) { m_allowHeadersHasBeenSet = true; m_allowHeaders.push_back(value); return *this; }
    inline Cors& AddAllowHeaders(Aws::String&& value) { m_allowHeadersHasBeenSet = true; m_allowHeaders.push_back(std::move(value)); return *this; }
    inline Cors& AddAllowHeaders(const char* value) { m_allowHeadersHasBeenSet = true; m_allowHeaders.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The HTTP methods that are allowed when calling your function URL. For
     * example: <code>GET</code>, <code>POST</code>, <code>DELETE</code>, or the
     * wildcard character (<code>*</code>).</p>
     */
    inline const Aws::Vector<Aws::String>& GetAllowMethods() const{ return m_allowMethods; }
    inline bool AllowMethodsHasBeenSet() const { return m_allowMethodsHasBeenSet; }
    inline void SetAllowMethods(const Aws::Vector<Aws::String>& value) { m_allowMethodsHasBeenSet = true; m_allowMethods = value; }
    inline void SetAllowMethods(Aws::Vector<Aws::String>&& value) { m_allowMethodsHasBeenSet = true; m_allowMethods = std::move(value); }
    inline Cors& WithAllowMethods(const Aws::Vector<Aws::String>& value) { SetAllowMethods(value); return *this;}
    inline Cors& WithAllowMethods(Aws::Vector<Aws::String>&& value) { SetAllowMethods(std::move(value)); return *this;}
    inline Cors& AddAllowMethods(const Aws::String& value) { m_allowMethodsHasBeenSet = true; m_allowMethods.push_back(value); return *this; }
    inline Cors& AddAllowMethods(Aws::String&& value) { m_allowMethodsHasBeenSet = true; m_allowMethods.push_back(std::move(value)); return *this; }
    inline Cors& AddAllowMethods(const char* value) { m_allowMethodsHasBeenSet = true; m_allowMethods.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The origins that can access your function URL. You can list any number of
     * specific origins, separated by a comma. For example:
     * <code>https://www.example.com</code>, <code>http://localhost:60905</code>.</p>
     * <p>Alternatively, you can grant access to all origins using the wildcard
     * character (<code>*</code>).</p>
     */
    inline const Aws::Vector<Aws::String>& GetAllowOrigins() const{ return m_allowOrigins; }
    inline bool AllowOriginsHasBeenSet() const { return m_allowOriginsHasBeenSet; }
    inline void SetAllowOrigins(const Aws::Vector<Aws::String>& value) { m_allowOriginsHasBeenSet = true; m_allowOrigins = value; }
    inline void SetAllowOrigins(Aws::Vector<Aws::String>&& value) { m_allowOriginsHasBeenSet = true; m_allowOrigins = std::move(value); }
    inline Cors& WithAllowOrigins(const Aws::Vector<Aws::String>& value) { SetAllowOrigins(value); return *this;}
    inline Cors& WithAllowOrigins(Aws::Vector<Aws::String>&& value) { SetAllowOrigins(std::move(value)); return *this;}
    inline Cors& AddAllowOrigins(const Aws::String& value) { m_allowOriginsHasBeenSet = true; m_allowOrigins.push_back(value); return *this; }
    inline Cors& AddAllowOrigins(Aws::String&& value) { m_allowOriginsHasBeenSet = true; m_allowOrigins.push_back(std::move(value)); return *this; }
    inline Cors& AddAllowOrigins(const char* value) { m_allowOriginsHasBeenSet = true; m_allowOrigins.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The HTTP headers in your function response that you want to expose to origins
     * that call your function URL. For example: <code>Date</code>,
     * <code>Keep-Alive</code>, <code>X-Custom-Header</code>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetExposeHeaders() const{ return m_exposeHeaders; }
    inline bool ExposeHeadersHasBeenSet() const { return m_exposeHeadersHasBeenSet; }
    inline void SetExposeHeaders(const Aws::Vector<Aws::String>& value) { m_exposeHeadersHasBeenSet = true; m_exposeHeaders = value; }
    inline void SetExposeHeaders(Aws::Vector<Aws::String>&& value) { m_exposeHeadersHasBeenSet = true; m_exposeHeaders = std::move(value); }
    inline Cors& WithExposeHeaders(const Aws::Vector<Aws::String>& value) { SetExposeHeaders(value); return *this;}
    inline Cors& WithExposeHeaders(Aws::Vector<Aws::String>&& value) { SetExposeHeaders(std::move(value)); return *this;}
    inline Cors& AddExposeHeaders(const Aws::String& value) { m_exposeHeadersHasBeenSet = true; m_exposeHeaders.push_back(value); return *this; }
    inline Cors& AddExposeHeaders(Aws::String&& value) { m_exposeHeadersHasBeenSet = true; m_exposeHeaders.push_back(std::move(value)); return *this; }
    inline Cors& AddExposeHeaders(const char* value) { m_exposeHeadersHasBeenSet = true; m_exposeHeaders.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The maximum amount of time, in seconds, that web browsers can cache results
     * of a preflight request. By default, this is set to <code>0</code>, which means
     * that the browser doesn't cache results.</p>
     */
    inline int GetMaxAge() const{ return m_maxAge; }
    inline bool MaxAgeHasBeenSet() const { return m_maxAgeHasBeenSet; }
    inline void SetMaxAge(int value) { m_maxAgeHasBeenSet = true; m_maxAge = value; }
    inline Cors& WithMaxAge(int value) { SetMaxAge(value); return *this;}
    ///@}
  private:

    bool m_allowCredentials;
    bool m_allowCredentialsHasBeenSet = false;

    Aws::Vector<Aws::String> m_allowHeaders;
    bool m_allowHeadersHasBeenSet = false;

    Aws::Vector<Aws::String> m_allowMethods;
    bool m_allowMethodsHasBeenSet = false;

    Aws::Vector<Aws::String> m_allowOrigins;
    bool m_allowOriginsHasBeenSet = false;

    Aws::Vector<Aws::String> m_exposeHeaders;
    bool m_exposeHeadersHasBeenSet = false;

    int m_maxAge;
    bool m_maxAgeHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
