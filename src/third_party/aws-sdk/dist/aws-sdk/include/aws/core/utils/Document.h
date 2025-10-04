/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/external/cjson/cJSON.h>

#include <utility>

namespace Aws
{
    namespace Utils
    {
        namespace Json
        {
            class JsonValue;
            class JsonView;
        }

        class DocumentView;
        /**
         * A Document type represents open content that is serialized using the same format as its surroundings and requires no additional encoding or escaping.
         * Document types provide a JSON "view" of data regardless of the underlying protocol. This normalized JSON interface makes document types protocol-agnostic.
         * Clients can use the data stored in a document without prior knowledge of the protocol by interacting with the normalized JSON view of the data.
         * Document types are only initially supported in JSON protocol, so it's identical to Aws::Utils::Json::JsonValue and Aws::Utils::Json::JsonView at this moment.
         */
        class AWS_CORE_API Document
        {
        public:
            /**
             * Constructs empty Document.
             */
            Document();

            /**
             * Constructs a Document by parsing the input string.
             */
            Document(const Aws::String& value);

            /**
             * Constructs a Document by parsing the text in the input stream.
             */
            Document(Aws::IStream& istream);

            /**
             * Performs a deep copy of the Document parameter.
             * Prefer using a @ref DocumentView if copying is not needed.
             */
            Document(const Document& value);

            /**
             * Moves the ownership of the internal Document.
             * No copying is performed.
             */
            Document(Document&& value);

            /**
             * Performs a deep copy of the JsonView parameter.
             */
            Document(const Json::JsonView& view);

            ~Document();

            /**
             * Performs a deep copy of the Document parameter.
             */
            Document& operator=(const Document& other);

            /**
             * Performs a deep copy of the JsonView parameter.
             */
            Document& operator=(const Json::JsonView& view);

            /**
             * Moves the ownership of the internal Document of the parameter to the current object.
             * No copying is performed.
             * A Document currently owned by the object will be freed prior to copying.
             * @warning This will result in invalidating any outstanding views of the current Document. However, views
             * to the moved-from Document would still valid.
             */
            Document& operator=(Document&& other);

            bool operator==(const Document& other) const;
            bool operator!=(const Document& other) const;

            /**
             * Adds a string to the top level of this Document with key.
             */
            Document& WithString(const Aws::String& key, const Aws::String& value);
            Document& WithString(const char* key, const Aws::String& value);
            /**
             * Converts the current Document to a string.
             */
            Document& AsString(const Aws::String& value);

            /**
             * Adds a bool value with key to the top level of this Document.
             */
            Document& WithBool(const Aws::String& key, bool value);
            Document& WithBool(const char* key, bool value);
            /**
             * Converts the current Document to a bool.
             */
            Document& AsBool(bool value);

            /**
             * Adds an integer value at key at the top level of this Document.
             */
            Document& WithInteger(const Aws::String& key, int value);
            Document& WithInteger(const char* key, int value);
            /**
             * Converts the current Document to an integer.
             */
            Document& AsInteger(int value);

            /**
             * Adds a 64-bit integer value at key to the top level of this Document.
             */
            Document& WithInt64(const Aws::String& key, long long value);
            Document& WithInt64(const char* key, long long value);
            /**
             * Converts the current Document to a 64-bit integer.
             */
            Document& AsInt64(long long value);

            /**
             * Adds a double value at key at the top level of this Document.
             */
            Document& WithDouble(const Aws::String& key, double value);
            Document& WithDouble(const char* key, double value);
            /**
             * Converts the current Document to a double.
             */
            Document& AsDouble(double value);

            /**
             * Adds an array of strings to the top level of this Document at key.
             */
            Document& WithArray(const Aws::String& key, const Array<Aws::String>& array);
            Document& WithArray(const char* key, const Array<Aws::String>& array);
            /**
             * Adds an array of arbitrary Document objects to the top level of this Document at key.
             * The values in the array parameter will be deep-copied.
             */
            Document& WithArray(const Aws::String& key, const Array<Document>& array);
            /**
             * Adds an array of arbitrary Document objects to the top level of this Document at key.
             * The values in the array parameter will be moved-from.
             */
            Document& WithArray(const Aws::String& key, Array<Document>&& array);
            /**
             * Converts the current Document to an array whose values are deep-copied from the array parameter.
             */
            Document& AsArray(const Array<Document>& array);
            /**
             * Converts the current Document to an array whose values are moved from the array parameter.
             */
            Document& AsArray(Array<Document>&& array);

            /**
             * Adds a Document object to the top level of this Document at key.
             * The object parameter is deep-copied.
             */
            Document& WithObject(const Aws::String& key, const Document& value);
            Document& WithObject(const char* key, const Document& value);
            /**
             * Adds a Document object to the top level of this Document at key.
             */
            Document& WithObject(const Aws::String& key, Document&& value);
            Document& WithObject(const char* key, Document&& value);
            /**
             * Converts the current Document to a Document object by deep-copying the parameter.
             */
            Document& AsObject(const Document& value);
            /**
             * Converts the current Document to a Document object by moving from the parameter.
             */
            Document& AsObject(Document&& value);

