/**
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

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "Decoder.hh"
#include "Encoder.hh"
#include "Generic.hh"
#include "NodeImpl.hh"
#include "Stream.hh"
#include "Symbol.hh"
#include "Types.hh"
#include "ValidSchema.hh"
#include "ValidatingCodec.hh"

namespace avro {

using std::make_shared;

namespace parsing {

using std::shared_ptr;
using std::static_pointer_cast;

using std::make_pair;
using std::map;
using std::pair;
using std::reverse;
using std::set;
using std::stack;
using std::string;
using std::unique_ptr;
using std::vector;

typedef pair<NodePtr, NodePtr> NodePair;

class ResolvingGrammarGenerator : public ValidatingGrammarGenerator {
    ProductionPtr doGenerate2(const NodePtr &writer,
                              const NodePtr &reader, map<NodePair, ProductionPtr> &m,
                              map<NodePtr, ProductionPtr> &m2);
    ProductionPtr resolveRecords(const NodePtr &writer,
                                 const NodePtr &reader, map<NodePair, ProductionPtr> &m,
                                 map<NodePtr, ProductionPtr> &m2);
    ProductionPtr resolveUnion(const NodePtr &writer,
                               const NodePtr &reader, map<NodePair, ProductionPtr> &m,
                               map<NodePtr, ProductionPtr> &m2);

    static std::optional<size_t> bestBranch(const NodePtr &writer, const NodePtr &reader);

    ProductionPtr getWriterProduction(const NodePtr &n,
                                      map<NodePtr, ProductionPtr> &m2);

public:
    Symbol generate(
        const ValidSchema &writer, const ValidSchema &reader);
};

Symbol ResolvingGrammarGenerator::generate(
    const ValidSchema &writer, const ValidSchema &reader) {
    map<NodePtr, ProductionPtr> m2;

    const NodePtr &rr = reader.root();
    const NodePtr &rw = writer.root();
    ProductionPtr backup = ValidatingGrammarGenerator::doGenerate(rw, m2);
    fixup(backup, m2);

    map<NodePair, ProductionPtr> m;
    ProductionPtr main = doGenerate2(rw, rr, m, m2);
    fixup(main, m);
    return Symbol::rootSymbol(main, backup);
}

std::optional<size_t> ResolvingGrammarGenerator::bestBranch(const NodePtr &writer,
                                                            const NodePtr &reader) {
    Type t = writer->type();

    const size_t c = reader->leaves();
    for (size_t j = 0; j < c; ++j) {
        NodePtr r = reader->leafAt(j);
        if (r->type() == AVRO_SYMBOLIC) {
            r = resolveSymbol(r);
        }
        if (t == r->type()) {
            if (r->hasName()) {
                if (r->name() == writer->name()) {
                    return j;
                }
            } else {
                return j;
            }
        }
    }

    for (size_t j = 0; j < c; ++j) {
        const NodePtr &r = reader->leafAt(j);
        Type rt = r->type();
        switch (t) {
            case AVRO_INT:
                if (rt == AVRO_LONG || rt == AVRO_DOUBLE || rt == AVRO_FLOAT) {
                    return j;
                }
                break;
            case AVRO_LONG:
            case AVRO_FLOAT:
                if (rt == AVRO_DOUBLE) {
                    return j;
                }
                break;
            default:
                break;
        }
    }
    return std::nullopt;
}

static shared_ptr<vector<uint8_t>> getAvroBinary(
    const GenericDatum &defaultValue) {
    EncoderPtr e = binaryEncoder();
    unique_ptr<OutputStream> os = memoryOutputStream();
    e->init(*os);
    GenericWriter::write(*e, defaultValue);
    e->flush();
    return snapshot(*os);
}

ProductionPtr ResolvingGrammarGenerator::getWriterProduction(
    const NodePtr &n, map<NodePtr, ProductionPtr> &m2) {
    const NodePtr &nn = (n->type() == AVRO_SYMBOLIC) ? static_cast<const NodeSymbolic &>(*n).getNode() : n;
    map<NodePtr, ProductionPtr>::const_iterator it2 = m2.find(nn);
    if (it2 != m2.end()) {
        return it2->second;
    } else {
        ProductionPtr result = ValidatingGrammarGenerator::doGenerate(nn, m2);
        fixup(result, m2);
        return result;
    }
}

ProductionPtr ResolvingGrammarGenerator::resolveRecords(
    const NodePtr &writer, const NodePtr &reader,
    map<NodePair, ProductionPtr> &m,
    map<NodePtr, ProductionPtr> &m2) {
    ProductionPtr result = make_shared<Production>();

    vector<string> wf(writer->names());
    for (size_t i = 0; i < wf.size(); ++i) {
        wf[i] = writer->nameAt(i);
    }

    set<size_t> rf;
    for (size_t i = 0; i < reader->names(); ++i) {
        rf.emplace(i);
    }

    vector<size_t> fieldOrder;
    fieldOrder.reserve(rf.size());

    /*
     * We look for all writer fields in the reader. If found, recursively
     * resolve the corresponding fields. Then erase the reader field.
     * If no matching field is found for reader, arrange to skip the writer
     * field.
     */
    for (size_t wi = 0; wi != wf.size(); ++wi) {
        size_t ri;
        if (reader->nameIndex(wf[wi], ri)) {
            ProductionPtr p = doGenerate2(writer->leafAt(wi), reader->leafAt(ri), m, m2);
            copy(p->rbegin(), p->rend(), back_inserter(*result));
            fieldOrder.push_back(ri);
            rf.erase(ri);
        } else {
            ProductionPtr p = getWriterProduction(writer->leafAt(wi), m2);
            result->push_back(Symbol::skipStart());
            if (p->size() == 1) {
                result->push_back((*p)[0]);
            } else {
                result->push_back(Symbol::indirect(p));
            }
        }
    }

    /*
     * Examine the reader fields left out (i.e. those didn't have corresponding
     * writer field).
     */
    for (const auto ri : rf) {
        NodePtr s = reader->leafAt(ri);
        fieldOrder.push_back(ri);

        if (s->type() == AVRO_SYMBOLIC) {
            s = resolveSymbol(s);
        }
        shared_ptr<vector<uint8_t>> defaultBinary =
            getAvroBinary(reader->defaultValueAt(ri));
        result->push_back(Symbol::defaultStartAction(defaultBinary));
        auto it = m.find(NodePair(s, s));
        ProductionPtr p = it == m.end() ? doGenerate2(s, s, m, m2) : it->second;
        copy(p->rbegin(), p->rend(), back_inserter(*result));
        result->push_back(Symbol::defaultEndAction());
    }
    reverse(result->begin(), result->end());
    result->push_back(Symbol::sizeListAction(fieldOrder));
    result->push_back(Symbol::recordAction());

    return result;
}

