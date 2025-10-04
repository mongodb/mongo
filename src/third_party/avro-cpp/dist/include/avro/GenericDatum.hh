/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef avro_GenericDatum_hh__
#define avro_GenericDatum_hh__

#include <any>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "LogicalType.hh"
#include "Node.hh"
#include "ValidSchema.hh"

namespace avro {

/**
 * Generic datum which can hold any Avro type. The datum has a type
 * and a value. The type is one of the Avro data types. The C++ type for
 * value corresponds to the Avro type.
 * \li An Avro <tt>null</tt> corresponds to no C++ type. It is illegal to
 * to try to access values for <tt>null</tt>.
 * \li Avro <tt>boolean</tt> maps to C++ <tt>bool</tt>
 * \li Avro <tt>int</tt> maps to C++ <tt>int32_t</tt>.
 * \li Avro <tt>long</tt> maps to C++ <tt>int64_t</tt>.
 * \li Avro <tt>float</tt> maps to C++ <tt>float</tt>.
 * \li Avro <tt>double</tt> maps to C++ <tt>double</tt>.
 * \li Avro <tt>string</tt> maps to C++ <tt>std::string</tt>.
 * \li Avro <tt>bytes</tt> maps to C++ <tt>std::vector&lt;uint_t&gt;</tt>.
 * \li Avro <tt>fixed</tt> maps to C++ class <tt>GenericFixed</tt>.
 * \li Avro <tt>enum</tt> maps to C++ class <tt>GenericEnum</tt>.
 * \li Avro <tt>array</tt> maps to C++ class <tt>GenericArray</tt>.
 * \li Avro <tt>map</tt> maps to C++ class <tt>GenericMap</tt>.
 * \li There is no C++ type corresponding to Avro <tt>union</tt>. The
 * object should have the C++ type corresponding to one of the constituent
 * types of the union.
 *
 */
class AVRO_DECL GenericDatum {
protected:
    Type type_;
    LogicalType logicalType_;
    std::any value_;

    explicit GenericDatum(Type t)
        : type_(t), logicalType_(LogicalType::NONE) {}

    GenericDatum(Type t, LogicalType logicalType)
        : type_(t), logicalType_(logicalType) {}

    template<typename T>
    GenericDatum(Type t, LogicalType logicalType, const T &v)
        : type_(t), logicalType_(logicalType), value_(v) {}

    void init(const NodePtr &schema);

public:
    /**
     * The avro data type this datum holds.
     */
    Type type() const;

    /**
     * The avro logical type that augments the main data type this datum holds.
     */
    LogicalType logicalType() const;

    /**
     * Returns the value held by this datum.
     * T The type for the value. This must correspond to the
     * avro type returned by type().
     */
    template<typename T>
    const T &value() const;

    /**
     * Returns the reference to the value held by this datum, which
     * can be used to change the contents. Please note that only
     * value can be changed, the data type of the value held cannot
     * be changed.
     *
     * T The type for the value. This must correspond to the
     * avro type returned by type().
     */
    template<typename T>
    T &value();

    /**
     * Returns true if and only if this datum is a union.
     */
    bool isUnion() const { return type_ == AVRO_UNION; }

    /**
     * Returns the index of the current branch, if this is a union.
     * \sa isUnion().
     */
    size_t unionBranch() const;

    /**
     * Selects a new branch in the union if this is a union.
     * \sa isUnion().
     */
    void selectBranch(size_t branch);

    /// Makes a new AVRO_NULL datum.
    GenericDatum() : type_(AVRO_NULL), logicalType_(LogicalType::NONE) {}

    /// Makes a new AVRO_BOOL datum whose value is of type bool.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(bool v)
        : type_(AVRO_BOOL), logicalType_(LogicalType::NONE), value_(v) {}

    /// Makes a new AVRO_INT datum whose value is of type int32_t.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(int32_t v)
        : type_(AVRO_INT), logicalType_(LogicalType::NONE), value_(v) {}

    /// Makes a new AVRO_LONG datum whose value is of type int64_t.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(int64_t v)
        : type_(AVRO_LONG), logicalType_(LogicalType::NONE), value_(v) {}

    /// Makes a new AVRO_FLOAT datum whose value is of type float.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(float v)
        : type_(AVRO_FLOAT), logicalType_(LogicalType::NONE), value_(v) {}

    /// Makes a new AVRO_DOUBLE datum whose value is of type double.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(double v)
        : type_(AVRO_DOUBLE), logicalType_(LogicalType::NONE), value_(v) {}

    /// Makes a new AVRO_STRING datum whose value is of type std::string.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(const std::string &v)
        : type_(AVRO_STRING), logicalType_(LogicalType::NONE), value_(v) {}

    /// Makes a new AVRO_BYTES datum whose value is of type
    /// std::vector<uint8_t>.
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(const std::vector<uint8_t> &v) : type_(AVRO_BYTES), logicalType_(LogicalType::NONE), value_(v) {}

