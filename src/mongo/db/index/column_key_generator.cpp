/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/column_key_generator.h"

#include <iomanip>
#include <ostream>
#include <vector>

#include "mongo/db/storage/column_store.h"
#include "mongo/util/functional.h"
#include "mongo/util/string_map.h"

namespace mongo::column_keygen {
namespace {

/**
 * Special handling for vals because BSONElement doesn't support ==, and we want binaryEqualValues
 * rather than woCompare equality anyway.
 */
bool identicalBSONElementArrays(const std::vector<BSONElement>& lhs,
                                const std::vector<BSONElement>& rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); i++) {
        if (!lhs[i].binaryEqualValues(rhs[i]))
            return false;
    }
    return true;
}

/**
 * This class handles the logic of key generation for columnar indexes. It produces
 * UnencodedCellViews, which have all of the data that should be put in the index values, but it is
 * not responsible for encoding that data into a flat buffer. This final serialization step is
 * handled by the 'writeEncodedCell' function.
 *
 * "Shredding" is an informal term for taking a single BSON document and splitting it into the data
 * for each unique path. The data at each path should be sufficient to reconstruct the object in
 * full fidelity, possibly considering parent paths. When determining the path, array index are not
 * considered, only field names are.
 *
 * This class is private to this file, and only exposed via the free-functions in the header.
 */
class ColumnShredder {
public:
    /**
     * Option to constructor to avoid overhead when you only need to know about the paths in an
     * object, such as for deletes.
     */
    enum PathsAndCells : bool { kOnlyPaths = false, kPathsAndCells = true };

    explicit ColumnShredder(const BSONObj& obj, PathsAndCells pathsAndCells = kPathsAndCells)
        : _pathsAndCells(pathsAndCells) {
        walkObj(_paths[ColumnStore::kRowIdPath], obj, /*isRoot=*/true);
    }

    void visitPaths(function_ref<void(PathView)> cb) const {
        for (auto&& [path, _] : _paths) {
            cb(path);
        }
    }

    void visitCells(function_ref<void(PathView, const UnencodedCellView&)> cb) {
        // This function requires that the shredder was constructed to record cell values.
        invariant(_pathsAndCells);

        for (auto&& [path, cell] : _paths) {
            cb(path, makeCellView(path, cell));
        }
    }

    static void visitDiff(const BSONObj& oldObj,
                          const BSONObj& newObj,
                          function_ref<void(DiffAction, PathView, const UnencodedCellView*)> cb) {
        auto oldShredder = ColumnShredder(oldObj);
        auto newShredder = ColumnShredder(newObj);

        for (auto&& [path, rawCellNew] : newShredder._paths) {
            auto itOld = oldShredder._paths.find(path);
            if (itOld == oldShredder._paths.end()) {
                auto cell = newShredder.makeCellView(path, rawCellNew);
                cb(kInsert, path, &cell);
                continue;
            }

            auto&& [_, rawCellOld] = *itOld;  // For symmetry with rawCellNew.

            // Need to compute sparseness prior to checking cell equality.
            newShredder.computeIsSparse(path, &rawCellNew);
            oldShredder.computeIsSparse(path, &rawCellOld);
            if (rawCellNew == rawCellOld)
                continue;  // No change for this path.


            auto cell = newShredder.makeCellView(path, rawCellNew);
            cb(kUpdate, path, &cell);
        }

        for (auto&& [path, _] : oldShredder._paths) {
            if (!newShredder._paths.contains(path)) {
                cb(kDelete, path, nullptr);
            }
        }
    }


private:
    enum Sparseness : int8_t { kUnknown, kIsSparse, kNotSparse };

    struct RawCellValue {
        // This is where we incrementally build up the full arrayInfo for this cell. While each
        // position is essentially a walk from the root to somewhere in the document, this is a walk
        // that starts at the root and then passes through each value or subobject at this path
        // taking (hopefully) the shortest distance between each value. First we build it up in
        // "uncompressed" format, then we call compressArrayInfo() to remove redundant information.
        std::string arrayInfoBuf;

        // This is used when building up arrayInfoBuf. It records the absolute position of the last
        // value or subobject appended to arrayInfoBuf, so that we can easily compute the relative
        // path to the next value. Equivalently, it is the value of _currentArrayInfo at the time of
        // the last call to appendToArrayInfo(). It never has any '|' or 'o' bytes.
        std::string lastPosition;

