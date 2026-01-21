// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/common/internal/xml_wrapper.hpp"

#include <azure/core/platform.hpp>

#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

#if defined(AZ_PLATFORM_WINDOWS)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <webservices.h>
#else
#include <libxml/xmlreader.h>
#include <libxml/xmlwriter.h>
#endif

namespace Azure { namespace Storage { namespace _internal {

#if defined(AZ_PLATFORM_WINDOWS)

  void XmlGlobalInitialize() {}
  void XmlGlobalDeinitialize() {}

  struct XmlReader::XmlReaderContext
  {
    XmlReaderContext()
    {
      HRESULT ret = WsCreateError(nullptr, 0, &error);
      if (ret != NO_ERROR)
      {
        throw std::runtime_error("Failed to initialize xml reader.");
      }
      ret = WsCreateReader(nullptr, 0, &reader, error);
      if (ret != NO_ERROR)
      {
        WsFreeError(error);
        throw std::runtime_error("Failed to initialize xml reader.");
      }
    }
    XmlReaderContext(const XmlReaderContext&) = delete;
    XmlReaderContext& operator=(const XmlReaderContext&) = delete;
    ~XmlReaderContext()
    {
      WsFreeReader(reader);
      WsFreeError(error);
    }

    WS_XML_READER* reader = nullptr;
    WS_ERROR* error = nullptr;
    bool readingAttributes = false;
    ULONG attributeIndex = 0;
    const WS_XML_ELEMENT_NODE* attributeElementNode = nullptr;
  };

  XmlReader::XmlReader(const char* data, size_t length)
  {
    if (length > static_cast<size_t>((std::numeric_limits<ULONG>::max)()))
    {
      throw std::runtime_error("Xml data too big.");
    }

    auto context = std::make_unique<XmlReaderContext>();

    WS_XML_READER_BUFFER_INPUT bufferInput;
    ZeroMemory(&bufferInput, sizeof(bufferInput));
    bufferInput.input.inputType = WS_XML_READER_INPUT_TYPE_BUFFER;
    bufferInput.encodedData = const_cast<char*>(data);
    bufferInput.encodedDataSize = static_cast<ULONG>(length);
    WS_XML_READER_TEXT_ENCODING textEncoding;
    ZeroMemory(&textEncoding, sizeof(textEncoding));
    textEncoding.encoding.encodingType = WS_XML_READER_ENCODING_TYPE_TEXT;
    textEncoding.charSet = WS_CHARSET_AUTO;
    HRESULT ret = WsSetInput(
        context->reader, &textEncoding.encoding, &bufferInput.input, nullptr, 0, context->error);
    if (ret != S_OK)
    {
      throw std::runtime_error("Failed to initialize xml reader.");
    }

    WS_CHARSET charSet;
    ret = WsGetReaderProperty(
        context->reader, WS_XML_READER_PROPERTY_CHARSET, &charSet, sizeof(charSet), context->error);
    if (ret != S_OK)
    {
      throw std::runtime_error("Failed to get xml encoding.");
    }
    if (charSet != WS_CHARSET_UTF8)
    {
      throw std::runtime_error("Unsupported xml encoding.");
    }

    m_context = std::move(context);
  }

  XmlReader::~XmlReader() {}

