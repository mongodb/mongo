#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/StlAllocator.h>
#include <aws/crt/Types.h>

struct aws_json_value;
namespace Aws
{
    namespace Crt
    {

        class JsonView;
        /**
         * JSON DOM manipulation class.
         * To read or serialize use @ref View function.
         */
        class AWS_CRT_CPP_API JsonObject
        {
          public:
            /**
             * Constructs empty JSON DOM.
             */
            JsonObject();

            /**
             * Constructs a JSON DOM by parsing the input string.
             * Call WasParseSuccessful() on new object to determine if parse was successful.
             */
            JsonObject(const String &stringToParse);

            /**
             * Construct a deep copy.
             * Prefer using a @ref JsonView if copying is not needed.
             */
            JsonObject(const JsonObject &other);

            /**
             * Move constructor.
             * No copying is performed.
             */
            JsonObject(JsonObject &&other) noexcept;

            ~JsonObject();

            /**
             * Performs a deep copy.
             */
            JsonObject &operator=(const JsonObject &other);

            /**
             * Moves the ownership of the internal JSON DOM of the parameter to the current object.
             * No copying is performed.
             * A DOM currently owned by the object will be freed prior to copying.
             * @warning This will result in invalidating any outstanding views of the current DOM. However, views
             * to the moved-from DOM would still valid.
             */
            JsonObject &operator=(JsonObject &&other) noexcept;

            bool operator==(const JsonObject &other) const;
            bool operator!=(const JsonObject &other) const;

            /**
             * Adds a string to the top level of this node with key.
             */
            JsonObject &WithString(const String &key, const String &value);
            JsonObject &WithString(const char *key, const String &value);

            /**
             * Converts the current JSON node to a string.
             */
            JsonObject &AsString(const String &value);

            /**
             * Adds a bool value with key to the top level of this node.
             */
            JsonObject &WithBool(const String &key, bool value);
            JsonObject &WithBool(const char *key, bool value);

            /**
             * Converts the current JSON node to a bool.
             */
            JsonObject &AsBool(bool value);

            /**
             * Adds a number value at key at the top level of this node.
             * Precision may be lost.
             */
            JsonObject &WithInteger(const String &key, int value);
            JsonObject &WithInteger(const char *key, int value);

            /**
             * Converts the current JSON node to a number.
             * Precision may be lost.
             */
            JsonObject &AsInteger(int value);

            /**
             * Adds a number value at key to the top level of this node.
             * Precision may be lost.
             */
            JsonObject &WithInt64(const String &key, int64_t value);
            JsonObject &WithInt64(const char *key, int64_t value);

            /**
             * Converts the current JSON node to a number.
             * Precision may be lost.
             */
            JsonObject &AsInt64(int64_t value);

            /**
             * Adds a number value at key at the top level of this node.
             */
            JsonObject &WithDouble(const String &key, double value);
            JsonObject &WithDouble(const char *key, double value);

            /**
             * Converts the current JSON node to a number.
             */
            JsonObject &AsDouble(double value);

            /**
             * Adds an array of strings to the top level of this node at key.
             */
            JsonObject &WithArray(const String &key, const Vector<String> &array);
            JsonObject &WithArray(const char *key, const Vector<String> &array);

            /**
             * Adds an array of arbitrary JSON objects to the top level of this node at key.
             * The values in the array parameter will be deep-copied.
             */
            JsonObject &WithArray(const String &key, const Vector<JsonObject> &array);

            /**
             * Adds an array of arbitrary JSON objects to the top level of this node at key.
             * The values in the array parameter will be moved-from.
             */
            JsonObject &WithArray(const String &key, Vector<JsonObject> &&array);

            /**
             * Converts the current JSON node to an array whose values are deep-copied from the array parameter.
             */
            JsonObject &AsArray(const Vector<JsonObject> &array);

            /**
             * Converts the current JSON node to an array whose values are moved from the array parameter.
             */
            JsonObject &AsArray(Vector<JsonObject> &&array);

