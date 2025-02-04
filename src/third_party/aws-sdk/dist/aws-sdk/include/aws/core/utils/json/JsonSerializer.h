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
        class Document;
        class DocumentView;

        namespace Json
        {
            class JsonView;
            /**
             * JSON DOM manipulation class.
             * To read or serialize use @ref View function.
             */
            class AWS_CORE_API JsonValue
            {
            public:
                /**
                 * Constructs empty JSON DOM.
                 */
                JsonValue();

                /**
                 * Constructs a JSON DOM by parsing the input string.
                 */
                JsonValue(const Aws::String& value);

                /**
                 * Constructs a JSON DOM by parsing the text in the input stream.
                 */
                JsonValue(Aws::IStream& istream);

                /**
                 * Performs a deep copy of the JSON DOM parameter.
                 * Prefer using a @ref JsonView if copying is not needed.
                 */
                JsonValue(const JsonValue& value);

                /**
                 * Moves the ownership of the internal JSON DOM.
                 * No copying is performed.
                 */
                JsonValue(JsonValue&& value);

                /**
                 * Performs a deep copy of the Document parameter.
                 */
                JsonValue(const Aws::Utils::DocumentView& value);

                ~JsonValue();

                /**
                 * Performs a deep copy of the JSON DOM parameter.
                 */
                JsonValue& operator=(const JsonValue& other);

                /**
                 * Moves the ownership of the internal JSON DOM of the parameter to the current object.
                 * No copying is performed.
                 * A DOM currently owned by the object will be freed prior to copying.
                 * @warning This will result in invalidating any outstanding views of the current DOM. However, views
                 * to the moved-from DOM would still valid.
                 */
                JsonValue& operator=(JsonValue&& other);

                /**
                 * Performs a deep copy of the Document parameter.
                 */
                JsonValue& operator=(const Aws::Utils::DocumentView& value);

                bool operator==(const JsonValue& other) const;
                bool operator!=(const JsonValue& other) const;

                /**
                 * Adds a string to the top level of this node with key.
                 */
                JsonValue& WithString(const Aws::String& key, const Aws::String& value);
                JsonValue& WithString(const char* key, const Aws::String& value);

                /**
                 * Converts the current JSON node to a string.
                 */
                JsonValue& AsString(const Aws::String& value);

                /**
                 * Adds a bool value with key to the top level of this node.
                 */
                JsonValue& WithBool(const Aws::String& key, bool value);
                JsonValue& WithBool(const char* key, bool value);

                /**
                 * Converts the current JSON node to a bool.
                 */
                JsonValue& AsBool(bool value);

                /**
                 * Adds an integer value at key at the top level of this node.
                 */
                JsonValue& WithInteger(const Aws::String& key, int value);
                JsonValue& WithInteger(const char* key, int value);

                /**
                 * Converts the current JSON node to an integer.
                 */
                JsonValue& AsInteger(int value);

                /**
                 * Adds a 64-bit integer value at key to the top level of this node.
                 */
                JsonValue& WithInt64(const Aws::String& key, long long value);
                JsonValue& WithInt64(const char* key, long long value);

                /**
                 * Converts the current JSON node to a 64-bit integer.
                 */
                JsonValue& AsInt64(long long value);

                /**
                 * Adds a double value at key at the top level of this node.
                 */
                JsonValue& WithDouble(const Aws::String& key, double value);
                JsonValue& WithDouble(const char* key, double value);

                /**
                 * Converts the current JSON node to a double.
                 */
                JsonValue& AsDouble(double value);

                /**
                 * Adds an array of strings to the top level of this node at key.
                 */
                JsonValue& WithArray(const Aws::String& key, const Array<Aws::String>& array);
                JsonValue& WithArray(const char* key, const Array<Aws::String>& array);

                /**
                 * Adds an array of arbitrary JSON objects to the top level of this node at key.
                 * The values in the array parameter will be deep-copied.
                 */
                JsonValue& WithArray(const Aws::String& key, const Array<JsonValue>& array);

                /**
                 * Adds an array of arbitrary JSON objects to the top level of this node at key.
                 * The values in the array parameter will be moved-from.
                 */
                JsonValue& WithArray(const Aws::String& key, Array<JsonValue>&& array);

                /**
                 * Converts the current JSON node to an array whose values are deep-copied from the array parameter.
                 */
                JsonValue& AsArray(const Array<JsonValue>& array);

                /**
                 * Converts the current JSON node to an array whose values are moved from the array parameter.
                 */
                JsonValue& AsArray(Array<JsonValue>&& array);

                /**
                 * Adds a JSON object to the top level of this node at key.
                 * The object parameter is deep-copied.
                 */
                JsonValue& WithObject(const Aws::String& key, const JsonValue& value);
                JsonValue& WithObject(const char* key, const JsonValue& value);

                /**
                 * Adds a JSON object to the top level of this node at key.
                 */
                JsonValue& WithObject(const Aws::String& key, JsonValue&& value);
                JsonValue& WithObject(const char* key, JsonValue&& value);

                /**
                 * Converts the current JSON node to a JSON object by deep-copying the parameter.
                 */
                JsonValue& AsObject(const JsonValue& value);

                /**
                 * Converts the current JSON node to a JSON object by moving from the parameter.
                 */
                JsonValue& AsObject(JsonValue&& value);

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
                 * Creates a view from the current root JSON node.
                 */
                JsonView View() const;

            private:
                void Destroy();
                JsonValue(cJSON* value);
                cJSON* m_value;
                bool m_wasParseSuccessful;
                Aws::String m_errorMessage;
                friend class JsonView;
            };

            /**
             * Provides read-only view to an existing JsonValue. This allows lightweight copying without making deep
             * copies of the JsonValue.
             * Note: This class does not extend the lifetime of the given JsonValue. It's your responsibility to ensure
             * the lifetime of the JsonValue is extended beyond the lifetime of its view.
             */
            class AWS_CORE_API JsonView
            {
            public:
                /* constructors */
                JsonView();
                JsonView(const JsonValue& v);
                JsonView& operator=(const JsonValue& v);

                /**
                 * Gets a string from this node by its key.
                 */
                Aws::String GetString(const Aws::String& key) const;

                /**
                 * Returns the value of this node as a string.
                 * The behavior is undefined if the node is _not_ of type string.
                 */
                Aws::String AsString() const;

                /**
                 * Gets a boolean value from this node by its key.
                 */
                bool GetBool(const Aws::String& key) const;

                /**
                 * Returns the value of this node as a boolean.
                 */
                bool AsBool() const;

                /**
                 * Gets an integer value from this node by its key.
                 * The integer is of the same size as an int on the machine.
                 */
                int GetInteger(const Aws::String& key) const;

                /**
                 * Returns the value of this node as an int.
                 */
                int AsInteger() const;

                /**
                 * Gets a 64-bit integer value from this node by its key.
                 * The value is 64-bit regardless of the platform/machine.
                 */
                int64_t GetInt64(const Aws::String& key) const;

                /**
                 * Returns the value of this node as 64-bit integer.
                 */
                int64_t AsInt64() const;

                /**
                 * Gets a double precision floating-point value from this node by its key.
                 */
                double GetDouble(const Aws::String& key) const;

                /**
                 * Returns the value of this node as a double precision floating-point.
                 */
                double AsDouble() const;

                /**
                 * Gets an array of JsonView objects from this node by its key.
                 */
                Array<JsonView> GetArray(const Aws::String& key) const;

                /**
                 * Returns the value of this node as an array of JsonView objects.
                 */
                Array<JsonView> AsArray() const;

                /**
                 * Gets a JsonView object from this node by its key.
                 */
                JsonView GetObject(const Aws::String& key) const;

                /**
                 * Returns the value of this node as a JsonView object.
                 */
                JsonView AsObject() const;

                /**
                 * Reads all json objects at the top level of this node (does not traverse the tree any further)
                 * along with their keys.
                 */
                Aws::Map<Aws::String, JsonView> GetAllObjects() const;

                /**
                 * Tests whether a value exists at the current node level for the given key.
                 * Returns true if a value has been found and its value is not null, false otherwise.
                 */
                bool ValueExists(const Aws::String& key) const;

                /**
                 * Tests whether a key exists at the current node level.
                 */
                bool KeyExists(const Aws::String& key) const;

                /**
                 * Tests whether the current value is a JSON object.
                 */
                bool IsObject() const;

                /**
                 * Tests whether the current value is a boolean.
                 */
                bool IsBool() const;

                /**
                 * Tests whether the current value is a string.
                 */
                bool IsString() const;

                /**
                 * Tests whether the current value is an int or int64_t.
                 * Returns false if the value is floating-point.
                 */
                bool IsIntegerType() const;

                /**
                 * Tests whether the current value is a floating-point.
                 */
                bool IsFloatingPointType() const;

                /**
                 * Tests whether the current value is a JSON array.
                 */
                bool IsListType() const;

                /**
                 * Tests whether the current value is NULL.
                 */
                bool IsNull() const;

                /**
                 * Writes the current JSON view without whitespace characters starting at the current level to a string.
                 * @param treatAsObject if the current value is empty, writes out '{}' rather than an empty string.
                 */
                Aws::String WriteCompact(bool treatAsObject = true) const;

                /**
                 * Writes the current JSON view to a string in a human friendly format.
                 * @param treatAsObject if the current value is empty, writes out '{}' rather than an empty string.
                 */
                Aws::String WriteReadable(bool treatAsObject = true) const;

                /**
                 * Creates a deep copy of the JSON value rooted in the current JSON view.
                 */
                JsonValue Materialize() const;

            private:
                JsonView(cJSON* val);
                JsonView& operator=(cJSON* val);
                cJSON* m_value;
                friend class Aws::Utils::Document;
            };

        } // namespace Json
    } // namespace Utils
} // namespace Aws