  XmlNode XmlReader::Read()
  {
    auto& context = m_context;

    auto moveToNext = [&]() {
      HRESULT ret = WsReadNode(context->reader, context->error);
      if (!SUCCEEDED(ret))
      {
        throw std::runtime_error("Failed to parse xml.");
      }
    };

    if (context->readingAttributes)
    {
      const WS_XML_ATTRIBUTE* attribute
          = context->attributeElementNode->attributes[context->attributeIndex];

      std::string name(
          reinterpret_cast<const char*>(attribute->localName->bytes), attribute->localName->length);

      if (attribute->value->textType != WS_XML_TEXT_TYPE_UTF8)
      {
        throw std::runtime_error("Unsupported xml encoding.");
      }

      const WS_XML_UTF8_TEXT* utf8Text
          = reinterpret_cast<const WS_XML_UTF8_TEXT*>(attribute->value);
      std::string value(
          reinterpret_cast<const char*>(utf8Text->value.bytes), utf8Text->value.length);

      if (++context->attributeIndex == context->attributeElementNode->attributeCount)
      {
        moveToNext();
        context->readingAttributes = false;
        context->attributeElementNode = nullptr;
        context->attributeIndex = 0;
      }

      return XmlNode{XmlNodeType::Attribute, std::move(name), std::move(value)};
    }

    const WS_XML_NODE* node;
    HRESULT ret = WsGetReaderNode(context->reader, &node, context->error);
    if (!SUCCEEDED(ret))
    {
      throw std::runtime_error("Failed to parse xml.");
    }
    switch (node->nodeType)
    {
      case WS_XML_NODE_TYPE_ELEMENT: {
        const WS_XML_ELEMENT_NODE* elementNode = reinterpret_cast<const WS_XML_ELEMENT_NODE*>(node);
        std::string name(
            reinterpret_cast<const char*>(elementNode->localName->bytes),
            elementNode->localName->length);

        if (elementNode->attributeCount != 0)
        {
          context->readingAttributes = true;
          context->attributeElementNode = elementNode;
          context->attributeIndex = 0;
        }
        else
        {
          moveToNext();
        }

        return XmlNode{XmlNodeType::StartTag, std::move(name)};
      }
      case WS_XML_NODE_TYPE_TEXT: {
        std::string value;
        while (true)
        {
          const WS_XML_TEXT_NODE* textNode = (const WS_XML_TEXT_NODE*)node;
          if (textNode->text->textType != WS_XML_TEXT_TYPE_UTF8)
          {
            throw std::runtime_error("Unsupported xml encoding.");
          }
          const WS_XML_UTF8_TEXT* utf8Text
              = reinterpret_cast<const WS_XML_UTF8_TEXT*>(textNode->text);
          value += std::string(
              reinterpret_cast<const char*>(utf8Text->value.bytes), utf8Text->value.length);

          moveToNext();
          ret = WsGetReaderNode(context->reader, &node, context->error);
          if (!SUCCEEDED(ret))
          {
            throw std::runtime_error("Failed to parse xml.");
          }
          if (node->nodeType != WS_XML_NODE_TYPE_TEXT)
          {
            break;
          }
        }
        return XmlNode{XmlNodeType::Text, std::string(), std::move(value)};
      }
      case WS_XML_NODE_TYPE_END_ELEMENT:
        moveToNext();
        return XmlNode{XmlNodeType::EndTag};
      case WS_XML_NODE_TYPE_EOF:
        return XmlNode{XmlNodeType::End};
      case WS_XML_NODE_TYPE_CDATA:
      case WS_XML_NODE_TYPE_END_CDATA:
      case WS_XML_NODE_TYPE_COMMENT:
      case WS_XML_NODE_TYPE_BOF:
        moveToNext();
        return Read();
      default:
        throw std::runtime_error(
            "Unknown type " + std::to_string(node->nodeType) + " while parsing xml.");
    }
  }

  struct XmlWriter::XmlWriterContext
  {
    XmlWriterContext()
    {
      HRESULT ret = WsCreateError(nullptr, 0, &error);
      if (ret != NO_ERROR)
      {
        throw std::runtime_error("Failed to initialize xml writer.");
      }
      ret = WsCreateWriter(nullptr, 0, &writer, error);
      if (ret != NO_ERROR)
      {
        WsFreeError(error);
        throw std::runtime_error("Failed to initialize xml writer.");
      }
      ret = WsCreateHeap(1024 * 1024 * 1024, 512, nullptr, 0, &heap, error);
      if (ret != NO_ERROR)
      {
        WsFreeWriter(writer);
        WsFreeError(error);
        throw std::runtime_error("Failed to initialize xml writer.");
      }
    }
    XmlWriterContext(const XmlWriterContext&) = delete;
    XmlWriterContext& operator=(const XmlWriterContext&) = delete;
    ~XmlWriterContext()
    {
      WsFreeHeap(heap);
      WsFreeWriter(writer);
      WsFreeError(error);
    }