            /**
             * Sets the current JSON node as null.
             */
            JsonObject &AsNull();

            /**
             * Adds a JSON object to the top level of this node at key.
             * The object parameter is deep-copied.
             */
            JsonObject &WithObject(const String &key, const JsonObject &value);
            JsonObject &WithObject(const char *key, const JsonObject &value);

            /**
             * Adds a JSON object to the top level of this node at key.
             */
            JsonObject &WithObject(const String &key, JsonObject &&value);
            JsonObject &WithObject(const char *key, JsonObject &&value);

            /**
             * Converts the current JSON node to a JSON object by deep-copying the parameter.
             */
            JsonObject &AsObject(const JsonObject &value);

            /**
             * Converts the current JSON node to a JSON object by moving from the parameter.
             */
            JsonObject &AsObject(JsonObject &&value);

            /**
             * Returns true if the last parse request was successful.
             */
            inline bool WasParseSuccessful() const { return m_value != nullptr; }

            /**
             * @deprecated
             */
            const String &GetErrorMessage() const;

            /**
             * Creates a view of this JSON node.
             */
            JsonView View() const;

          private:
            /**
             * Construct a duplicate of this JSON value.
             */
            JsonObject(const aws_json_value *valueToCopy);

            /**
             * Helper for all AsXYZ() functions.
             * Destroys any pre-existing value and takes ownership of new value.
             */
            JsonObject &AsNewValue(aws_json_value *valueToOwn);

            /**
             * Helper for all WithXZY() functions.
             * Take ownership of new value and add at key, replacing any previous value.
             * Converts this node to JSON object if necessary.
             */
            JsonObject &WithNewKeyValue(const char *key, aws_json_value *valueToOwn);

            /**
             * Return new aws_json_value, an array containing duplicates of everything in objectsToCopy.
             */
            static aws_json_value *NewArray(const Vector<JsonObject> &objectsToCopy);

            /**
             * Return new aws_json_value, an array which has taken ownership of everything in objectsToMove
             */
            static aws_json_value *NewArray(Vector<JsonObject> &&objectsToMove);

            aws_json_value *m_value;

            /* Once upon a time each class instance had an m_errorMessage string member,
             * and if parse failed the string would explain why.
             * When we switched json implementations, there was no longer a unique string
             * explaining why parse failed so we dropped that member from the class.
             * To avoid breaking the GetErrorMessage() API, which returns the string by REFERENCE,
             * we now use singletons that are created/destroyed along with library init/cleanup. */
            static std::unique_ptr<String> s_errorMessage;
            static std::unique_ptr<String> s_okMessage;
            static void OnLibraryInit();
            static void OnLibraryCleanup();

            friend class JsonView;
            friend class ApiHandle;
        };

        /**
         * Provides read-only view to an existing JsonObject. This allows lightweight copying without making deep
         * copies of the JsonObject.
         * Note: This class does not extend the lifetime of the given JsonObject. It's your responsibility to ensure
         * the lifetime of the JsonObject is extended beyond the lifetime of its view.
         */
        class AWS_CRT_CPP_API JsonView
        {
          public:
            /* constructors */
            JsonView();
            JsonView(const JsonObject &val);
            JsonView &operator=(const JsonObject &val);

            /**
             * Gets a string from this node by its key.
             */
            String GetString(const String &key) const;
            /**
             * Gets a string from this node by its key.
             */
            String GetString(const char *key) const;

            /**
             * Returns the value of this node as a string.
             * The behavior is undefined if the node is _not_ of type string.
             */
            String AsString() const;

            /**
             * Gets a boolean value from this node by its key.
             */
            bool GetBool(const String &key) const;
            /**
             * Gets a boolean value from this node by its key.
             */
            bool GetBool(const char *key) const;

            /**
             * Returns the value of this node as a boolean.
             */
            bool AsBool() const;