ProductionPtr ResolvingGrammarGenerator::resolveUnion(
    const NodePtr &writer, const NodePtr &reader,
    map<NodePair, ProductionPtr> &m,
    map<NodePtr, ProductionPtr> &m2) {
    vector<ProductionPtr> v;
    size_t c = writer->leaves();
    v.reserve(c);
    for (size_t i = 0; i < c; ++i) {
        ProductionPtr p = doGenerate2(writer->leafAt(i), reader, m, m2);
        v.push_back(p);
    }
    ProductionPtr result = make_shared<Production>();
    result->push_back(Symbol::alternative(v));
    result->push_back(Symbol::writerUnionAction());
    return result;
}

ProductionPtr ResolvingGrammarGenerator::doGenerate2(
    const NodePtr &w, const NodePtr &r,
    map<NodePair, ProductionPtr> &m,
    map<NodePtr, ProductionPtr> &m2) {
    const NodePtr writer = w->type() == AVRO_SYMBOLIC ? resolveSymbol(w) : w;
    const NodePtr reader = r->type() == AVRO_SYMBOLIC ? resolveSymbol(r) : r;
    Type writerType = writer->type();
    Type readerType = reader->type();

    if (writerType == readerType) {
        switch (writerType) {
            case AVRO_NULL:
                return make_shared<Production>(1, Symbol::nullSymbol());
            case AVRO_BOOL:
                return make_shared<Production>(1, Symbol::boolSymbol());
            case AVRO_INT:
                return make_shared<Production>(1, Symbol::intSymbol());
            case AVRO_LONG:
                return make_shared<Production>(1, Symbol::longSymbol());
            case AVRO_FLOAT:
                return make_shared<Production>(1, Symbol::floatSymbol());
            case AVRO_DOUBLE:
                return make_shared<Production>(1, Symbol::doubleSymbol());
            case AVRO_STRING:
                return make_shared<Production>(1, Symbol::stringSymbol());
            case AVRO_BYTES:
                return make_shared<Production>(1, Symbol::bytesSymbol());
            case AVRO_FIXED:
                if (writer->name().equalOrAliasedBy(reader->name()) && writer->fixedSize() == reader->fixedSize()) {
                    ProductionPtr result = make_shared<Production>();
                    result->push_back(Symbol::sizeCheckSymbol(reader->fixedSize()));
                    result->push_back(Symbol::fixedSymbol());
                    m[make_pair(writer, reader)] = result;
                    return result;
                }
                break;
            case AVRO_RECORD:
                if (writer->name().equalOrAliasedBy(reader->name())) {
                    const pair<NodePtr, NodePtr> key(writer, reader);
                    map<NodePair, ProductionPtr>::const_iterator kp = m.find(key);
                    if (kp != m.end()) {
                        return (kp->second) ? kp->second : make_shared<Production>(1, Symbol::placeholder(key));
                    }
                    m[key] = ProductionPtr();
                    ProductionPtr result = resolveRecords(writer, reader, m, m2);
                    m[key] = result;
                    return make_shared<Production>(1, Symbol::indirect(result));
                }
                break;

            case AVRO_ENUM:
                if (writer->name().equalOrAliasedBy(reader->name())) {
                    ProductionPtr result = make_shared<Production>();
                    result->push_back(Symbol::enumAdjustSymbol(writer, reader));
                    result->push_back(Symbol::enumSymbol());
                    m[make_pair(writer, reader)] = result;
                    return result;
                }
                break;
            case AVRO_ARRAY: {
                ProductionPtr p = getWriterProduction(writer->leafAt(0), m2);
                ProductionPtr p2 = doGenerate2(writer->leafAt(0), reader->leafAt(0), m, m2);
                ProductionPtr result = make_shared<Production>();
                result->push_back(Symbol::arrayEndSymbol());
                result->push_back(Symbol::repeater(p2, p, true));
                result->push_back(Symbol::arrayStartSymbol());
                return result;
            }
            case AVRO_MAP: {
                ProductionPtr pp =
                    doGenerate2(writer->leafAt(1), reader->leafAt(1), m, m2);
                ProductionPtr v(new Production(*pp));
                v->push_back(Symbol::stringSymbol());

                ProductionPtr pp2 = getWriterProduction(writer->leafAt(1), m2);
                ProductionPtr v2(new Production(*pp2));

                v2->push_back(Symbol::stringSymbol());

                ProductionPtr result = make_shared<Production>();
                result->push_back(Symbol::mapEndSymbol());
                result->push_back(Symbol::repeater(v, v2, false));
                result->push_back(Symbol::mapStartSymbol());
                return result;
            }
            case AVRO_UNION:
                return resolveUnion(writer, reader, m, m2);
            case AVRO_SYMBOLIC: {
                shared_ptr<NodeSymbolic> w2 =
                    static_pointer_cast<NodeSymbolic>(writer);
                shared_ptr<NodeSymbolic> r2 =
                    static_pointer_cast<NodeSymbolic>(reader);
                NodePair p(w2->getNode(), r2->getNode());
                auto it = m.find(p);
                if (it != m.end() && it->second) {
                    return it->second;
                } else {
                    m[p] = ProductionPtr();
                    return make_shared<Production>(1, Symbol::placeholder(p));
                }
            }
            default:
                throw Exception("Unknown node type");
        }
    } else if (writerType == AVRO_UNION) {
        return resolveUnion(writer, reader, m, m2);
    } else {
        switch (readerType) {
            case AVRO_LONG:
                if (writerType == AVRO_INT) {
                    return make_shared<Production>(1,
                                                   Symbol::resolveSymbol(Symbol::Kind::Int, Symbol::Kind::Long));
                }
                break;
            case AVRO_FLOAT:
                if (writerType == AVRO_INT || writerType == AVRO_LONG) {
                    return make_shared<Production>(1,
                                                   Symbol::resolveSymbol(writerType == AVRO_INT ? Symbol::Kind::Int : Symbol::Kind::Long, Symbol::Kind::Float));
                }
                break;
            case AVRO_DOUBLE:
                if (writerType == AVRO_INT || writerType == AVRO_LONG
                    || writerType == AVRO_FLOAT) {
                    return make_shared<Production>(1,
                                                   Symbol::resolveSymbol(writerType == AVRO_INT ? Symbol::Kind::Int : writerType == AVRO_LONG ? Symbol::Kind::Long
                                                                                                                                              : Symbol::Kind::Float,
                                                                         Symbol::Kind::Double));
                }
                break;

            case AVRO_UNION: {
                auto j = bestBranch(writer, reader);
                if (j) {
                    ProductionPtr p = doGenerate2(writer, reader->leafAt(*j), m, m2);
                    ProductionPtr result = make_shared<Production>();
                    result->push_back(Symbol::unionAdjustSymbol(*j, p));
                    result->push_back(Symbol::unionSymbol());
                    return result;
                }
            } break;
            case AVRO_NULL:
            case AVRO_BOOL:
            case AVRO_INT:
            case AVRO_STRING:
            case AVRO_BYTES:
            case AVRO_ENUM:
            case AVRO_ARRAY:
            case AVRO_MAP:
            case AVRO_RECORD:
                break;
            default:
                throw Exception("Unknown node type");
        }
    }
    return make_shared<Production>(1, Symbol::error(writer, reader));
}

