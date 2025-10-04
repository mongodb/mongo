/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace mongo {

class GeoHash;
class Box;
struct Point;
std::ostream& operator<<(std::ostream& s, const GeoHash& h);

/* This class maps an unsigned x,y coordinate pair to a hash value.
 * To hash values more interesting than unsigned, use the GeoHashConverter,
 * which maps doubles to unsigned values.
 */
class GeoHash {
public:
    static unsigned int const kMaxBits;  // = 32;

    GeoHash();
    // The strings are binary values of length <= 64,
    // examples: 1001010100101, 1
    explicit GeoHash(const std::string& hash);
    explicit GeoHash(const char* s);
    // bits is how many bits are used to hash each of x and y.
    GeoHash(unsigned x, unsigned y, unsigned bits = 32);
    GeoHash(const GeoHash& old);
    // hash is a raw hash value.  we just copy these into our private fields.
    GeoHash(long long hash, unsigned bits);
    // This only works if e is BinData.  To get a GeoHash from other BSONElements,
    // use the converter class.
    explicit GeoHash(const BSONElement& e, unsigned bits = 32);

    // Convert from the hashed value to unsigned.
    void unhash(unsigned* x, unsigned* y) const;

    /** Is the 'bit'-th most significant bit set?  (NOT the least significant) */
    static bool isBitSet(unsigned val, unsigned bit);

    bool hasPrefix(const GeoHash& other) const;

    std::string toString() const;
    std::string toStringHex1() const;

    void setBit(unsigned pos, bool value);
    bool getBit(unsigned pos) const;

    // XXX comment better
    bool atMinX() const;
    bool atMinY() const;

    // XXX comment better
    bool atMaxX() const;
    bool atMaxY() const;

    // XXX: what does this do
    void move(int x, int y);

    GeoHash& operator=(const GeoHash& h);
    bool operator==(const GeoHash& h) const;
    bool operator!=(const GeoHash& h) const;
    bool operator<(const GeoHash& h) const;

    // Append the minimum range of the hash to the builder provided (inclusive)
    void appendHashMin(BSONObjBuilder* builder, const char* fieldName) const;
    void appendHashMin(key_string::PooledBuilder* ks) const;
    // Append the maximum range of the hash to the builder provided (inclusive)
    void appendHashMax(BSONObjBuilder* builder, const char* fieldName) const;

    long long getHash() const;
    unsigned getBits() const;

    // If this is not a leaf cell, set children[0..3] to the four children of
    // this cell (in traversal order) and return true. Otherwise returns false.
    bool subdivide(GeoHash children[4]) const;
    // Return true if the given cell is contained within this one.
    bool contains(const GeoHash& other) const;
    // Return the parent at given level.
    GeoHash parent(unsigned int level) const;
    GeoHash parent() const;

    // Return the neighbors of closest vertex to this cell at the given level,
    // by appending them to "output".  Normally there are four neighbors, but
    // the closest vertex may only have two or one neighbor if it is next to the
    // boundary.
    //
    // Requires: level < this->_bits, so that we can determine which vertex is
    // closest (in particular, level == kMaxBits is not allowed).
    void appendVertexNeighbors(unsigned level, std::vector<GeoHash>* output) const;

private:
    // Create a hash from the provided string.  Used by the std::string and char* cons.
    void initFromString(const char* s);
    /* Keep the upper _bits*2 bits of _hash, clear the lower bits.
     * Maybe there's junk in there?  XXX Not sure why this is done.
     */
    void clearUnusedBits();
    // XXX: what does this do
    void _move(unsigned offset, int d);

    long long _hash;
    // Bits per field.  Our hash is 64 bits, and we have an X and a Y field,
    // so this is 1 to 32.
    unsigned _bits;
};

/* Convert between various types and the GeoHash. We need additional information (scaling etc.)
 * to convert to/from GeoHash.  The additional information doesn't change often and is the same
 * for all conversions, so we stick all the conversion methods here with their associated
 * data.
 */
class GeoHashConverter {
public:
    static double const kMachinePrecision;  // = 1.1e-16
    static double const kNumBuckets;        // = 4 * 1024 * 1024 * 1024

    struct Parameters {
        // How many bits to use for the hash?
        int bits;
        // X/Y values must be [min, max]
        double min;
        double max;
        // Values are scaled by this when converted to/from hash scale.
        double scaling;
    };

    /**
     * Factory method to return a new instance with status. Uses hashing parameters parsed from a
     * BSONObj.
     */
    static StatusWith<std::unique_ptr<GeoHashConverter>> createFromDoc(const BSONObj& paramDoc);

    /**
     * Factory method to return a new instance with status.
     */
    static StatusWith<std::unique_ptr<GeoHashConverter>> createFromParams(const Parameters& params);

    static double calcUnhashToBoxError(const Parameters& params);

    /**
     * Return converter parameterss which can be used to
     * construct an copy of this converter.
     */
    const Parameters& getParams() const {
        return _params;
    }

    int getBits() const {
        return _params.bits;
    }
    double getError() const {
        return _error;
    }
    double getErrorSphere() const {
        return _errorSphere;
    }
    double getMin() const {
        return _params.min;
    }
    double getMax() const {
        return _params.max;
    }

    double distanceBetweenHashes(const GeoHash& a, const GeoHash& b) const;

    /**
     * Hashing functions.  Convert the following types to a GeoHash:
     * BSONElement
     * BSONObj, BSONObj
     * Point
     * double, double
     */
    GeoHash hash(const Point& p) const;
    GeoHash hash(const BSONElement& e) const;
    // src is printed out as debugging information.  I'm not sure if it's actually
    // somehow the 'source' of o?  Anyway, this is nasty, very nasty.  XXX
    GeoHash hash(const BSONObj& o, const BSONObj* src) const;
    GeoHash hash(double x, double y) const;

    /** Unhashing functions.
     * Convert from a hash to the following types:
     * double, double
     * Point
     * Box
     */
    // XXX: these should have consistent naming
    Point unhashToPoint(const GeoHash& h) const;
    Point unhashToPoint(const BSONElement& e) const;
    void unhash(const GeoHash& h, double* x, double* y) const;

    /**
     * Generates bounding box from geohash, expanded by the error bound
     */
    Box unhashToBoxCovering(const GeoHash& h) const;

    // Return the sizeEdge of a cell at a given level.
    double sizeEdge(unsigned level) const;

    // Used by test.
    double convertDoubleFromHashScale(double in) const;
    double convertToDoubleHashScale(double in) const;

private:
    GeoHashConverter(const Parameters& params);

    void init();

    // Convert from an unsigned in [0, (max-min)*scaling] to [min, max]
    double convertFromHashScale(unsigned in) const;

    // Convert from a double that is [min, max] to an unsigned in [0, (max-min)*scaling]
    unsigned convertToHashScale(double in) const;

    Parameters _params;
    // We compute these based on the _params:
    double _error;
    double _errorSphere;

    // Error bound of unhashToBox, see hash_test.cpp for its proof.
    // 8 * max(|max|, |min|) * u
    double _errorUnhashToBox;
};
}  // namespace mongo
