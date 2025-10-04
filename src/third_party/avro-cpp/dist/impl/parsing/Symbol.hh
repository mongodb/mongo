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

#ifndef avro_parsing_Symbol_hh__
#define avro_parsing_Symbol_hh__

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stack>
#include <utility>
#include <vector>

#include "Decoder.hh"
#include "Exception.hh"
#include "Node.hh"

#include <boost/any.hpp>
#include <boost/tuple/tuple.hpp>

namespace avro {
namespace parsing {

class Symbol;

typedef std::vector<Symbol> Production;
typedef std::shared_ptr<Production> ProductionPtr;
typedef boost::tuple<std::stack<ssize_t>, bool, ProductionPtr, ProductionPtr> RepeaterInfo;
typedef boost::tuple<ProductionPtr, ProductionPtr> RootInfo;

class Symbol {
public:
    enum class Kind {
        TerminalLow,  // extra has nothing
        Null,
        Bool,
        Int,
        Long,
        Float,
        Double,
        String,
        Bytes,
        ArrayStart,
        ArrayEnd,
        MapStart,
        MapEnd,
        Fixed,
        Enum,
        Union,
        TerminalHigh,
        SizeCheck,    // Extra has size
        NameList,     // Extra has a vector<string>
        Root,         // Root for a schema, extra is Symbol
        Repeater,     // Array or Map, extra is symbol
        Alternative,  // One of many (union), extra is Union
        Placeholder,  // To be fixed up later.
        Indirect,     // extra is shared_ptr<Production>
        Symbolic,     // extra is weal_ptr<Production>
        EnumAdjust,
        UnionAdjust,
        SkipStart,
        Resolve,

        ImplicitActionLow,
        RecordStart,
        RecordEnd,
        Field,  // extra is string
        Record,
        SizeList,
        WriterUnion,
        DefaultStart,  // extra has default value in Avro binary encoding
        DefaultEnd,
        ImplicitActionHigh,
        Error
    };

private:
    Kind kind_;
    boost::any extra_;

    explicit Symbol(Kind k) : kind_(k) {}
    template <typename T>
    Symbol(Kind k, T t) : kind_(k), extra_(t) {}

public:
    Kind kind() const {
        return kind_;
    }

    template <typename T>
    T extra() const {
        return boost::any_cast<T>(extra_);
    }

    template <typename T>
    T* extrap() {
        return boost::any_cast<T>(&extra_);
    }

    template <typename T>
    const T* extrap() const {
        return boost::any_cast<T>(&extra_);
    }

    template <typename T>
    void extra(const T& t) {
        extra_ = t;
    }

    bool isTerminal() const {
        return kind_ > Kind::TerminalLow && kind_ < Kind::TerminalHigh;
    }

    bool isImplicitAction() const {
        return kind_ > Kind::ImplicitActionLow && kind_ < Kind::ImplicitActionHigh;
    }

    static const char* stringValues[];
    static const char* toString(Kind k) {
        return stringValues[static_cast<size_t>(k)];
    }

    static Symbol rootSymbol(ProductionPtr& s) {
        return Symbol(Kind::Root, RootInfo(s, std::make_shared<Production>()));
    }

    static Symbol rootSymbol(const ProductionPtr& main, const ProductionPtr& backup) {
        return Symbol(Kind::Root, RootInfo(main, backup));
    }

    static Symbol nullSymbol() {
        return Symbol(Kind::Null);
    }

    static Symbol boolSymbol() {
        return Symbol(Kind::Bool);
    }

    static Symbol intSymbol() {
        return Symbol(Kind::Int);
    }

    static Symbol longSymbol() {
        return Symbol(Kind::Long);
    }

    static Symbol floatSymbol() {
        return Symbol(Kind::Float);
    }

    static Symbol doubleSymbol() {
        return Symbol(Kind::Double);
    }

    static Symbol stringSymbol() {
        return Symbol(Kind::String);
    }

    static Symbol bytesSymbol() {
        return Symbol(Kind::Bytes);
    }

    static Symbol sizeCheckSymbol(size_t s) {
        return Symbol(Kind::SizeCheck, s);
    }

    static Symbol fixedSymbol() {
        return Symbol(Kind::Fixed);
    }

    static Symbol enumSymbol() {
        return Symbol(Kind::Enum);
    }

    static Symbol arrayStartSymbol() {
        return Symbol(Kind::ArrayStart);
    }

    static Symbol arrayEndSymbol() {
        return Symbol(Kind::ArrayEnd);
    }

    static Symbol mapStartSymbol() {
        return Symbol(Kind::MapStart);
    }

