#include "mongo/unittest/unittest.h"

#include <string>

#include "avro/Compiler.hh"
#include "avro/Decoder.hh"
#include "avro/Encoder.hh"
#include "avro/Exception.hh"
#include "avro/Generic.hh"
#include "avro/GenericDatum.hh"
#include "avro/Stream.hh"
#include "avro/Types.hh"
#include "avro/ValidSchema.hh"
#include "avro/Zigzag.hh"

TEST(AvroTests, BasicSchemaCompilation) {
    std::string schema_str = R"({
        "type": "record",
        "name": "Test",
        "fields": [
            {"name": "id", "type": "int"},
            {"name": "name", "type": "string"}
        ]
    })";

    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schema_str.c_str());
    ASSERT_EQUALS(schema.root()->type(), avro::AVRO_RECORD);
}

TEST(AvroTests, ComplexSchemaTypes) {
    std::string complex_schema = R"({
        "type": "record",
        "name": "ComplexRecord",
        "fields": [
            {"name": "id", "type": "long"},
            {"name": "tags", "type": {"type": "array", "items": "string"}},
            {"name": "metadata", "type": {"type": "map", "values": "string"}},
            {"name": "status", "type": {"type": "enum", "name": "Status", "symbols": ["ACTIVE", "INACTIVE", "PENDING"]}},
            {"name": "optional_field", "type": ["null", "string"], "default": null}
        ]
    })";

    avro::ValidSchema schema = avro::compileJsonSchemaFromString(complex_schema.c_str());
    ASSERT_EQUALS(schema.root()->type(), avro::AVRO_RECORD);
}

TEST(AvroTests, GenericDatumOperations) {
    std::string schema_str = R"({
        "type": "record",
        "name": "Person",
        "fields": [
            {"name": "id", "type": "int"},
            {"name": "name", "type": "string"},
            {"name": "age", "type": "int"}
        ]
    })";

    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schema_str.c_str());

    // Create a generic record
    avro::GenericDatum datum(schema);
    avro::GenericRecord& record = datum.value<avro::GenericRecord>();

    // Set field values
    record.setFieldAt(0, avro::GenericDatum(int32_t(12345)));
    record.setFieldAt(1, avro::GenericDatum(std::string("John Doe")));
    record.setFieldAt(2, avro::GenericDatum(int32_t(30)));

    // Verify field values
    ASSERT_EQUALS(record.fieldAt(0).value<int32_t>(), 12345);
    ASSERT_EQUALS(record.fieldAt(1).value<std::string>(), "John Doe");
    ASSERT_EQUALS(record.fieldAt(2).value<int32_t>(), 30);
}

TEST(AvroTests, BinarySerializationDeserialization) {
    std::string schema_str = R"({
        "type": "record",
        "name": "SimpleRecord",
        "fields": [
            {"name": "id", "type": "int"},
            {"name": "message", "type": "string"}
        ]
    })";

    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schema_str.c_str());

    // Create test data
    avro::GenericDatum datum(schema);
    avro::GenericRecord& record = datum.value<avro::GenericRecord>();
    record.setFieldAt(0, avro::GenericDatum(int32_t(42)));
    record.setFieldAt(1, avro::GenericDatum(std::string("Hello Avro!")));

    // Serialize to binary using GenericWriter
    auto out = avro::memoryOutputStream();
    avro::EncoderPtr encoder = avro::binaryEncoder();
    encoder->init(*out);
    avro::GenericWriter writer(schema, encoder);
    writer.write(datum);

    // Deserialize from binary using GenericReader
    auto in = avro::memoryInputStream(*out);
    avro::DecoderPtr decoder = avro::binaryDecoder();
    decoder->init(*in);
    avro::GenericReader reader(schema, decoder);

    avro::GenericDatum deserialized_datum;
    reader.read(deserialized_datum);

    // Verify deserialized data
    avro::GenericRecord& deserialized_record = deserialized_datum.value<avro::GenericRecord>();
    ASSERT_EQUALS(deserialized_record.fieldAt(0).value<int32_t>(), 42);
    ASSERT_EQUALS(deserialized_record.fieldAt(1).value<std::string>(), "Hello Avro!");
}

