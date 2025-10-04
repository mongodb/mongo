/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/FullDocument.h>
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
   * <p> Specific configuration settings for a DocumentDB event source.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DocumentDBEventSourceConfig">AWS
   * API Reference</a></p>
   */
  class DocumentDBEventSourceConfig
  {
  public:
    AWS_LAMBDA_API DocumentDBEventSourceConfig();
    AWS_LAMBDA_API DocumentDBEventSourceConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API DocumentDBEventSourceConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p> The name of the database to consume within the DocumentDB cluster. </p>
     */
    inline const Aws::String& GetDatabaseName() const{ return m_databaseName; }
    inline bool DatabaseNameHasBeenSet() const { return m_databaseNameHasBeenSet; }
    inline void SetDatabaseName(const Aws::String& value) { m_databaseNameHasBeenSet = true; m_databaseName = value; }
    inline void SetDatabaseName(Aws::String&& value) { m_databaseNameHasBeenSet = true; m_databaseName = std::move(value); }
    inline void SetDatabaseName(const char* value) { m_databaseNameHasBeenSet = true; m_databaseName.assign(value); }
    inline DocumentDBEventSourceConfig& WithDatabaseName(const Aws::String& value) { SetDatabaseName(value); return *this;}
    inline DocumentDBEventSourceConfig& WithDatabaseName(Aws::String&& value) { SetDatabaseName(std::move(value)); return *this;}
    inline DocumentDBEventSourceConfig& WithDatabaseName(const char* value) { SetDatabaseName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The name of the collection to consume within the database. If you do not
     * specify a collection, Lambda consumes all collections. </p>
     */
    inline const Aws::String& GetCollectionName() const{ return m_collectionName; }
    inline bool CollectionNameHasBeenSet() const { return m_collectionNameHasBeenSet; }
    inline void SetCollectionName(const Aws::String& value) { m_collectionNameHasBeenSet = true; m_collectionName = value; }
    inline void SetCollectionName(Aws::String&& value) { m_collectionNameHasBeenSet = true; m_collectionName = std::move(value); }
    inline void SetCollectionName(const char* value) { m_collectionNameHasBeenSet = true; m_collectionName.assign(value); }
    inline DocumentDBEventSourceConfig& WithCollectionName(const Aws::String& value) { SetCollectionName(value); return *this;}
    inline DocumentDBEventSourceConfig& WithCollectionName(Aws::String&& value) { SetCollectionName(std::move(value)); return *this;}
    inline DocumentDBEventSourceConfig& WithCollectionName(const char* value) { SetCollectionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> Determines what DocumentDB sends to your event stream during document update
     * operations. If set to UpdateLookup, DocumentDB sends a delta describing the
     * changes, along with a copy of the entire document. Otherwise, DocumentDB sends
     * only a partial document that contains the changes. </p>
     */
    inline const FullDocument& GetFullDocument() const{ return m_fullDocument; }
    inline bool FullDocumentHasBeenSet() const { return m_fullDocumentHasBeenSet; }
    inline void SetFullDocument(const FullDocument& value) { m_fullDocumentHasBeenSet = true; m_fullDocument = value; }
    inline void SetFullDocument(FullDocument&& value) { m_fullDocumentHasBeenSet = true; m_fullDocument = std::move(value); }
    inline DocumentDBEventSourceConfig& WithFullDocument(const FullDocument& value) { SetFullDocument(value); return *this;}
    inline DocumentDBEventSourceConfig& WithFullDocument(FullDocument&& value) { SetFullDocument(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_databaseName;
    bool m_databaseNameHasBeenSet = false;

    Aws::String m_collectionName;
    bool m_collectionNameHasBeenSet = false;

    FullDocument m_fullDocument;
    bool m_fullDocumentHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