    static Symbol mapEndSymbol() {
        return Symbol(Kind::MapEnd);
    }

    static Symbol repeater(const ProductionPtr& p, bool isArray) {
        return repeater(p, p, isArray);
    }

    static Symbol repeater(const ProductionPtr& read, const ProductionPtr& skip, bool isArray) {
        std::stack<ssize_t> s;
        return Symbol(Kind::Repeater, RepeaterInfo(s, isArray, read, skip));
    }

    static Symbol defaultStartAction(std::shared_ptr<std::vector<uint8_t>> bb) {
        return Symbol(Kind::DefaultStart, std::move(bb));
    }

    static Symbol defaultEndAction() {
        return Symbol(Kind::DefaultEnd);
    }

    static Symbol alternative(const std::vector<ProductionPtr>& branches) {
        return Symbol(Symbol::Kind::Alternative, branches);
    }

    static Symbol unionSymbol() {
        return Symbol(Kind::Union);
    }

    static Symbol recordStartSymbol() {
        return Symbol(Kind::RecordStart);
    }

    static Symbol recordEndSymbol() {
        return Symbol(Kind::RecordEnd);
    }

    static Symbol fieldSymbol(const std::string& name) {
        return Symbol(Kind::Field, name);
    }

    static Symbol writerUnionAction() {
        return Symbol(Kind::WriterUnion);
    }

    static Symbol nameListSymbol(const std::vector<std::string>& v) {
        return Symbol(Kind::NameList, v);
    }

    template <typename T>
    static Symbol placeholder(const T& n) {
        return Symbol(Kind::Placeholder, n);
    }

    static Symbol indirect(const ProductionPtr& p) {
        return Symbol(Kind::Indirect, p);
    }

    static Symbol symbolic(const std::weak_ptr<Production>& p) {
        return Symbol(Kind::Symbolic, p);
    }

    static Symbol enumAdjustSymbol(const NodePtr& writer, const NodePtr& reader);

    static Symbol unionAdjustSymbol(size_t branch, const ProductionPtr& p) {
        return Symbol(Kind::UnionAdjust, std::make_pair(branch, p));
    }

    static Symbol sizeListAction(std::vector<size_t> order) {
        return Symbol(Kind::SizeList, std::move(order));
    }

    static Symbol recordAction() {
        return Symbol(Kind::Record);
    }

    static Symbol error(const NodePtr& writer, const NodePtr& reader);

    static Symbol resolveSymbol(Kind w, Kind r) {
        return Symbol(Kind::Resolve, std::make_pair(w, r));
    }

    static Symbol skipStart() {
        return Symbol(Kind::SkipStart);
    }
};

/**
 * Recursively replaces all placeholders in the production with the
 * corresponding values.
 */
template <typename T>
void fixup(const ProductionPtr& p, const std::map<T, ProductionPtr>& m) {
    std::set<ProductionPtr> seen;
    for (auto& it : *p) {
        fixup(it, m, seen);
    }
}

/**
 * Recursively replaces all placeholders in the symbol with the values with the
 * corresponding values.
 */
template <typename T>
void fixup_internal(const ProductionPtr& p,
                    const std::map<T, ProductionPtr>& m,
                    std::set<ProductionPtr>& seen) {
    if (seen.find(p) == seen.end()) {
        seen.insert(p);
        for (auto& it : *p) {
            fixup(it, m, seen);
        }
    }
}

template <typename T>
void fixup(Symbol& s, const std::map<T, ProductionPtr>& m, std::set<ProductionPtr>& seen) {
    switch (s.kind()) {
        case Symbol::Kind::Indirect:
            fixup_internal(s.extra<ProductionPtr>(), m, seen);
            break;
        case Symbol::Kind::Alternative: {
            const std::vector<ProductionPtr>* vv = s.extrap<std::vector<ProductionPtr>>();
            for (const auto& it : *vv) {
                fixup_internal(it, m, seen);
            }
        } break;
        case Symbol::Kind::Repeater: {
            const RepeaterInfo& ri = *s.extrap<RepeaterInfo>();
            fixup_internal(boost::tuples::get<2>(ri), m, seen);
            fixup_internal(boost::tuples::get<3>(ri), m, seen);
        } break;
        case Symbol::Kind::Placeholder: {
            typename std::map<T, std::shared_ptr<Production>>::const_iterator it =
                m.find(s.extra<T>());
            if (it == m.end()) {
                throw Exception("Placeholder symbol cannot be resolved");
            }
            s = Symbol::symbolic(std::weak_ptr<Production>(it->second));
        } break;
        case Symbol::Kind::UnionAdjust:
            fixup_internal(s.extrap<std::pair<size_t, ProductionPtr>>()->second, m, seen);
            break;
        default:
            break;
    }
}

template <typename Handler>
class SimpleParser {
    Decoder* decoder_;
    Handler& handler_;
    /*
     * parsingStack always has root at the bottom of it.
     * So it is safe to call top() on it.
     */
    std::stack<Symbol> parsingStack;