        std::vector<BSONElement> vals;
        int nSeen = 0;                     // Number of times this field has been seen.
        int nNonEmptySubobjects = 0;       // Number of values that are non-empty objects
        Sparseness sparseness = kUnknown;  // Memoized in computeIsSparse()
        bool childrenMustBeSparse = false;
        bool hasDoubleNestedArrays = false;
        bool hasDuplicateFields = false;

        bool operator==(const RawCellValue& rhs) const {
            const RawCellValue& lhs = *this;

            // Sparseness must have already been resolved.
            invariant(lhs.sparseness != kUnknown);
            invariant(rhs.sparseness != kUnknown);

            // Intentionally not checking nSeen or childrenMustBeSparse, since they are just inputs
            // to sparseness computation.

            // Ordered to test cheaper stuff before more expensive stuff.
            return lhs.sparseness == rhs.sparseness &&                     //
                lhs.hasDoubleNestedArrays == rhs.hasDoubleNestedArrays &&  //
                lhs.nNonEmptySubobjects == rhs.nNonEmptySubobjects &&      //
                lhs.hasDuplicateFields == rhs.hasDuplicateFields &&        //
                lhs.arrayInfoBuf == rhs.arrayInfoBuf &&                    //
                identicalBSONElementArrays(lhs.vals, rhs.vals);
        }
    };

    UnencodedCellView makeCellView(PathView path, RawCellValue& cell) {
        compressArrayInfo(cell.arrayInfoBuf);
        return {
            cell.vals,
            cell.arrayInfoBuf,
            cell.hasDuplicateFields,
            /*hasSubPaths*/ cell.nNonEmptySubobjects != 0,
            computeIsSparse(path, &cell),
            cell.hasDoubleNestedArrays,
        };
    }

    // Memoized, so amortized O(1) when called on all paths, even though it must check all parent
    // paths.
    bool computeIsSparse(PathView path, RawCellValue* cell) {
        if (cell->sparseness != kUnknown)
            return cell->sparseness == kIsSparse;

        auto parentPath = parentPathOf(path);
        if (!parentPath) {
            // Top level fields are never considered sparse.
            cell->sparseness = kNotSparse;
            return false;
        }

        auto parentIt = _paths.find(*parentPath);
        invariant(parentIt != _paths.end());
        auto& parent = parentIt->second;
        const bool isSparse = parent.childrenMustBeSparse ||
            cell->nSeen != parent.nNonEmptySubobjects || computeIsSparse(*parentPath, &parent);
        cell->sparseness = isSparse ? kIsSparse : kNotSparse;
        return isSparse;
    }

    void walkObj(RawCellValue& cell, const BSONObj& obj, bool isRoot = false) {
        if (_pathsAndCells) {
            cell.nNonEmptySubobjects++;
            if (!isRoot) {
                appendToArrayInfo(cell, 'o');
            }
        }

        ON_BLOCK_EXIT([&, oldArrInfoSize = _currentArrayInfo.size()] {
            if (_pathsAndCells)
                _currentArrayInfo.resize(oldArrInfoSize);
        });
        if (_pathsAndCells && !isRoot)
            _currentArrayInfo += '{';

        for (auto [name, elem] : obj) {
            // We skip fields in some edge cases such as dots in the field name. This may also throw
            // if the field name contains invalid UTF-8 in a way that would break the index.
            if (shouldSkipField(name))
                continue;

            ON_BLOCK_EXIT([&, oldPathSize = _currentPath.size()] {  //
                _currentPath.resize(oldPathSize);
            });
            if (!isRoot)
                _currentPath += '.';
            _currentPath += name;

            auto& subCell = _paths[_currentPath];
            subCell.nSeen++;
            if (_inDoubleNestedArray)
                subCell.hasDoubleNestedArrays = true;
            handleElem(subCell, elem);
        }
    }