TEST(AvroTests, ArrayAndMapTypes) {
    std::string schema_str = R"({
        "type": "record",
        "name": "CollectionRecord",
        "fields": [
            {"name": "tags", "type": {"type": "array", "items": "string"}},
            {"name": "count", "type": "int"}
        ]
    })";

    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schema_str.c_str());

    // Create test data with arrays
    avro::GenericDatum datum(schema);
    avro::GenericRecord& record = datum.value<avro::GenericRecord>();

    // Create array datum directly from schema
    avro::GenericDatum array_datum(schema.root()->leafAt(0));
    avro::GenericArray& tags = array_datum.value<avro::GenericArray>();
    tags.value().push_back(avro::GenericDatum(std::string("tag1")));
    tags.value().push_back(avro::GenericDatum(std::string("tag2")));
    tags.value().push_back(avro::GenericDatum(std::string("tag3")));
    record.setFieldAt(0, array_datum);

    // Set simple field
    record.setFieldAt(1, avro::GenericDatum(int32_t(42)));

    // Verify array data
    const avro::GenericArray& retrieved_tags = record.fieldAt(0).value<avro::GenericArray>();
    ASSERT_EQUALS(retrieved_tags.value().size(), 3U);
    ASSERT_EQUALS(retrieved_tags.value()[0].value<std::string>(), "tag1");

    // Verify simple field
    ASSERT_EQUALS(record.fieldAt(1).value<int32_t>(), 42);
}

TEST(AvroTests, UnionTypes) {
    std::string schema_str = R"({
        "type": "record",
        "name": "UnionRecord",
        "fields": [
            {"name": "optional_string", "type": ["null", "string"]},
            {"name": "simple_field", "type": "int"}
        ]
    })";

    avro::ValidSchema schema = avro::compileJsonSchemaFromString(schema_str.c_str());

    // Create test data with union types
    avro::GenericDatum datum(schema);
    avro::GenericRecord& record = datum.value<avro::GenericRecord>();

    // Create union datum directly from schema and use GenericDatum's union methods
    avro::GenericDatum union_datum(schema.root()->leafAt(0));
    union_datum.selectBranch(1);  // Select string branch (index 1)
    // Set the union value through the GenericDatum interface
    union_datum.value<std::string>() = "Hello Union!";
    record.setFieldAt(0, union_datum);

    // Set simple field
    record.setFieldAt(1, avro::GenericDatum(int32_t(42)));

    // Verify union data using GenericDatum's union methods
    const avro::GenericDatum& retrieved_union = record.fieldAt(0);
    ASSERT_EQUALS(retrieved_union.unionBranch(), 1U);
    ASSERT_EQUALS(retrieved_union.value<std::string>(), "Hello Union!");

    // Verify simple field
    ASSERT_EQUALS(record.fieldAt(1).value<int32_t>(), 42);
}

TEST(AvroTests, ZigzagEncoding) {
    // Test zigzag encoding which is used internally by Avro for efficient integer storage
    int32_t positive = 123;
    int32_t negative = -123;

    uint32_t encoded_positive = avro::encodeZigzag32(positive);
    uint32_t encoded_negative = avro::encodeZigzag32(negative);

    int32_t decoded_positive = avro::decodeZigzag32(encoded_positive);
    int32_t decoded_negative = avro::decodeZigzag32(encoded_negative);

    ASSERT_EQUALS(decoded_positive, positive);
    ASSERT_EQUALS(decoded_negative, negative);
}

TEST(AvroTests, ErrorHandling) {
    // Try to compile an invalid schema - should throw an exception
    std::string invalid_schema = R"({
        "type": "invalid_type",
        "name": "BadSchema"
    })";

    ASSERT_THROWS(avro::compileJsonSchemaFromString(invalid_schema.c_str()), avro::Exception);
}