            /**
             * Gets an integer value from this node by its key.
             * The integer is of the same size as an int on the machine.
             */
            int GetInteger(const String &key) const;
            /**
             * Gets an integer value from this node by its key.
             * The integer is of the same size as an int on the machine.
             */
            int GetInteger(const char *key) const;

            /**
             * Returns the value of this node as an int.
             */
            int AsInteger() const;

            /**
             * Gets a 64-bit integer value from this node by its key.
             * The value is 64-bit regardless of the platform/machine.
             */
            int64_t GetInt64(const String &key) const;
            /**
             * Gets a 64-bit integer value from this node by its key.
             * The value is 64-bit regardless of the platform/machine.
             */
            int64_t GetInt64(const char *key) const;

            /**
             * Returns the value of this node as 64-bit integer.
             */
            int64_t AsInt64() const;

            /**
             * Gets a double precision floating-point value from this node by its key.
             */
            double GetDouble(const String &key) const;
            /**
             * Gets a double precision floating-point value from this node by its key.
             */
            double GetDouble(const char *key) const;

            /**
             * Returns the value of this node as a double precision floating-point.
             */
            double AsDouble() const;

            /**
             * Gets an array of JsonView objects from this node by its key.
             */
            Vector<JsonView> GetArray(const String &key) const;
            /**
             * Gets an array of JsonView objects from this node by its key.
             */
            Vector<JsonView> GetArray(const char *key) const;

            /**
             * Returns the value of this node as an array of JsonView objects.
             */
            Vector<JsonView> AsArray() const;

            /**
             * Gets a JsonView object from this node by its key.
             */
            JsonView GetJsonObject(const String &key) const;
            /**
             * Gets a JsonView object from this node by its key.
             */
            JsonView GetJsonObject(const char *key) const;

            JsonObject GetJsonObjectCopy(const String &key) const;

            JsonObject GetJsonObjectCopy(const char *key) const;

            /**
             * Returns the value of this node as a JsonView object.
             */
            JsonView AsObject() const;

            /**
             * Reads all json objects at the top level of this node (does not traverse the tree any further)
             * along with their keys.
             */
            Map<String, JsonView> GetAllObjects() const;

            /**
             * Tests whether a value exists at the current node level for the given key.
             * Returns true if a value has been found and its value is not null, false otherwise.
             */
            bool ValueExists(const String &key) const;
            /**
             * Tests whether a value exists at the current node level for the given key.
             * Returns true if a value has been found and its value is not null, false otherwise.
             */
            bool ValueExists(const char *key) const;

            /**
             * Tests whether a key exists at the current node level.
             */
            bool KeyExists(const String &key) const;
            /**
             * Tests whether a key exists at the current node level.
             */
            bool KeyExists(const char *key) const;

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
             * Tests whether the current value is a number.
             */
            bool IsNumber() const;

            /**
             * Tests whether the current value is a number that can convert to an int64_t without losing precision.
             */
            bool IsIntegerType() const;

            /**
             * Tests whether the current value is a number that will lose precision if converted to an int64_t.
             */
            bool IsFloatingPointType() const;

            /**
             * Tests whether the current value is a JSON array.
             */
            bool IsListType() const;

            /**
             * Tests whether the current value is a JSON null.
             */
            bool IsNull() const;

            /**
             * Writes the current JSON view without whitespace characters starting at the current level to a string.
             * @param treatAsObject if the current value is empty, writes out '{}' rather than an empty string.
             */
            String WriteCompact(bool treatAsObject = true) const;

            /**
             * Writes the current JSON view to a string in a human friendly format.
             * @param treatAsObject if the current value is empty, writes out '{}' rather than an empty string.
             */
            String WriteReadable(bool treatAsObject = true) const;

            /**
             * Creates a deep copy of the JSON value rooted in the current JSON view.
             */
            JsonObject Materialize() const;

          private:
            JsonView(const aws_json_value *val);

            String Write(bool treatAsObject, bool readable) const;

            const aws_json_value *m_value;
        };
    } // namespace Crt
} // namespace Aws