    static void throwMismatch(Symbol::Kind actual, Symbol::Kind expected) {
        std::ostringstream oss;
        oss << "Invalid operation. Schema requires: " << Symbol::toString(expected)
            << ", got: " << Symbol::toString(actual);
        throw Exception(oss.str());
    }

    static void assertMatch(Symbol::Kind actual, Symbol::Kind expected) {
        if (expected != actual) {
            throwMismatch(actual, expected);
        }
    }

    void append(const ProductionPtr& ss) {
        for (Production::const_iterator it = ss->begin(); it != ss->end(); ++it) {
            parsingStack.push(*it);
        }
    }

    size_t popSize() {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::SizeCheck, s.kind());
        auto result = s.extra<size_t>();
        parsingStack.pop();
        return result;
    }

    static void assertLessThan(size_t n, size_t s) {
        if (n >= s) {
            std::ostringstream oss;
            oss << "Size max value. Upper bound: " << s << " found " << n;
            throw Exception(oss.str());
        }
    }

public:
    Symbol::Kind advance(Symbol::Kind k) {
        for (;;) {
            Symbol& s = parsingStack.top();
            //            std::cout << "advance: " << Symbol::toString(s.kind())
            //                      << " looking for " << Symbol::toString(k) << '\n';
            if (s.kind() == k) {
                parsingStack.pop();
                return k;
            } else if (s.isTerminal()) {
                throwMismatch(k, s.kind());
            } else {
                switch (s.kind()) {
                    case Symbol::Kind::Root:
                        append(boost::tuples::get<0>(*s.extrap<RootInfo>()));
                        continue;
                    case Symbol::Kind::Indirect: {
                        ProductionPtr pp = s.extra<ProductionPtr>();
                        parsingStack.pop();
                        append(pp);
                    }
                        continue;
                    case Symbol::Kind::Symbolic: {
                        ProductionPtr pp(s.extra<std::weak_ptr<Production>>());
                        parsingStack.pop();
                        append(pp);
                    }
                        continue;
                    case Symbol::Kind::Repeater: {
                        auto* p = s.extrap<RepeaterInfo>();
                        std::stack<ssize_t>& ns = boost::tuples::get<0>(*p);
                        if (ns.empty()) {
                            throw Exception("Empty item count stack in repeater advance");
                        }
                        if (ns.top() == 0) {
                            throw Exception("Zero item count in repeater advance");
                        }
                        --ns.top();
                        append(boost::tuples::get<2>(*p));
                    }
                        continue;
                    case Symbol::Kind::Error:
                        throw Exception(s.extra<std::string>());
                    case Symbol::Kind::Resolve: {
                        const std::pair<Symbol::Kind, Symbol::Kind>* p =
                            s.extrap<std::pair<Symbol::Kind, Symbol::Kind>>();
                        assertMatch(p->second, k);
                        Symbol::Kind result = p->first;
                        parsingStack.pop();
                        return result;
                    }
                    case Symbol::Kind::SkipStart:
                        parsingStack.pop();
                        skip(*decoder_);
                        break;
                    default:
                        if (s.isImplicitAction()) {
                            size_t n = handler_.handle(s);
                            if (s.kind() == Symbol::Kind::WriterUnion) {
                                parsingStack.pop();
                                selectBranch(n);
                            } else {
                                parsingStack.pop();
                            }
                        } else {
                            std::ostringstream oss;
                            oss << "Encountered " << Symbol::toString(s.kind())
                                << " while looking for " << Symbol::toString(k);
                            throw Exception(oss.str());
                        }
                }
            }
        }
    }

