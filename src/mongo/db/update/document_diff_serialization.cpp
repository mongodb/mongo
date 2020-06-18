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
        uassert(470521,
                str::stream() << "Expected " << kArrayHeader << " field to be bool but got" << *it,
                (*it).type() == BSONType::Bool);

        uassert(470522,
                str::stream() << "Expected " << kArrayHeader << " field to be true but got" << *it,
                (*it).Bool());
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

DocumentDiffBuilder ArrayDiffBuilder::startSubObjDiff(size_t idx) {
    invariant(!_childSubDiffIndex);
    _childSubDiffIndex = idx;
    return DocumentDiffBuilder(this);
}
ArrayDiffBuilder ArrayDiffBuilder::startSubArrDiff(size_t idx) {
    invariant(!_childSubDiffIndex);
    _childSubDiffIndex = idx;
    return ArrayDiffBuilder(this);
}

void ArrayDiffBuilder::addUpdate(size_t idx, BSONElement elem) {
    _modifications.push_back({idx, elem});
}

void ArrayDiffBuilder::releaseTo(BSONObjBuilder* output) {
    output->append(kArrayHeader, true);
    if (_newSize) {
        output->append(kResizeSectionFieldName, *_newSize);
    }

    for (auto&& [idx, modification] : _modifications) {
        stdx::visit(
            visit_helper::Overloaded{
                [idx = idx, output](const Diff& subDiff) {
                    // TODO: SERVER-48602 Try to avoid using BSON macro here. Ideally we will just
                    // need one BSONObjBuilder for serializing the diff, at the end.
                    output->append(std::to_string(idx), BSON(kSubDiffSectionFieldName << subDiff));
                },
                [idx = idx, output](BSONElement elt) {
                    output->append(std::to_string(idx), elt.wrap(kUpdateSectionFieldName));
                }},
            modification);
    }
}

void DocumentDiffBuilder::releaseTo(BSONObjBuilder* output) {
    if (!_deletes.asTempObj().isEmpty()) {
        BSONObjBuilder subBob(output->subobjStart(kDeleteSectionFieldName));
        subBob.appendElements(_deletes.done());
    }

    if (!_updates.asTempObj().isEmpty()) {
        BSONObjBuilder subBob(output->subobjStart(kUpdateSectionFieldName));
        subBob.appendElements(_updates.done());
    }

    if (!_inserts.asTempObj().isEmpty()) {
        BSONObjBuilder subBob(output->subobjStart(kInsertSectionFieldName));
        subBob.appendElements(_inserts.done());
    }

    if (!_subDiffs.asTempObj().isEmpty()) {
        BSONObjBuilder subBob(output->subobjStart(kSubDiffSectionFieldName));
        subBob.appendElements(_subDiffs.done());
    }
}

DocumentDiffBuilder DocumentDiffBuilder::startSubObjDiff(StringData field) {
    invariant(!_childSubDiffField);
    _childSubDiffField = field.toString();
    return DocumentDiffBuilder(this);
}
ArrayDiffBuilder DocumentDiffBuilder::startSubArrDiff(StringData field) {
    invariant(!_childSubDiffField);
    _childSubDiffField = field.toString();
    return ArrayDiffBuilder(this);
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
    const size_t idx = extractArrayIndex(next.fieldNameStringData());
    uassert(4770514,
            str::stream() << "expected object at index " << idx << " in array diff but got "
                          << next,
            next.type() == BSONType::Object);

    const auto nextAsObj = next.embeddedObject();
    uassert(4770521,
            str::stream() << "expected single-field object at index " << idx
                          << " in array diff but got " << nextAsObj,
            nextAsObj.nFields() == 1);
    if (nextAsObj.firstElementFieldNameStringData() == kUpdateSectionFieldName) {
        // It's an update.
        return {{idx, nextAsObj.firstElement()}};
    } else if (nextAsObj.firstElementFieldNameStringData() == kSubDiffSectionFieldName) {
        // It's a sub diff...But which type?
        uassert(4770501,
                str::stream() << "expected sub diff at index " << idx << " but got " << nextAsObj,
                nextAsObj.firstElement().type() == BSONType::Object);

        auto modification =
            stdx::visit(visit_helper::Overloaded{[](const auto& reader) -> ArrayModification {
                            return {reader};
                        }},
                        getReader(nextAsObj.firstElement().embeddedObject()));
        return {{idx, modification}};
    } else {
        uasserted(4770502,
                  str::stream() << "Expected either 'u' (update) or 's' (sub diff) at index " << idx
                                << " but got " << nextAsObj);
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