    /**
     * Constructs a datum corresponding to the given avro type.
     * The value will the appropriate default corresponding to the
     * data type.
     * \param schema The schema that defines the avro type.
     */
    /// We don't make this explicit constructor because we want to allow automatic conversion
    // NOLINTNEXTLINE(google-explicit-constructor)
    GenericDatum(const NodePtr &schema);

    /**
     * Constructs a datum corresponding to the given avro type and set
     * the value.
     * \param schema The schema that defines the avro type.
     * \param v The value for this type.
     */
    template<typename T>
    GenericDatum(const NodePtr &schema, const T &v) : type_(schema->type()), logicalType_(schema->logicalType()) {
        init(schema);
        *std::any_cast<T>(&value_) = v;
    }

    /**
     * Constructs a datum corresponding to the given avro type.
     * The value will the appropriate default corresponding to the
     * data type.
     * \param schema The schema that defines the avro type.
     */
    explicit GenericDatum(const ValidSchema &schema);
};

/**
 * The base class for all generic type for containers.
 */
class AVRO_DECL GenericContainer {
    NodePtr schema_;
    static void assertType(const NodePtr &schema, Type type);

protected:
    /**
     * Constructs a container corresponding to the given schema.
     */
    GenericContainer(Type type, const NodePtr &s) : schema_(s) {
        assertType(s, type);
    }

public:
    /// Returns the schema for this object
    const NodePtr &schema() const {
        return schema_;
    }
};

/**
 * Generic container for unions.
 */
class AVRO_DECL GenericUnion : public GenericContainer {
    size_t curBranch_;
    GenericDatum datum_;

public:
    /**
     * Constructs a generic union corresponding to the given schema \p schema,
     * and the given value. The schema should be of Avro type union
     * and the value should correspond to one of the branches of the union.
     */
    explicit GenericUnion(const NodePtr &schema) : GenericContainer(AVRO_UNION, schema), curBranch_(schema->leaves()) {
        selectBranch(0);
    }

    /**
     * Returns the index of the current branch.
     */
    size_t currentBranch() const { return curBranch_; }

    /**
     * Selects a new branch. The type for the value is changed accordingly.
     * \param branch The index for the selected branch.
     */
    void selectBranch(size_t branch) {
        if (curBranch_ != branch) {
            datum_ = GenericDatum(schema()->leafAt(branch));
            curBranch_ = branch;
        }
    }

    /**
     * Returns the datum corresponding to the currently selected branch
     * in this union.
     */
    GenericDatum &datum() {
        return datum_;
    }

    /**
     * Returns the datum corresponding to the currently selected branch
     * in this union.
     */
    const GenericDatum &datum() const {
        return datum_;
    }
};

/**
 * The generic container for Avro records.
 */
class AVRO_DECL GenericRecord : public GenericContainer {
    std::vector<GenericDatum> fields_;

public:
    /**
     * Constructs a generic record corresponding to the given schema \p schema,
     * which should be of Avro type record.
     */
    explicit GenericRecord(const NodePtr &schema);

    /**
     * Returns the number of fields in the current record.
     */
    size_t fieldCount() const {
        return fields_.size();
    }

    /**
     * Returns index of the field with the given name \p name
     */
    size_t fieldIndex(const std::string &name) const {
        size_t index = 0;
        if (!schema()->nameIndex(name, index)) {
            throw Exception("Invalid field name: " + name);
        }
        return index;
    }

    /**
     * Returns true if a field with the given name \p name is located in this r
     * false otherwise
     */
    bool hasField(const std::string &name) const {
        size_t index = 0;
        return schema()->nameIndex(name, index);
    }

    /**
     * Returns the field with the given name \p name.
     */
    const GenericDatum &field(const std::string &name) const {
        return fieldAt(fieldIndex(name));
    }

    /**
     * Returns the reference to the field with the given name \p name,
     * which can be used to change the contents.
     */
    GenericDatum &field(const std::string &name) {
        return fieldAt(fieldIndex(name));
    }

    /**
     * Returns the field at the given position \p pos.
     */
    const GenericDatum &fieldAt(size_t pos) const {
        return fields_[pos];
    }

    /**
     * Returns the reference to the field at the given position \p pos,
     * which can be used to change the contents.
     */
    GenericDatum &fieldAt(size_t pos) {
        return fields_[pos];
    }

    /**
     * Replaces the field at the given position \p pos with \p v.
     */
    void setFieldAt(size_t pos, const GenericDatum &v) {
        // assertSameType(v, schema()->leafAt(pos));
        fields_[pos] = v;
    }
};

/**
 * The generic container for Avro arrays.
 */
class AVRO_DECL GenericArray : public GenericContainer {
public:
    /**
     * The contents type for the array.
     */
    typedef std::vector<GenericDatum> Value;

    /**
     * Constructs a generic array corresponding to the given schema \p schema,
     * which should be of Avro type array.
     */
    explicit GenericArray(const NodePtr &schema) : GenericContainer(AVRO_ARRAY, schema) {
    }

