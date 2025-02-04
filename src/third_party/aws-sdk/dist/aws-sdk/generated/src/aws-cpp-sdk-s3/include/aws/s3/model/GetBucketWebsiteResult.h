/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/RedirectAllRequestsTo.h>
#include <aws/s3/model/IndexDocument.h>
#include <aws/s3/model/ErrorDocument.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/RoutingRule.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{
  class GetBucketWebsiteResult
  {
  public:
    AWS_S3_API GetBucketWebsiteResult();
    AWS_S3_API GetBucketWebsiteResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketWebsiteResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Specifies the redirect behavior of all requests to a website endpoint of an
     * Amazon S3 bucket.</p>
     */
    inline const RedirectAllRequestsTo& GetRedirectAllRequestsTo() const{ return m_redirectAllRequestsTo; }
    inline void SetRedirectAllRequestsTo(const RedirectAllRequestsTo& value) { m_redirectAllRequestsTo = value; }
    inline void SetRedirectAllRequestsTo(RedirectAllRequestsTo&& value) { m_redirectAllRequestsTo = std::move(value); }
    inline GetBucketWebsiteResult& WithRedirectAllRequestsTo(const RedirectAllRequestsTo& value) { SetRedirectAllRequestsTo(value); return *this;}
    inline GetBucketWebsiteResult& WithRedirectAllRequestsTo(RedirectAllRequestsTo&& value) { SetRedirectAllRequestsTo(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the index document for the website (for example
     * <code>index.html</code>).</p>
     */
    inline const IndexDocument& GetIndexDocument() const{ return m_indexDocument; }
    inline void SetIndexDocument(const IndexDocument& value) { m_indexDocument = value; }
    inline void SetIndexDocument(IndexDocument&& value) { m_indexDocument = std::move(value); }
    inline GetBucketWebsiteResult& WithIndexDocument(const IndexDocument& value) { SetIndexDocument(value); return *this;}
    inline GetBucketWebsiteResult& WithIndexDocument(IndexDocument&& value) { SetIndexDocument(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The object key name of the website error document to use for 4XX class
     * errors.</p>
     */
    inline const ErrorDocument& GetErrorDocument() const{ return m_errorDocument; }
    inline void SetErrorDocument(const ErrorDocument& value) { m_errorDocument = value; }
    inline void SetErrorDocument(ErrorDocument&& value) { m_errorDocument = std::move(value); }
    inline GetBucketWebsiteResult& WithErrorDocument(const ErrorDocument& value) { SetErrorDocument(value); return *this;}
    inline GetBucketWebsiteResult& WithErrorDocument(ErrorDocument&& value) { SetErrorDocument(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Rules that define when a redirect is applied and the redirect behavior.</p>
     */
    inline const Aws::Vector<RoutingRule>& GetRoutingRules() const{ return m_routingRules; }
    inline void SetRoutingRules(const Aws::Vector<RoutingRule>& value) { m_routingRules = value; }
    inline void SetRoutingRules(Aws::Vector<RoutingRule>&& value) { m_routingRules = std::move(value); }
    inline GetBucketWebsiteResult& WithRoutingRules(const Aws::Vector<RoutingRule>& value) { SetRoutingRules(value); return *this;}
    inline GetBucketWebsiteResult& WithRoutingRules(Aws::Vector<RoutingRule>&& value) { SetRoutingRules(std::move(value)); return *this;}
    inline GetBucketWebsiteResult& AddRoutingRules(const RoutingRule& value) { m_routingRules.push_back(value); return *this; }
    inline GetBucketWebsiteResult& AddRoutingRules(RoutingRule&& value) { m_routingRules.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketWebsiteResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketWebsiteResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketWebsiteResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    RedirectAllRequestsTo m_redirectAllRequestsTo;

    IndexDocument m_indexDocument;

    ErrorDocument m_errorDocument;

    Aws::Vector<RoutingRule> m_routingRules;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
