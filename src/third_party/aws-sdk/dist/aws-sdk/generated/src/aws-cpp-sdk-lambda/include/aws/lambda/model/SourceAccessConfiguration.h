/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/SourceAccessType.h>
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
   * <p>To secure and define access to your event source, you can specify the
   * authentication protocol, VPC components, or virtual host.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/SourceAccessConfiguration">AWS
   * API Reference</a></p>
   */
  class SourceAccessConfiguration
  {
  public:
    AWS_LAMBDA_API SourceAccessConfiguration();
    AWS_LAMBDA_API SourceAccessConfiguration(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API SourceAccessConfiguration& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The type of authentication protocol, VPC components, or virtual host for your
     * event source. For example: <code>"Type":"SASL_SCRAM_512_AUTH"</code>.</p> <ul>
     * <li> <p> <code>BASIC_AUTH</code> – (Amazon MQ) The Secrets Manager secret that
     * stores your broker credentials.</p> </li> <li> <p> <code>BASIC_AUTH</code> –
     * (Self-managed Apache Kafka) The Secrets Manager ARN of your secret key used for
     * SASL/PLAIN authentication of your Apache Kafka brokers.</p> </li> <li> <p>
     * <code>VPC_SUBNET</code> – (Self-managed Apache Kafka) The subnets associated
     * with your VPC. Lambda connects to these subnets to fetch data from your
     * self-managed Apache Kafka cluster.</p> </li> <li> <p>
     * <code>VPC_SECURITY_GROUP</code> – (Self-managed Apache Kafka) The VPC security
     * group used to manage access to your self-managed Apache Kafka brokers.</p> </li>
     * <li> <p> <code>SASL_SCRAM_256_AUTH</code> – (Self-managed Apache Kafka) The
     * Secrets Manager ARN of your secret key used for SASL SCRAM-256 authentication of
     * your self-managed Apache Kafka brokers.</p> </li> <li> <p>
     * <code>SASL_SCRAM_512_AUTH</code> – (Amazon MSK, Self-managed Apache Kafka) The
     * Secrets Manager ARN of your secret key used for SASL SCRAM-512 authentication of
     * your self-managed Apache Kafka brokers.</p> </li> <li> <p>
     * <code>VIRTUAL_HOST</code> –- (RabbitMQ) The name of the virtual host in your
     * RabbitMQ broker. Lambda uses this RabbitMQ host as the event source. This
     * property cannot be specified in an UpdateEventSourceMapping API call.</p> </li>
     * <li> <p> <code>CLIENT_CERTIFICATE_TLS_AUTH</code> – (Amazon MSK, self-managed
     * Apache Kafka) The Secrets Manager ARN of your secret key containing the
     * certificate chain (X.509 PEM), private key (PKCS#8 PEM), and private key
     * password (optional) used for mutual TLS authentication of your MSK/Apache Kafka
     * brokers.</p> </li> <li> <p> <code>SERVER_ROOT_CA_CERTIFICATE</code> –
     * (Self-managed Apache Kafka) The Secrets Manager ARN of your secret key
     * containing the root CA certificate (X.509 PEM) used for TLS encryption of your
     * Apache Kafka brokers. </p> </li> </ul>
     */
    inline const SourceAccessType& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const SourceAccessType& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(SourceAccessType&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline SourceAccessConfiguration& WithType(const SourceAccessType& value) { SetType(value); return *this;}
    inline SourceAccessConfiguration& WithType(SourceAccessType&& value) { SetType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The value for your chosen configuration in <code>Type</code>. For example:
     * <code>"URI":
     * "arn:aws:secretsmanager:us-east-1:01234567890:secret:MyBrokerSecretName"</code>.</p>
     */
    inline const Aws::String& GetURI() const{ return m_uRI; }
    inline bool URIHasBeenSet() const { return m_uRIHasBeenSet; }
    inline void SetURI(const Aws::String& value) { m_uRIHasBeenSet = true; m_uRI = value; }
    inline void SetURI(Aws::String&& value) { m_uRIHasBeenSet = true; m_uRI = std::move(value); }
    inline void SetURI(const char* value) { m_uRIHasBeenSet = true; m_uRI.assign(value); }
    inline SourceAccessConfiguration& WithURI(const Aws::String& value) { SetURI(value); return *this;}
    inline SourceAccessConfiguration& WithURI(Aws::String&& value) { SetURI(std::move(value)); return *this;}
    inline SourceAccessConfiguration& WithURI(const char* value) { SetURI(value); return *this;}
    ///@}
  private:

    SourceAccessType m_type;
    bool m_typeHasBeenSet = false;

    Aws::String m_uRI;
    bool m_uRIHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