class ResolvingDecoderHandler {
    shared_ptr<vector<uint8_t>> defaultData_;
    unique_ptr<InputStream> inp_;
    DecoderPtr backup_;
    DecoderPtr &base_;
    const DecoderPtr binDecoder;

public:
    explicit ResolvingDecoderHandler(DecoderPtr &base) : base_(base),
                                                         binDecoder(binaryDecoder()) {}
    size_t handle(const Symbol &s) {
        switch (s.kind()) {
            case Symbol::Kind::WriterUnion:
                return base_->decodeUnionIndex();
            case Symbol::Kind::DefaultStart:
                defaultData_ = s.extra<shared_ptr<vector<uint8_t>>>();
                backup_ = base_;
                inp_ = memoryInputStream(&(*defaultData_)[0], defaultData_->size());
                base_ = binDecoder;
                base_->init(*inp_);
                return 0;
            case Symbol::Kind::DefaultEnd:
                base_ = backup_;
                backup_.reset();
                return 0;
            default:
                return 0;
        }
    }

    void reset() {
        if (backup_ != nullptr) {
            base_ = backup_;
            backup_.reset();
        }
    }
};

template<typename Parser>
class ResolvingDecoderImpl : public ResolvingDecoder {
    DecoderPtr base_;
    ResolvingDecoderHandler handler_;
    Parser parser_;