    void skip(Decoder& d) {
        const size_t sz = parsingStack.size();
        if (sz == 0) {
            throw Exception("Nothing to skip!");
        }
        while (parsingStack.size() >= sz) {
            Symbol& t = parsingStack.top();
            // std::cout << "skip: " << Symbol::toString(t.kind()) << '\n';
            switch (t.kind()) {
                case Symbol::Kind::Null:
                    d.decodeNull();
                    break;
                case Symbol::Kind::Bool:
                    d.decodeBool();
                    break;
                case Symbol::Kind::Int:
                    d.decodeInt();
                    break;
                case Symbol::Kind::Long:
                    d.decodeLong();
                    break;
                case Symbol::Kind::Float:
                    d.decodeFloat();
                    break;
                case Symbol::Kind::Double:
                    d.decodeDouble();
                    break;
                case Symbol::Kind::String:
                    d.skipString();
                    break;
                case Symbol::Kind::Bytes:
                    d.skipBytes();
                    break;
                case Symbol::Kind::ArrayStart: {
                    parsingStack.pop();
                    size_t n = d.skipArray();
                    processImplicitActions();
                    assertMatch(Symbol::Kind::Repeater, parsingStack.top().kind());
                    if (n == 0) {
                        break;
                    }
                    Symbol& t2 = parsingStack.top();
                    auto* p = t2.extrap<RepeaterInfo>();
                    boost::tuples::get<0>(*p).push(n);
                    continue;
                }
                case Symbol::Kind::ArrayEnd:
                    break;
                case Symbol::Kind::MapStart: {
                    parsingStack.pop();
                    size_t n = d.skipMap();
                    processImplicitActions();
                    assertMatch(Symbol::Kind::Repeater, parsingStack.top().kind());
                    if (n == 0) {
                        break;
                    }
                    Symbol& t2 = parsingStack.top();
                    auto* p2 = t2.extrap<RepeaterInfo>();
                    boost::tuples::get<0>(*p2).push(n);
                    continue;
                }
                case Symbol::Kind::MapEnd:
                    break;
                case Symbol::Kind::Fixed: {
                    parsingStack.pop();
                    Symbol& t2 = parsingStack.top();
                    d.decodeFixed(t2.extra<size_t>());
                } break;
                case Symbol::Kind::Enum:
                    parsingStack.pop();
                    d.decodeEnum();
                    break;
                case Symbol::Kind::Union: {
                    parsingStack.pop();
                    size_t n = d.decodeUnionIndex();
                    selectBranch(n);
                    continue;
                }
                case Symbol::Kind::Repeater: {
                    auto* p = t.extrap<RepeaterInfo>();
                    std::stack<ssize_t>& ns = boost::tuples::get<0>(*p);
                    if (ns.empty()) {
                        throw Exception("Empty item count stack in repeater skip");
                    }
                    ssize_t& n = ns.top();
                    if (n == 0) {
                        n = boost::tuples::get<1>(*p) ? d.arrayNext() : d.mapNext();
                    }
                    if (n != 0) {
                        --n;
                        append(boost::tuples::get<3>(*p));
                        continue;
                    } else {
                        ns.pop();
                    }
                    break;
                }
                case Symbol::Kind::Indirect: {
                    ProductionPtr pp = t.extra<ProductionPtr>();
                    parsingStack.pop();
                    append(pp);
                }
                    continue;
                case Symbol::Kind::Symbolic: {
                    ProductionPtr pp(t.extra<std::weak_ptr<Production>>());
                    parsingStack.pop();
                    append(pp);
                }
                    continue;
                default: {
                    std::ostringstream oss;
                    oss << "Don't know how to skip " << Symbol::toString(t.kind());
                    throw Exception(oss.str());
                }
            }
            parsingStack.pop();
        }
    }

    void assertSize(size_t n) {
        size_t s = popSize();
        if (s != n) {
            std::ostringstream oss;
            oss << "Incorrect size. Expected: " << s << " found " << n;
            throw Exception(oss.str());
        }
    }

    void assertLessThanSize(size_t n) {
        assertLessThan(n, popSize());
    }

    size_t enumAdjust(size_t n) {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::EnumAdjust, s.kind());
        const auto* v = s.extrap<std::pair<std::vector<int>, std::vector<std::string>>>();
        assertLessThan(n, v->first.size());

        int result = v->first[n];
        if (result < 0) {
            std::ostringstream oss;
            oss << "Cannot resolve symbol: " << v->second[-result - 1] << std::endl;
            throw Exception(oss.str());
        }
        parsingStack.pop();
        return result;
    }

