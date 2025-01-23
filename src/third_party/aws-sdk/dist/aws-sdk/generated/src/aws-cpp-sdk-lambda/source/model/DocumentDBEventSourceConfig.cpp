/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/DocumentDBEventSourceConfig.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

DocumentDBEventSourceConfig::DocumentDBEventSourceConfig() : 
    m_databaseNameHasBeenSet(false),
    m_collectionNameHasBeenSet(false),
    m_fullDocument(FullDocument::NOT_SET),
    m_fullDocumentHasBeenSet(false)
{
}

DocumentDBEventSourceConfig::DocumentDBEventSourceConfig(JsonView jsonValue)
  : DocumentDBEventSourceConfig()
{
  *this = jsonValue;
}

DocumentDBEventSourceConfig& DocumentDBEventSourceConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("DatabaseName"))
  {
    m_databaseName = jsonValue.GetString("DatabaseName");

    m_databaseNameHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CollectionName"))
  {
    m_collectionName = jsonValue.GetString("CollectionName");

    m_collectionNameHasBeenSet = true;
  }

  if(jsonValue.ValueExists("FullDocument"))
  {
    m_fullDocument = FullDocumentMapper::GetFullDocumentForName(jsonValue.GetString("FullDocument"));

    m_fullDocumentHasBeenSet = true;
  }

  return *this;
}

JsonValue DocumentDBEventSourceConfig::Jsonize() const
{
  JsonValue payload;

  if(m_databaseNameHasBeenSet)
  {
   payload.WithString("DatabaseName", m_databaseName);

  }

  if(m_collectionNameHasBeenSet)
  {
   payload.WithString("CollectionName", m_collectionName);

  }

  if(m_fullDocumentHasBeenSet)
  {
   payload.WithString("FullDocument", FullDocumentMapper::GetNameForFullDocument(m_fullDocument));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
