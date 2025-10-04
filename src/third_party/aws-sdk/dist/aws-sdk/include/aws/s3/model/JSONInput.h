/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/JSONType.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>Specifies JSON as object's input serialization format.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/JSONInput">AWS API
   * Reference</a></p>
   */
  class JSONInput
  {
  public:
    AWS_S3_API JSONInput();
    AWS_S3_API JSONInput(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API JSONInput& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The type of JSON. Valid values: Document, Lines.</p>
     */
    inline const JSONType& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const JSONType& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(JSONType&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline JSONInput& WithType(const JSONType& value) { SetType(value); return *this;}
    inline JSONInput& WithType(JSONType&& value) { SetType(std::move(value)); return *this;}
    ///@}
  private:

    JSONType m_type;
    bool m_typeHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