    WS_XML_WRITER* writer = nullptr;
    WS_ERROR* error = nullptr;
    WS_HEAP* heap = nullptr;
    WS_XML_BUFFER* buffer = nullptr;
  };

  XmlWriter::XmlWriter()
  {
    auto context = std::make_unique<XmlWriterContext>();

    HRESULT ret = WsCreateXmlBuffer(context->heap, nullptr, 0, &context->buffer, context->error);
    if (ret != NO_ERROR)
    {
      throw std::runtime_error("Failed to initialize xml writer.");
    }

    ret = WsSetOutputToBuffer(context->writer, context->buffer, nullptr, 0, context->error);
    if (ret != NO_ERROR)
    {
      throw std::runtime_error("Failed to initialize xml writer.");
    }

    m_context = std::move(context);
  }

  XmlWriter::~XmlWriter() {}

  void XmlWriter::Write(XmlNode node)
  {
    auto& context = m_context;
    if (node.Type == XmlNodeType::StartTag)
    {
      if (node.HasValue)
      {
        Write(XmlNode{XmlNodeType::StartTag, std::move(node.Name)});
        Write(XmlNode{XmlNodeType::Text, std::string(), std::move(node.Value)});
        Write(XmlNode{XmlNodeType::EndTag});
        return;
      }
      WS_XML_STRING name;
      name.bytes = reinterpret_cast<BYTE*>(&node.Name[0]);
      name.length = static_cast<ULONG>(node.Name.length());
      name.dictionary = nullptr;
      WS_XML_STRING ns = WS_XML_STRING_NULL;
      HRESULT ret = WsWriteStartElement(context->writer, nullptr, &name, &ns, context->error);
      if (!SUCCEEDED(ret))
      {
        throw std::runtime_error("Failed to write xml.");
      }
    }
    else if (node.Type == XmlNodeType::EndTag)
    {
      HRESULT ret = WsWriteEndElement(context->writer, context->error);
      if (!SUCCEEDED(ret))
      {
        throw std::runtime_error("Failed to write xml.");
      }
    }
    else if (node.Type == XmlNodeType::Text)
    {
      HRESULT ret = WsWriteCharsUtf8(
          context->writer,
          reinterpret_cast<const BYTE*>(node.Value.data()),
          static_cast<ULONG>(node.Value.size()),
          context->error);
      if (!SUCCEEDED(ret))
      {
        throw std::runtime_error("Failed to write xml.");
      }
    }
    else if (node.Type == XmlNodeType::Attribute)
    {
      WS_XML_STRING name;
      name.bytes = reinterpret_cast<BYTE*>(&node.Name[0]);
      name.length = static_cast<ULONG>(node.Name.length());
      name.dictionary = nullptr;
      WS_XML_STRING ns = WS_XML_STRING_NULL;
      HRESULT ret
          = WsWriteStartAttribute(context->writer, nullptr, &name, &ns, FALSE, context->error);
      if (!SUCCEEDED(ret))
      {
        throw std::runtime_error("Failed to write xml.");
      }
      Write(XmlNode{XmlNodeType::Text, std::string(), std::move(node.Value)});
      ret = WsWriteEndAttribute(context->writer, context->error);
      if (!SUCCEEDED(ret))
      {
        throw std::runtime_error("Failed to write xml.");
      }
    }
    else if (node.Type == XmlNodeType::End)
    {
    }
    else
    {
      throw std::runtime_error(
          "Unsupported XmlNode type "
          + std::to_string(static_cast<std::underlying_type<XmlNodeType>::type>(node.Type)) + ".");
    }
  }