    void walkArray(RawCellValue& cell, const BSONObj& arr) {
        DecimalCounter<unsigned> index;

        for (auto elem : arr) {
            ON_BLOCK_EXIT([&,
                           oldArrInfoSize = _currentArrayInfo.size(),
                           oldInDoubleNested = _inDoubleNestedArray] {
                if (_pathsAndCells) {
                    _currentArrayInfo.resize(oldArrInfoSize);
                    _inDoubleNestedArray = oldInDoubleNested;
                }
            });

            if (_pathsAndCells) {
                _currentArrayInfo += '[';
                if (index == 0) {
                    // Zero is common so make it implied rather than explicit.
                } else {
                    // Theoretically, index should be the same as elem.fieldNameStringData(),
                    // however, since we don't validate the array indexes, they cannot be trusted.
                    // The logic to encode array info relies on "sane" array indexes (at the very
                    // least they must be monotonically increasing), so we create a new index string
                    // here.
                    _currentArrayInfo += StringData(index);
                }
                ++index;

                // [] doesn't start a double nesting since {a:{$eq: []}} matches {a: [[]]}
                if (elem.type() == Array && !elem.Obj().isEmpty())
                    _inDoubleNestedArray = true;
            }

            // Note: always same cell, since array traversal never changes path.
            handleElem(cell, elem);
        }
    }

    void handleElem(RawCellValue& cell, const BSONElement& elem) {
        // Only recurse on non-empty objects and arrays. Empty objects and arrays are handled as
        // scalars.
        if (elem.type() == Object) {
            if (auto obj = elem.Obj(); !obj.isEmpty())
                return walkObj(cell, obj);
        } else if (elem.type() == Array) {
            if (auto obj = elem.Obj(); !obj.isEmpty())
                return walkArray(cell, obj);
        }

        if (_pathsAndCells) {
            // If we get here, then this walk will not have any children. This means that there is
            // at least one path where all children (if any) will be missing structural information,
            // so they will need to consult the parent path.
            cell.childrenMustBeSparse = true;

            if (_inDoubleNestedArray)
                cell.hasDoubleNestedArrays = true;

            cell.vals.push_back(elem);
            appendToArrayInfo(cell, '|');
        }
    }

    void appendToArrayInfo(RawCellValue& rcd, char finalByte) {
        dassert(finalByte == '|' || finalByte == 'o');

        auto foundDuplicateField = [&] {
            rcd.arrayInfoBuf.clear();
            rcd.hasDuplicateFields = true;
        };

        if (rcd.hasDuplicateFields) {
            // arrayInfo should be left empty in this case.
            invariant(rcd.arrayInfoBuf.empty());
            return;
        }

        ON_BLOCK_EXIT([&] { rcd.lastPosition = _currentArrayInfo; });

        if (rcd.arrayInfoBuf.empty()) {
            // The first time we get a position for this path, just record it verbatim. The first
            // is special because we are essentially recording the absolute path to this value,
            // while every other time we append the path relative to the prior value.
            invariant(rcd.lastPosition.empty());
            rcd.arrayInfoBuf.reserve(_currentArrayInfo.size() + 1);
            rcd.arrayInfoBuf += _currentArrayInfo;
            rcd.arrayInfoBuf += finalByte;
            return;
        }

        // Make better names for symmetry (and to prevent accidental modifications):
        StringData oldPosition = rcd.lastPosition;
        StringData newPosition = _currentArrayInfo;

        if (MONGO_unlikely(oldPosition.empty() || newPosition.empty())) {
            // This can only happen if there is a duplicate field at the top level.
            return foundDuplicateField();
        }

        auto [oldIt, newIt] = std::mismatch(oldPosition.begin(),  //
                                            oldPosition.end(),
                                            newPosition.begin(),
                                            newPosition.end());
        if (MONGO_unlikely(newIt == newPosition.end())) {
            // This can only happen if there is a duplicate field in an array, because otherwise
            // the raw array infos must differ by an index in some array.
            return foundDuplicateField();
        }

        // Walk back to start of differing elem. Important to use newIt here because if they are
        // in the same array, oldIt may have an implicitly encoded 0 index, while newIt must
        // have a higher index.
        while (*newIt != '[') {
            if (MONGO_unlikely(!(*newIt >= '0' && *newIt <= '9'))) {
                // Non-index difference can only happen if there are duplicate fields in an array.
                return foundDuplicateField();
            }
            invariant(newIt > newPosition.begin());
            dassert(oldIt > oldPosition.begin());  // oldIt and newIt are at same index.
            --newIt;
            --oldIt;
        }
        invariant(oldIt < oldPosition.end());
        if (MONGO_unlikely(*oldIt != '[')) {
            // This is another type of non-index difference.
            return foundDuplicateField();
        }

        // Close out arrays past the first mismatch in LIFO order.
        for (auto revOldIt = oldPosition.end() - 1; revOldIt != oldIt; --revOldIt) {
            if (*revOldIt == '[') {
                rcd.arrayInfoBuf += ']';
            } else {
                dassert((*revOldIt >= '0' && *revOldIt <= '9') || *revOldIt == '{');
            }
        }

        // Now process the mismatch. It must be a difference in array index (checked above).
        dassert(*oldIt == '[' && *newIt == '[');
        ++oldIt;
        ++newIt;
        const auto oldIx = ColumnStore::readArrInfoNumber(&oldIt, oldPosition.end());
        const auto newIx = ColumnStore::readArrInfoNumber(&newIt, newPosition.end());

        if (MONGO_unlikely(newIx <= oldIx)) {
            // If this element is at a same or lower index, we must have hit a duplicate field
            // above and restarted the array indexing.
            return foundDuplicateField();
        }
        const auto delta = newIx - oldIx;
        const auto skips = delta - 1;
        if (skips == 0) {
            // Nothing. Increment by one (skipping zero) is implicit.
        } else {
            rcd.arrayInfoBuf += '+';
            rcd.arrayInfoBuf += ItoA(skips);
        }

        // Now put the rest of the new array info into the output buffer.
        rcd.arrayInfoBuf += StringData(newIt, newPosition.end());
        rcd.arrayInfoBuf += finalByte;
    }