    /**
     * Returns the contents of this array.
     */
    const Value &value() const {
        return value_;
    }

    /**
     * Returns the reference to the contents of this array.
     */
    Value &value() {
        return value_;
    }

private:
    Value value_;
};

/**
 * The generic container for Avro maps.
 */
class AVRO_DECL GenericMap : public GenericContainer {
public:
    /**
     * The contents type for the map.
     */
    typedef std::vector<std::pair<std::string, GenericDatum>> Value;

    /**
     * Constructs a generic map corresponding to the given schema \p schema,
     * which should be of Avro type map.
     */
    explicit GenericMap(const NodePtr &schema) : GenericContainer(AVRO_MAP, schema) {
    }

    /**
     * Returns the contents of this map.
     */
    const Value &value() const {
        return value_;
    }

    /**
     * Returns the reference to the contents of this map.
     */
    Value &value() {
        return value_;
    }

private:
    Value value_;
};

/**
 * Generic container for Avro enum.
 */
class AVRO_DECL GenericEnum : public GenericContainer {
    size_t value_;

    static size_t index(const NodePtr &schema, const std::string &symbol) {
        size_t result;
        if (schema->nameIndex(symbol, result)) {
            return result;
        }
        throw Exception("No such symbol");
    }

public:
    /**
     * Constructs a generic enum corresponding to the given schema \p schema,
     * which should be of Avro type enum.
     */
    explicit GenericEnum(const NodePtr &schema) : GenericContainer(AVRO_ENUM, schema), value_(0) {
    }

    GenericEnum(const NodePtr &schema, const std::string &symbol) : GenericContainer(AVRO_ENUM, schema), value_(index(schema, symbol)) {
    }

    /**
     * Returns the symbol corresponding to the cardinal \p n. If the
     * value for \p n is not within the limits an exception is thrown.
     */
    const std::string &symbol(size_t n) {
        if (n < schema()->names()) {
            return schema()->nameAt(n);
        }
        throw Exception("Not as many symbols");
    }

    /**
     * Returns the cardinal for the given symbol \c symbol. If the symbol
     * is not defined for this enum and exception is thrown.
     */
    size_t index(const std::string &symbol) const {
        return index(schema(), symbol);
    }

    /**
     * Set the value for this enum corresponding to the given symbol \c symbol.
     */
    size_t set(const std::string &symbol) {
        return value_ = index(symbol);
    }

    /**
     * Set the value for this enum corresponding to the given cardinal \c n.
     */
    void set(size_t n) {
        if (n < schema()->names()) {
            value_ = n;
            return;
        }
        throw Exception("Not as many symbols");
    }

    /**
     * Returns the cardinal for the current value of this enum.
     */
    size_t value() const {
        return value_;
    }

    /**
     * Returns the symbol for the current value of this enum.
     */
    const std::string &symbol() const {
        return schema()->nameAt(value_);
    }
};

/**
 * Generic container for Avro fixed.
 */
class AVRO_DECL GenericFixed : public GenericContainer {
    std::vector<uint8_t> value_;

public:
    /**
     * Constructs a generic enum corresponding to the given schema \p schema,
     * which should be of Avro type fixed.
     */
    explicit GenericFixed(const NodePtr &schema) : GenericContainer(AVRO_FIXED, schema) {
        value_.resize(schema->fixedSize());
    }

    GenericFixed(const NodePtr &schema, const std::vector<uint8_t> &v);

    /**
     * Returns the contents of this fixed.
     */
    const std::vector<uint8_t> &value() const {
        return value_;
    }

    /**
     * Returns the reference to the contents of this fixed.
     */
    std::vector<uint8_t> &value() {
        return value_;
    }
};

inline Type GenericDatum::type() const {
    return (type_ == AVRO_UNION) ? std::any_cast<GenericUnion>(&value_)->datum().type()
                                 : type_;
}

inline LogicalType GenericDatum::logicalType() const {
    return (type_ == AVRO_UNION) ? std::any_cast<GenericUnion>(&value_)->datum().logicalType()
                                 : logicalType_;
}

template<typename T>
T &GenericDatum::value() {
    return (type_ == AVRO_UNION) ? std::any_cast<GenericUnion>(&value_)->datum().value<T>()
                                 : *std::any_cast<T>(&value_);
}

template<typename T>
const T &GenericDatum::value() const {
    return (type_ == AVRO_UNION) ? std::any_cast<GenericUnion>(&value_)->datum().value<T>()
                                 : *std::any_cast<T>(&value_);
}

inline size_t GenericDatum::unionBranch() const {
    return std::any_cast<GenericUnion>(&value_)->currentBranch();
}

inline void GenericDatum::selectBranch(size_t branch) {
    std::any_cast<GenericUnion>(&value_)->selectBranch(branch);
}

} // namespace avro
#endif // avro_GenericDatum_hh__