    void init(InputStream &is) final;
    void decodeNull() final;
    bool decodeBool() final;
    int32_t decodeInt() final;
    int64_t decodeLong() final;
    float decodeFloat() final;
    double decodeDouble() final;
    void decodeString(string &value) final;
    void skipString() final;
    void decodeBytes(vector<uint8_t> &value) final;
    void skipBytes() final;
    void decodeFixed(size_t n, vector<uint8_t> &value) final;
    void skipFixed(size_t n) final;
    size_t decodeEnum() final;
    size_t arrayStart() final;
    size_t arrayNext() final;
    size_t skipArray() final;
    size_t mapStart() final;
    size_t mapNext() final;
    size_t skipMap() final;
    size_t decodeUnionIndex() final;
    const vector<size_t> &fieldOrder() final;
    void drain() final {
        parser_.processImplicitActions();
        base_->drain();
    }

public:
    ResolvingDecoderImpl(const ValidSchema &writer, const ValidSchema &reader,
                         DecoderPtr base) : base_(std::move(base)),
                                            handler_(base_),
                                            parser_(ResolvingGrammarGenerator().generate(writer, reader),
                                                    &(*base_), handler_) {
    }
};

template<typename P>
void ResolvingDecoderImpl<P>::init(InputStream &is) {
    handler_.reset();
    base_->init(is);
    parser_.reset();
}

template<typename P>
void ResolvingDecoderImpl<P>::decodeNull() {
    parser_.advance(Symbol::Kind::Null);
    base_->decodeNull();
}

template<typename P>
bool ResolvingDecoderImpl<P>::decodeBool() {
    parser_.advance(Symbol::Kind::Bool);
    return base_->decodeBool();
}

template<typename P>
int32_t ResolvingDecoderImpl<P>::decodeInt() {
    parser_.advance(Symbol::Kind::Int);
    return base_->decodeInt();
}

template<typename P>
int64_t ResolvingDecoderImpl<P>::decodeLong() {
    Symbol::Kind k = parser_.advance(Symbol::Kind::Long);
    return k == Symbol::Kind::Int ? base_->decodeInt() : base_->decodeLong();
}

template<typename P>
float ResolvingDecoderImpl<P>::decodeFloat() {
    Symbol::Kind k = parser_.advance(Symbol::Kind::Float);
    return k == Symbol::Kind::Int ? static_cast<float>(base_->decodeInt())
        : k == Symbol::Kind::Long ? static_cast<float>(base_->decodeLong())
                                  : base_->decodeFloat();
}

template<typename P>
double ResolvingDecoderImpl<P>::decodeDouble() {
    Symbol::Kind k = parser_.advance(Symbol::Kind::Double);
    return k == Symbol::Kind::Int  ? static_cast<double>(base_->decodeInt())
        : k == Symbol::Kind::Long  ? static_cast<double>(base_->decodeLong())
        : k == Symbol::Kind::Float ? base_->decodeFloat()
                                   : base_->decodeDouble();
}