            /**
             * Returns true if the last parse request was successful. If this returns false,
             * you can call GetErrorMessage() to find the cause.
             */
            inline bool WasParseSuccessful() const
            {
                return m_wasParseSuccessful;
            }

            /**
             * Returns the last error message from a failed parse attempt. Returns empty string if no error.
             */
            inline const Aws::String& GetErrorMessage() const
            {
                return m_errorMessage;
            }

            /**
             * Creates a view from the current Document.
             */
            DocumentView View() const;

        private:
            void Destroy();
            Document(cJSON* value);
            cJSON* m_json;
            bool m_wasParseSuccessful;
            Aws::String m_errorMessage;
            friend DocumentView;
        };

        /**
         * Provides read-only view to an existing Document. This allows lightweight copying without making deep
         * copies of the Document.
         * Note: This class does not extend the lifetime of the given Document. It's your responsibility to ensure
         * the lifetime of the Document is extended beyond the lifetime of its view.
         */
        class AWS_CORE_API DocumentView
        {
        public:
            /* constructors */
            DocumentView();
            DocumentView(const Document& value);
            DocumentView& operator=(const Document& value);

            /**
             * Gets a string from this Document by its key.
             */
            Aws::String GetString(const Aws::String& key) const;
            /**
             * Returns the value of this Document as a string.
             */
            Aws::String AsString() const;
            /**
             * Tests whether the current value is a string.
             */
            bool IsString() const;

            /**
             * Gets a boolean value from this Document by its key.
             */
            bool GetBool(const Aws::String& key) const;
            /**
             * Returns the value of this Document as a boolean.
             */
            bool AsBool() const;
            /**
             * Tests whether the current value is a boolean.
             */
            bool IsBool() const;

            /**
             * Gets an integer value from this Document by its key.
             * The integer is of the same size as an int on the machine.
             */
            int GetInteger(const Aws::String& key) const;
            /**
             * Returns the value of this Document as an int.
             */
            int AsInteger() const;
            /**
             * Tests whether the current value is an int or int64_t.
             * Returns false if the value is floating-point.
             */
            bool IsIntegerType() const;

            /**
             * Converts the current Document to a 64-bit integer.
             */
            Document& AsInt64(long long value);
            /**
             * Gets a 64-bit integer value from this Document by its key.
             * The value is 64-bit regardless of the platform/machine.
             */
            int64_t GetInt64(const Aws::String& key) const;
            /**
             * Returns the value of this Document as 64-bit integer.
             */
            int64_t AsInt64() const;

            /**
             * Gets a double precision floating-point value from this Document by its key.
             */
            double GetDouble(const Aws::String& key) const;
            /**
             * Returns the value of this Document as a double precision floating-point.
             */
            double AsDouble() const;
            /**
             * Tests whether the current value is a floating-point.
             */
            bool IsFloatingPointType() const;

            /**
             * Gets an array of DocumentView objects from this Document by its key.
             */
            Array<DocumentView> GetArray(const Aws::String& key) const;
            /**
             * Returns the value of this Document as an array of DocumentView objects.
             */
            Array<DocumentView> AsArray() const;
            /**
             * Tests whether the current value is a Document array.
             */
            bool IsListType() const;

            /**
             * Gets a DocumentView object from this Document by its key.
             */
            DocumentView GetObject(const Aws::String& key) const;
            /**
             * Returns the value of this Document as a DocumentView object.
             */
            DocumentView AsObject() const;
            /**
             * Reads all Document objects at the top level of this Document (does not traverse the tree any further)
             * along with their keys.
             */
            Aws::Map<Aws::String, DocumentView> GetAllObjects() const;
            /**
             * Tests whether the current value is a Document object.
             */
            bool IsObject() const;

            /**
             * Tests whether the current value is NULL.
             */
            bool IsNull() const;

            /**
             * Tests whether a value exists at the current Document level for the given key.
             * Returns true if a value has been found and its value is not null, false otherwise.
             */
            bool ValueExists(const Aws::String& key) const;

            /**
             * Tests whether a key exists at the current Document level.
             */
            bool KeyExists(const Aws::String& key) const;

            /**
             * Writes the current Document view without whitespace characters starting at the current level to a string.
             */
            Aws::String WriteCompact() const;

            /**
             * Writes the current Document view to a string in a human friendly format.
             */
            Aws::String WriteReadable() const;

            /**
             * Creates a deep copy of the JSON value rooted in the current JSON view.
             */
            Document Materialize() const;
        private:
            DocumentView(cJSON* value);
            DocumentView& operator=(cJSON* value);
            cJSON* m_json;
            friend Aws::Utils::Json::JsonValue;
        };

    } // namespace Utils
} // namespace Aws