  std::string XmlWriter::GetDocument()
  {
    auto& context = m_context;

    BOOL boolValueTrue = TRUE;
    WS_XML_WRITER_PROPERTY writerProperty[2];
    writerProperty[0].id = WS_XML_WRITER_PROPERTY_WRITE_DECLARATION;
    writerProperty[0].value = &boolValueTrue;
    writerProperty[0].valueSize = sizeof(boolValueTrue);
    writerProperty[1].id = WS_XML_WRITER_PROPERTY_BUFFER_MAX_SIZE;
    ULONG maxBufferSize = 256 * 1024 * 1024UL;
    writerProperty[1].value = &maxBufferSize;
    writerProperty[1].valueSize = sizeof(maxBufferSize);
    void* xml = nullptr;
    ULONG xmlLength = 0;
    HRESULT ret = WsWriteXmlBufferToBytes(
        context->writer,
        context->buffer,
        nullptr,
        writerProperty,
        sizeof(writerProperty) / sizeof(writerProperty[0]),
        context->heap,
        &xml,
        &xmlLength,
        context->error);
    if (!SUCCEEDED(ret))
    {
      throw std::runtime_error("Failed to write xml.");
    }

    return std::string(static_cast<const char*>(xml), xmlLength);
  }

#else

  struct XmlGlobalInitializer final
  {
    XmlGlobalInitializer() { xmlInitParser(); }
    ~XmlGlobalInitializer() { xmlCleanupParser(); }
  };

  static void XmlGlobalInitialize() { static XmlGlobalInitializer globalInitializer; }

  struct XmlReader::XmlReaderContext
  {
    using XmlTextReaderPtr = std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)>;

    XmlTextReaderPtr reader;
    bool readingAttributes = false;
    bool readingEmptyTag = false;

    explicit XmlReaderContext(XmlTextReaderPtr&& reader_) : reader(std::move(reader_)) {}
  };

  XmlReader::XmlReader(const char* data, size_t length)
  {
    XmlGlobalInitialize();

    if (length > static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
      throw std::runtime_error("Xml data too big.");
    }

    auto reader = XmlReaderContext::XmlTextReaderPtr(
        xmlReaderForMemory(data, static_cast<int>(length), nullptr, nullptr, 0), xmlFreeTextReader);

    if (!reader)
    {
      throw std::runtime_error("Failed to parse xml.");
    }

    m_context = std::make_unique<XmlReaderContext>(std::move(reader));
  }

  XmlReader::XmlReader(XmlReader&& other) noexcept { *this = std::move(other); }

  XmlReader& XmlReader::operator=(XmlReader&& other) noexcept
  {
    m_context = std::move(other.m_context);
    return *this;
  }

  XmlReader::~XmlReader() = default;

  XmlNode XmlReader::Read()
  {
    XmlReaderContext* context = m_context.get();
    xmlTextReader* reader = m_context->reader.get();
    if (context->readingAttributes)
    {
      int ret = xmlTextReaderMoveToNextAttribute(reader);
      if (ret == 1)
      {
        const char* name = reinterpret_cast<const char*>(xmlTextReaderConstName(reader));
        const char* value = reinterpret_cast<const char*>(xmlTextReaderConstValue(reader));
        return XmlNode{XmlNodeType::Attribute, name, value};
      }
      else if (ret == 0)
      {
        context->readingAttributes = false;
      }
      else
      {
        throw std::runtime_error("Failed to parse xml.");
      }
    }
    if (context->readingEmptyTag)
    {
      context->readingEmptyTag = false;
      return XmlNode{XmlNodeType::EndTag};
    }

    int ret = xmlTextReaderRead(reader);
    if (ret == 0)
    {
      return XmlNode{XmlNodeType::End};
    }
    if (ret != 1)
    {
      throw std::runtime_error("Failed to parse xml.");
    }

    int type = xmlTextReaderNodeType(reader);
    bool is_empty = xmlTextReaderIsEmptyElement(reader) == 1;
    bool has_value = xmlTextReaderHasValue(reader) == 1;
    bool has_attributes = xmlTextReaderHasAttributes(reader) == 1;

    const char* name = reinterpret_cast<const char*>(xmlTextReaderConstName(reader));
    const char* value = reinterpret_cast<const char*>(xmlTextReaderConstValue(reader));

    if (has_attributes)
    {
      context->readingAttributes = true;
    }

    if (type == XML_READER_TYPE_ELEMENT && is_empty)
    {
      context->readingEmptyTag = true;
      return XmlNode{XmlNodeType::StartTag, name};
    }
    else if (type == XML_READER_TYPE_ELEMENT)
    {
      return XmlNode{XmlNodeType::StartTag, name};
    }
    else if (type == XML_READER_TYPE_END_ELEMENT)
    {
      return XmlNode{XmlNodeType::EndTag};
    }
    else if (type == XML_READER_TYPE_TEXT)
    {
      if (has_value)
      {
        return XmlNode{XmlNodeType::Text, std::string(), value};
      }
    }
    else if (type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE)
    {
      // silently ignore
    }
    else
    {
      throw std::runtime_error("Unknown type " + std::to_string(type) + " while parsing xml.");
    }

    return Read();
  }

