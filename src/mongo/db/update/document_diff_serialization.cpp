/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/document_diff_serialization.h"

#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo::doc_diff {
namespace {
static const StringDataSet kDocumentDiffSections = {kInsertSectionFieldName,
                                                    kUpdateSectionFieldName,
                                                    kDeleteSectionFieldName,
                                                    kSubDiffSectionFieldName};

void assertDiffNonEmpty(const BSONObjIterator& it) {
    uassert(4770500, "Expected diff to be non-empty", it.more());
}

// Helper for taking a BSONObj and determining whether it's an array diff or an object diff.
doc_diff::DiffType identifyType(const BSONObj& diff) {
    BSONObjIterator it(diff);
    assertDiffNonEmpty(it);

    if ((*it).fieldNameStringData() == kArrayHeader) {
        return DiffType::kArray;
    }
    return DiffType::kDocument;
}

stdx::variant<DocumentDiffReader, ArrayDiffReader> getReader(const Diff& diff) {
    const auto type = identifyType(diff);
    if (type == DiffType::kArray) {
        return ArrayDiffReader(diff);
    }
    return DocumentDiffReader(diff);
}
}  // namespace

SubBuilderGuard<DocumentDiffBuilder> ArrayDiffBuilder::startSubObjDiff(size_t idx) {
    auto subDiffBuilder = std::make_unique<DocumentDiffBuilder>(0);
    DocumentDiffBuilder* subBuilderPtr = subDiffBuilder.get();
    _modifications.push_back({std::to_string(idx), std::move(subDiffBuilder)});
    return SubBuilderGuard<DocumentDiffBuilder>(this, subBuilderPtr);
}
SubBuilderGuard<ArrayDiffBuilder> ArrayDiffBuilder::startSubArrDiff(size_t idx) {
    auto subDiffBuilder = std::unique_ptr<ArrayDiffBuilder>(new ArrayDiffBuilder());
    ArrayDiffBuilder* subBuilderPtr = subDiffBuilder.get();
    _modifications.push_back({std::to_string(idx), std::move(subDiffBuilder)});
    return SubBuilderGuard<ArrayDiffBuilder>(this, subBuilderPtr);
}

void ArrayDiffBuilder::addUpdate(size_t idx, BSONElement elem) {
    auto fieldName = std::to_string(idx);
    sizeTracker.addEntry(fieldName.size() + kUpdateSectionFieldName.size(), elem.valuesize());
    _modifications.push_back({std::move(fieldName), elem});
}

void ArrayDiffBuilder::serializeTo(BSONObjBuilder* output) const {
    output->append(kArrayHeader, true);
    if (_newSize) {
        output->append(kResizeSectionFieldName, *_newSize);
    }

    for (auto&& modificationEntry : _modifications) {
        auto&& idx = modificationEntry.first;
        auto&& modification = modificationEntry.second;
        stdx::visit(
            visit_helper::Overloaded{
                [&idx, output](const std::unique_ptr<DiffBuilderBase>& subDiff) {
                    BSONObjBuilder subObjBuilder =
                        output->subobjStart(kSubDiffSectionFieldName + idx);
                    subDiff->serializeTo(&subObjBuilder);
                },
                [&idx, output](BSONElement elt) {
                    output->appendAs(elt, kUpdateSectionFieldName + idx);
                }},
            modification);
    }
}

void DocumentDiffBuilder::serializeTo(BSONObjBuilder* output) const {
    if (!_deletes.empty()) {
        BSONObjBuilder subBob(output->subobjStart(kDeleteSectionFieldName));
        for (auto&& del : _deletes) {
            subBob.append(del, false);
        }
    }

    if (!_updates.empty()) {
        BSONObjBuilder subBob(output->subobjStart(kUpdateSectionFieldName));
        for (auto&& update : _updates) {
            subBob.appendAs(update.second, update.first);
        }
    }

    if (!_inserts.empty()) {
        BSONObjBuilder subBob(output->subobjStart(kInsertSectionFieldName));
        for (auto&& insert : _inserts) {
            subBob.appendAs(insert.second, insert.first);
        }
    }

    if (!_subDiffs.empty()) {
        BSONObjBuilder subBob(output->subobjStart(kSubDiffSectionFieldName));
        for (auto&& subDiff : _subDiffs) {
            BSONObjBuilder subDiffBuilder(subBob.subobjStart(subDiff.first));
            subDiff.second->serializeTo(&subDiffBuilder);
        }
    }
}

SubBuilderGuard<DocumentDiffBuilder> DocumentDiffBuilder::startSubObjDiff(StringData field) {
    auto subDiffBuilder = std::make_unique<DocumentDiffBuilder>(0);
    DocumentDiffBuilder* subBuilderPtr = subDiffBuilder.get();
    _subDiffs.push_back({field, std::move(subDiffBuilder)});
    return SubBuilderGuard<DocumentDiffBuilder>(this, subBuilderPtr);
}
SubBuilderGuard<ArrayDiffBuilder> DocumentDiffBuilder::startSubArrDiff(StringData field) {
    auto subDiffBuilder = std::unique_ptr<ArrayDiffBuilder>(new ArrayDiffBuilder());
    ArrayDiffBuilder* subBuilderPtr = subDiffBuilder.get();
    _subDiffs.push_back({field, std::move(subDiffBuilder)});
    return SubBuilderGuard<ArrayDiffBuilder>(this, subBuilderPtr);
}