    size_t unionAdjust() {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::UnionAdjust, s.kind());
        std::pair<size_t, ProductionPtr> p = s.extra<std::pair<size_t, ProductionPtr>>();
        parsingStack.pop();
        append(p.second);
        return p.first;
    }

    std::string nameForIndex(size_t e) {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::NameList, s.kind());
        const std::vector<std::string> names = s.extra<std::vector<std::string>>();
        if (e >= names.size()) {
            throw Exception("Not that many names");
        }
        std::string result = names[e];
        parsingStack.pop();
        return result;
    }

    size_t indexForName(const std::string& name) {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::NameList, s.kind());
        const std::vector<std::string> names = s.extra<std::vector<std::string>>();
        auto it = std::find(names.begin(), names.end(), name);
        if (it == names.end()) {
            throw Exception("No such enum symbol");
        }
        size_t result = it - names.begin();
        parsingStack.pop();
        return result;
    }

    void pushRepeatCount(size_t n) {
        processImplicitActions();
        Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::Repeater, s.kind());
        auto* p = s.extrap<RepeaterInfo>();
        std::stack<ssize_t>& nn = boost::tuples::get<0>(*p);
        nn.push(n);
    }

    void nextRepeatCount(size_t n) {
        processImplicitActions();
        Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::Repeater, s.kind());
        auto* p = s.extrap<RepeaterInfo>();
        std::stack<ssize_t>& nn = boost::tuples::get<0>(*p);
        if (nn.empty() || nn.top() != 0) {
            throw Exception("Wrong number of items");
        }
        nn.top() = n;
    }

    void popRepeater() {
        processImplicitActions();
        Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::Repeater, s.kind());
        auto* p = s.extrap<RepeaterInfo>();
        std::stack<ssize_t>& ns = boost::tuples::get<0>(*p);
        if (ns.empty()) {
            throw Exception("Incorrect number of items (empty)");
        }
        if (ns.top() > 0) {
            throw Exception("Incorrect number of items (non-zero)");
        }
        ns.pop();
        parsingStack.pop();
    }

    void selectBranch(size_t n) {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::Alternative, s.kind());
        std::vector<ProductionPtr> v = s.extra<std::vector<ProductionPtr>>();
        if (n >= v.size()) {
            throw Exception("Not that many branches");
        }
        parsingStack.pop();
        append(v[n]);
    }

    const std::vector<size_t>& sizeList() {
        const Symbol& s = parsingStack.top();
        assertMatch(Symbol::Kind::SizeList, s.kind());
        return *s.extrap<std::vector<size_t>>();
    }

    Symbol::Kind top() const {
        return parsingStack.top().kind();
    }

    void pop() {
        parsingStack.pop();
    }

    void processImplicitActions() {
        for (;;) {
            Symbol& s = parsingStack.top();
            if (s.isImplicitAction()) {
                handler_.handle(s);
                parsingStack.pop();
            } else if (s.kind() == Symbol::Kind::SkipStart) {
                parsingStack.pop();
                skip(*decoder_);
            } else if (s.kind() == Symbol::Kind::Indirect) {
                ProductionPtr pp = s.extra<ProductionPtr>();
                parsingStack.pop();
                append(pp);
            } else if (s.kind() == Symbol::Kind::Symbolic) {
                ProductionPtr pp(s.extra<std::weak_ptr<Production>>());
                parsingStack.pop();
                append(pp);
            } else {
                break;
            }
        }
    }

    SimpleParser(const Symbol& s, Decoder* d, Handler& h) : decoder_(d), handler_(h) {
        parsingStack.push(s);
    }

    void reset() {
        while (parsingStack.size() > 1) {
            parsingStack.pop();
        }
        Symbol& s = parsingStack.top();
        append(boost::tuples::get<0>(*s.extrap<RootInfo>()));
    }
};

inline std::ostream& operator<<(std::ostream& os, const Symbol& s);

inline std::ostream& operator<<(std::ostream& os, const Production& p) {
    os << '(';
    for (const auto& it : p) {
        os << it << ", ";
    }
    os << ')';
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const Symbol& s) {
    switch (s.kind()) {
        case Symbol::Kind::Repeater: {
            const RepeaterInfo& ri = *s.extrap<RepeaterInfo>();
            os << '(' << Symbol::toString(s.kind()) << ' ' << *boost::tuples::get<2>(ri) << ' '
               << *boost::tuples::get<3>(ri) << ')';
        } break;
        case Symbol::Kind::Indirect: {
            os << '(' << Symbol::toString(s.kind()) << ' '
               << *s.extra<std::shared_ptr<Production>>() << ')';
        } break;
        case Symbol::Kind::Alternative: {
            os << '(' << Symbol::toString(s.kind());
            for (const auto& it : *s.extrap<std::vector<ProductionPtr>>()) {
                os << ' ' << *it;
            }
            os << ')';
        } break;
        case Symbol::Kind::Symbolic: {
            os << '(' << Symbol::toString(s.kind()) << ' '
               << s.extra<std::weak_ptr<Production>>().lock() << ')';
        } break;
        default:
            os << Symbol::toString(s.kind());
            break;
    }
    return os;
}
}  // namespace parsing
}  // namespace avro

#endif