template<typename P>
void ResolvingDecoderImpl<P>::decodeString(string &value) {
    parser_.advance(Symbol::Kind::String);
    base_->decodeString(value);
}

template<typename P>
void ResolvingDecoderImpl<P>::skipString() {
    parser_.advance(Symbol::Kind::String);
    base_->skipString();
}

template<typename P>
void ResolvingDecoderImpl<P>::decodeBytes(vector<uint8_t> &value) {
    parser_.advance(Symbol::Kind::Bytes);
    base_->decodeBytes(value);
}

template<typename P>
void ResolvingDecoderImpl<P>::skipBytes() {
    parser_.advance(Symbol::Kind::Bytes);
    base_->skipBytes();
}

template<typename P>
void ResolvingDecoderImpl<P>::decodeFixed(size_t n, vector<uint8_t> &value) {
    parser_.advance(Symbol::Kind::Fixed);
    parser_.assertSize(n);
    return base_->decodeFixed(n, value);
}

template<typename P>
void ResolvingDecoderImpl<P>::skipFixed(size_t n) {
    parser_.advance(Symbol::Kind::Fixed);
    parser_.assertSize(n);
    base_->skipFixed(n);
}

template<typename P>
size_t ResolvingDecoderImpl<P>::decodeEnum() {
    parser_.advance(Symbol::Kind::Enum);
    size_t n = base_->decodeEnum();
    return parser_.enumAdjust(n);
}

template<typename P>
size_t ResolvingDecoderImpl<P>::arrayStart() {
    parser_.advance(Symbol::Kind::ArrayStart);
    size_t result = base_->arrayStart();
    parser_.pushRepeatCount(result);
    if (result == 0) {
        parser_.popRepeater();
        parser_.advance(Symbol::Kind::ArrayEnd);
    }
    return result;
}

template<typename P>
size_t ResolvingDecoderImpl<P>::arrayNext() {
    parser_.processImplicitActions();
    size_t result = base_->arrayNext();
    parser_.nextRepeatCount(result);
    if (result == 0) {
        parser_.popRepeater();
        parser_.advance(Symbol::Kind::ArrayEnd);
    }
    return result;
}

template<typename P>
size_t ResolvingDecoderImpl<P>::skipArray() {
    parser_.advance(Symbol::Kind::ArrayStart);
    size_t n = base_->skipArray();
    if (n == 0) {
        parser_.pop();
    } else {
        parser_.pushRepeatCount(n);
        parser_.skip(*base_);
    }
    parser_.advance(Symbol::Kind::ArrayEnd);
    return 0;
}

template<typename P>
size_t ResolvingDecoderImpl<P>::mapStart() {
    parser_.advance(Symbol::Kind::MapStart);
    size_t result = base_->mapStart();
    parser_.pushRepeatCount(result);
    if (result == 0) {
        parser_.popRepeater();
        parser_.advance(Symbol::Kind::MapEnd);
    }
    return result;
}

template<typename P>
size_t ResolvingDecoderImpl<P>::mapNext() {
    parser_.processImplicitActions();
    size_t result = base_->mapNext();
    parser_.nextRepeatCount(result);
    if (result == 0) {
        parser_.popRepeater();
        parser_.advance(Symbol::Kind::MapEnd);
    }
    return result;
}

template<typename P>
size_t ResolvingDecoderImpl<P>::skipMap() {
    parser_.advance(Symbol::Kind::MapStart);
    size_t n = base_->skipMap();
    if (n == 0) {
        parser_.pop();
    } else {
        parser_.pushRepeatCount(n);
        parser_.skip(*base_);
    }
    parser_.advance(Symbol::Kind::MapEnd);
    return 0;
}

template<typename P>
size_t ResolvingDecoderImpl<P>::decodeUnionIndex() {
    parser_.advance(Symbol::Kind::Union);
    return parser_.unionAdjust();
}

template<typename P>
const vector<size_t> &ResolvingDecoderImpl<P>::fieldOrder() {
    parser_.advance(Symbol::Kind::Record);
    return parser_.sizeList();
}

} // namespace parsing

ResolvingDecoderPtr resolvingDecoder(const ValidSchema &writer,
                                     const ValidSchema &reader, const DecoderPtr &base) {
    return make_shared<parsing::ResolvingDecoderImpl<parsing::SimpleParser<parsing::ResolvingDecoderHandler>>>(
        writer, reader, base);
}

} // namespace avro