  struct XmlWriter::XmlWriterContext
  {
    using XmlBufferPtr = std::unique_ptr<xmlBuffer, decltype(&xmlBufferFree)>;
    using XmlTextWriterPtr = std::unique_ptr<xmlTextWriter, decltype(&xmlFreeTextWriter)>;

    XmlBufferPtr buffer;
    XmlTextWriterPtr writer;

    explicit XmlWriterContext(XmlBufferPtr&& buffer_, XmlTextWriterPtr&& writer_)
        : buffer(std::move(buffer_)), writer(std::move(writer_))
    {
    }
  };

  XmlWriter::XmlWriter()
  {
    XmlGlobalInitialize();
    auto buffer = XmlWriterContext::XmlBufferPtr(xmlBufferCreate(), xmlBufferFree);

    if (!buffer)
    {
      throw std::runtime_error("Failed to initialize xml writer.");
    }

    auto writer = XmlWriterContext::XmlTextWriterPtr(
        xmlNewTextWriterMemory(buffer.get(), 0), xmlFreeTextWriter);

    if (!writer)
    {
      throw std::runtime_error("Failed to initialize xml writer.");
    }

    xmlTextWriterStartDocument(writer.get(), nullptr, nullptr, nullptr);

    m_context = std::make_unique<XmlWriterContext>(std::move(buffer), std::move(writer));
  }

  XmlWriter::XmlWriter(XmlWriter&& other) noexcept { *this = std::move(other); }

  XmlWriter& XmlWriter::operator=(XmlWriter&& other) noexcept
  {
    m_context = std::move(other.m_context);
    return *this;
  }

  XmlWriter::~XmlWriter() = default;

  namespace {
    inline xmlChar* BadCast(const char* x)
    {
      return const_cast<xmlChar*>(reinterpret_cast<const xmlChar*>(x));
    }
  } // namespace

  void XmlWriter::Write(XmlNode node)
  {
    xmlTextWriter* writer = m_context->writer.get();
    if (node.Type == XmlNodeType::StartTag)
    {
      if (!node.HasValue)
      {
        xmlTextWriterStartElement(writer, BadCast(node.Name.data()));
      }
      else
      {
        xmlTextWriterWriteElement(writer, BadCast(node.Name.data()), BadCast(node.Value.data()));
      }
    }
    else if (node.Type == XmlNodeType::EndTag)
    {
      xmlTextWriterEndElement(writer);
    }
    else if (node.Type == XmlNodeType::Text)
    {
      xmlTextWriterWriteString(writer, BadCast(node.Value.data()));
    }
    else if (node.Type == XmlNodeType::Attribute)
    {
      xmlTextWriterWriteAttribute(writer, BadCast(node.Name.data()), BadCast(node.Value.data()));
    }
    else if (node.Type == XmlNodeType::End)
    {
      xmlTextWriterEndDocument(writer);
    }
    else
    {
      throw std::runtime_error(
          "Unsupported XmlNode type "
          + std::to_string(static_cast<std::underlying_type<XmlNodeType>::type>(node.Type)) + ".");
    }
  }

  std::string XmlWriter::GetDocument()
  {
    return std::string(
        reinterpret_cast<const char*>(m_context->buffer->content), m_context->buffer->use);
  }

#endif

}}} // namespace Azure::Storage::_internal