    static void compressArrayInfo(std::string& arrayInfo) {
        // NOTE: all operations in this function either shrink the array info or keep it the same
        // size, so they are able to work in-place in the arrayInfo buffer.

        // Logic below assumes arrayInfo is null terminated as a simplification.
        dassert(strlen(arrayInfo.data()) == arrayInfo.size());

        const char* in = arrayInfo.data();
        char* out = arrayInfo.data();
        bool anyArrays = false;

        // Remove all '{' immediately before a '|' or 'o', and check for arrays
        char* lastNonBrace = nullptr;
        while (auto c = *in++) {
            if (c == '[') {
                anyArrays = true;
            } else if ((c == '|' || c == 'o') && lastNonBrace) {
                // Rewind output to just past last non-brace, since the last set of opens are
                // encoded implicitly.
                dassert(lastNonBrace + 1 <= out);
                out = lastNonBrace + 1;
            }

            if (c != '{')
                lastNonBrace = out;

            *out++ = c;
        }

        // If there were no arrays, we don't need any array info.
        if (!anyArrays) {
            arrayInfo.clear();
            return;
        }

        invariant(size_t(out - arrayInfo.data()) <= arrayInfo.size());

        // Remove final run of '|' since end implies an infinite sequence of them.
        while (out - arrayInfo.data() >= 1 && out[-1] == '|') {
            out--;
        }
        arrayInfo.resize(out - arrayInfo.data());  // Reestablishes null termination.

        // Now do a final pass to RLE remaining runs of '|' or 'o'. It may be possible to integrate
        // this with the first loop above, but it would be a bit more complicated because we need
        // to have removed any implicit runs of '{' prior to each '|' or 'o' before looking for
        // runs.
        out = arrayInfo.data();
        in = arrayInfo.data();
        while (auto c = *in++) {
            *out++ = c;
            if (c != '|' && c != 'o')
                continue;

            size_t repeats = 0;
            while (*in == c) {
                repeats++;
                in++;
            }

            if (repeats) {
                auto buf = ItoA(repeats);  // Must be on own line so the buffer outlives numStr.
                auto numStr = StringData(buf);

                // We know there is room because a number > 0 takes up no more space than the number
                // of '|' that it is replacing. repeats == 1 is the worst case because it isn't
                // saving any space. However we still replace here since it should make decoding
                // slightly faster.
                dassert(numStr.size() <= repeats);
                for (char c : numStr) {
                    dassert(out < in);  // Note: `in` has already been advanced.
                    *out++ = c;
                }
            }
        }

        invariant(size_t(out - arrayInfo.data()) <= arrayInfo.size());
        arrayInfo.resize(out - arrayInfo.data());
    }