namespace {
void checkSection(BSONObjIterator* it, StringData sectionName, BSONType expectedType) {
    uassert(4770507,
            str::stream() << "Expected " << sectionName << " section to be type " << expectedType,
            (**it).type() == expectedType);
}
}  // namespace

ArrayDiffReader::ArrayDiffReader(const Diff& diff) : _diff(diff), _it(_diff) {
    assertDiffNonEmpty(_it);

    uassert(4770504,
            str::stream() << "Expected first field to be array header " << kArrayHeader
                          << " but found " << (*_it).fieldNameStringData(),
            (*_it).fieldNameStringData() == kArrayHeader);
    uassert(4770519,
            str::stream() << "Expected array header to be bool but got " << (*_it),
            (*_it).type() == BSONType::Bool);
    uassert(4770520,
            str::stream() << "Expected array header to be value true but got " << (*_it),
            (*_it).Bool());
    ++_it;

    if (_it.more() && (*_it).fieldNameStringData() == kResizeSectionFieldName) {
        checkSection(&_it, kResizeSectionFieldName, BSONType::NumberInt);
        _newSize.emplace((*_it).numberInt());
        ++_it;
    }
}

namespace {
// Converts a (decimal) string to number. Will throw if the string is not a valid unsigned int.
size_t extractArrayIndex(StringData fieldName) {
    auto idx = str::parseUnsignedBase10Integer(fieldName);
    uassert(4770512, str::stream() << "Expected integer but got " << fieldName, idx);
    return *idx;
}
}  // namespace

boost::optional<std::pair<size_t, ArrayDiffReader::ArrayModification>> ArrayDiffReader::next() {
    if (!_it.more()) {
        return {};
    }

    auto next = _it.next();
    auto fieldName = next.fieldNameStringData();

    static_assert(kUpdateSectionFieldName.size() == 1 && kSubDiffSectionFieldName.size() == 1,
                  "The code below assumes that the field names used in the diff format are single "
                  "character long");
    uassert(4770521,
            str::stream() << "expected field name to be at least two characters long, but found: "
                          << fieldName,
            fieldName.size() > 1);
    const size_t idx = extractArrayIndex(fieldName.substr(1, fieldName.size()));

    if (fieldName[0] == kUpdateSectionFieldName[0]) {
        // It's an update.
        return {{idx, next}};
    } else if (fieldName[0] == kSubDiffSectionFieldName[0]) {
        // It's a sub diff...But which type?
        uassert(4770501,
                str::stream() << "expected sub diff at index " << idx << " but got " << next,
                next.type() == BSONType::Object);

        auto modification =
            stdx::visit(visit_helper::Overloaded{[](const auto& reader) -> ArrayModification {
                            return {reader};
                        }},
                        getReader(next.embeddedObject()));
        return {{idx, modification}};
    } else {
        uasserted(4770502,
                  str::stream() << "Expected either 'u' (update) or 's' (sub diff) at index " << idx
                                << " but got " << next);
    }
}

DocumentDiffReader::DocumentDiffReader(const Diff& diff) : _diff(diff) {
    BSONObjIterator it(diff);
    assertDiffNonEmpty(it);

    // Find each seection of the diff and initialize an iterator.
    struct Section {
        StringData fieldName;
        boost::optional<BSONObjIterator>* outIterator;
    };

    const std::array<Section, 4> sections{
        Section{kDeleteSectionFieldName, &_deletes},
        Section{kUpdateSectionFieldName, &_updates},
        Section{kInsertSectionFieldName, &_inserts},
        Section{kSubDiffSectionFieldName, &_subDiffs},
    };

    for (size_t i = 0; i < sections.size(); ++i) {
        if (!it.more()) {
            break;
        }

        const auto fieldName = (*it).fieldNameStringData();
        if (fieldName != sections[i].fieldName) {
            uassert(4770503,
                    str::stream() << "Unexpected section: " << fieldName << " in document diff",
                    kDocumentDiffSections.count(fieldName) != 0);

            continue;
        }

        checkSection(&it, sections[i].fieldName, BSONType::Object);
        sections[i].outIterator->emplace((*it).embeddedObject());
        ++it;
    }

    uassert(4770513,
            str::stream() << "Did not expect more sections in diff but found one: "
                          << (*it).fieldNameStringData(),
            !it.more());
}

boost::optional<StringData> DocumentDiffReader::nextDelete() {
    if (!_deletes || !_deletes->more()) {
        return {};
    }

    return _deletes->next().fieldNameStringData();
}

boost::optional<BSONElement> DocumentDiffReader::nextUpdate() {
    if (!_updates || !_updates->more()) {
        return {};
    }
    return _updates->next();
}

boost::optional<BSONElement> DocumentDiffReader::nextInsert() {
    if (!_inserts || !_inserts->more()) {
        return {};
    }
    return _inserts->next();
}

boost::optional<std::pair<StringData, stdx::variant<DocumentDiffReader, ArrayDiffReader>>>
DocumentDiffReader::nextSubDiff() {
    if (!_subDiffs || !_subDiffs->more()) {
        return {};
    }

    auto next = _subDiffs->next();
    uassert(470510,
            str::stream() << "Subdiffs should be objects, got " << next,
            next.type() == BSONType::Object);

    return {{next.fieldNameStringData(), getReader(next.embeddedObject())}};
}
}  // namespace mongo::doc_diff