    static bool shouldSkipField(PathView fieldName) {
        // TODO consider skipping when the nesting depths exceed our documented limits of 100
        // documents. This would allow decoding to use fixed-size bitsets without needing to check
        // for overflow.

        // WARNING: do not include the field name in error messages because if we throw from this
        // function then the field name isn't valid utf-8, and could poison the whole error message!

        // We only care about \xFF at the beginning, even though utf-8 bans it everywhere.
        static_assert(ColumnStore::kRowIdPath == "\xFF"_sd);
        uassert(6519200,
                "Field name contains '\\xFF' which isn't valid in UTF-8",
                fieldName[0] != '\xFF');  // We know field names are nul-terminated so this is safe.

        // Fields with dots are not illegal, but we skip them because a) the query optimizer can't
        // correctly track dependencies with them, and b) the current storage format uses dots as a
        // separator and everything (including the array info encoder) would get confused with
        // documents like {a: {b:1}, a.b: 2} because that breaks some of the assumptions.
        return fieldName.find('.') != std::string::npos;
    }

    static boost::optional<PathView> parentPathOf(PathView path) {
        auto sep = path.rfind('.');
        if (sep == std::string::npos)
            return {};

        return path.substr(0, sep);
    }

    // This is the same as StringMap<V> but with node_hash_map. We need the reference stability
    // guarantee because we have recursive functions that both insert into _paths and hold a
    // reference to one of its values. It seems better to use a node-based hash table than to redo
    // the lookup every time we want to touch a cell.
    template <typename V>
    using NodeStringMap = absl::node_hash_map<std::string, V, StringMapHasher, StringMapEq>;

    PathValue _currentPath;
    std::string _currentArrayInfo;  // Describes a walk from root to current pos. No '|' or 'o'.
    NodeStringMap<RawCellValue> _paths;
    bool _inDoubleNestedArray = false;

    const PathsAndCells _pathsAndCells = kPathsAndCells;
};
}  // namespace

void visitCellsForInsert(const BSONObj& obj,
                         function_ref<void(PathView, const UnencodedCellView&)> cb) {
    ColumnShredder(obj).visitCells(cb);
}

void visitPathsForDelete(const BSONObj& obj, function_ref<void(PathView)> cb) {
    ColumnShredder(obj, ColumnShredder::kOnlyPaths).visitPaths(cb);
}

void visitDiffForUpdate(const BSONObj& oldObj,
                        const BSONObj& newObj,
                        function_ref<void(DiffAction, PathView, const UnencodedCellView*)> cb) {
    ColumnShredder::visitDiff(oldObj, newObj, cb);
}

bool operator==(const UnencodedCellView& lhs, const UnencodedCellView& rhs) {
    if (lhs.hasDuplicateFields || rhs.hasDuplicateFields) {
        // As a special case, if either is true, we only care about comparing this field, since all
        // other fields are suspect. This simplifies testing, because we don't need to guess what
        // the output will be (and lock it into a correctness test!) for the case with duplicate
        // fields.
        return lhs.hasDuplicateFields == rhs.hasDuplicateFields;
    }

    return identicalBSONElementArrays(lhs.vals, rhs.vals) &&  //
        lhs.arrayInfo == rhs.arrayInfo &&                     //
        lhs.hasSubPaths == rhs.hasSubPaths &&                 //
        lhs.isSparse == rhs.isSparse &&                       //
        lhs.hasDoubleNestedArrays == rhs.hasDoubleNestedArrays;
}

std::ostream& operator<<(std::ostream& os, const UnencodedCellView& cell) {
    if (cell.hasDuplicateFields) {
        // As a special case just output this info, since other fields don't matter.
        return os << "{duplicateFields: 1}";
    }

    os << "{vals: [";
    for (auto&& elem : cell.vals) {
        if (&elem != &cell.vals.front())
            os << ", ";
        os << elem.toString(/*includeFieldName*/ false);
    }
    os << "], arrayInfo: '" << cell.arrayInfo;
    os << "', hasSubPaths: " << cell.hasSubPaths;
    os << ", isSparse: " << cell.isSparse;
    os << ", hasDoubleNestedArrays: " << cell.hasDoubleNestedArrays;
    os << '}';

    return os;
}
std::ostream& operator<<(std::ostream& os, const UnencodedCellView* cell) {
    return cell ? (os << *cell) : (os << "(no cell)");
}
}  // namespace mongo::column_keygen
