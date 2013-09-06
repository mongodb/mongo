/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/geo/core.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/mongoutils/str.h"

// So we can get at the str namespace.
using namespace mongoutils;

namespace mongo {

    inline std::ostream& operator<<(std::ostream &s, const GeoHash &h) {
        s << h.toString();
        return s;
    }

    /* 
     * GeoBitSets fills out various bit patterns that are used by GeoHash.
     * What patterns?  Look at the comments next to the fields.
     * TODO(hk): hashedToNormal is still a bit of a mystery.
     */
    class GeoBitSets {
    public:
        GeoBitSets() {
            for (unsigned i = 0; i < 16; i++) {
                unsigned fixed = 0;
                for (int j = 0; j < 4; j++) {
                    if (i & (1 << j))
                        fixed |= (1 << (j * 2));
                }
                hashedToNormal[fixed] = i;
            }

            long long currAllX = 0, currAllY = 0;
            for (int i = 0; i < 64; i++){
                long long thisBit = 1LL << (63 - i);

                if (i % 2 == 0) {
                    allX[i / 2] = currAllX;
                    currAllX |= thisBit;
                } else{
                    allY[i / 2] = currAllY;
                    currAllY |= thisBit;
                }
            }
        }

        // The 0-th entries of each all[XY] is 0.
        // The i-th entry of allX has i alternating bits turned on starting
        // with the most significant.  Example:
        // allX[1] = 8000000000000000
        // allX[2] = a000000000000000
        // allX[3] = a800000000000000
        long long allX[32];
        // Same alternating bits but starting with one from the MSB:
        // allY[1] = 4000000000000000
        // allY[2] = 5000000000000000
        // allY[3] = 5400000000000000
        long long allY[32];

        unsigned hashedToNormal[256];
    };

    // Oh global variables.
    GeoBitSets geoBitSets;

    // For i return the i-th most significant bit.
    // masks(0) = 80000..000
    // masks(1) = 40000..000
    // etc.
    // Number of 0s depends on 32 vs. 64 bit.
    inline static int mask32For(const int i) {
        return 1 << (31 - i);
    }

    inline static long long mask64For(const int i) {
        return 1LL << (63 - i);
    }

    /* This class maps an x,y coordinate pair to a hash value.
     * This should probably be renamed/generalized so that it's more of a planar hash,
     * and we also have a spherical hash, etc.
     */
    GeoHash::GeoHash() : _hash(0), _bits(0) { }

    GeoHash::GeoHash(const string& hash) {
        initFromString(hash.c_str());
    }

    GeoHash::GeoHash(const char *s) {
        initFromString(s);
    }

    void GeoHash::initFromString(const char *s) {
        int length = strlen(s);
        uassert(16457, "initFromString passed a too-long string", length <= 64);
        uassert(16458, "initFromString passed an odd length string ", 0 == (length % 2));
        _hash = 0;
        // _bits is how many bits for X or Y, not both, so we divide by 2.
        _bits = length / 2;
        for (int i = 0; s[i] != '\0'; ++i)
            if (s[i] == '1')
                setBit(i, 1);
    }

    // This only works if e is BinData.
    GeoHash::GeoHash(const BSONElement& e, unsigned bits) {
        _bits = bits;
        if (e.type() == BinData) {
            int len = 0;
            _copyAndReverse((char*)&_hash, e.binData(len));
            verify(len == 8);
        } else {
            cout << "GeoHash bad element: " << e << endl;
            uassert(13047, "wrong type for geo index. if you're using a pre-release version,"
                           " need to rebuild index", 0);
        }
        clearUnusedBits();
    }

    GeoHash::GeoHash(unsigned x, unsigned y, unsigned bits) {
        verify(bits <= 32);
        _hash = 0;
        _bits = bits;
        for (unsigned i = 0; i < bits; i++) {
            if (isBitSet(x, i)) _hash |= mask64For(i * 2);
            if (isBitSet(y, i)) _hash |= mask64For((i * 2) + 1);
        }
    }

    GeoHash::GeoHash(const GeoHash& old) {
        _hash = old._hash;
        _bits = old._bits;
    }

    GeoHash::GeoHash(long long hash, unsigned bits) : _hash(hash) , _bits(bits) {
        clearUnusedBits();
    }

    // TODO(hk): This is nasty and has no examples.
    void GeoHash::unhash_fast(unsigned *x, unsigned *y) const {
        *x = 0;
        *y = 0;
        const char *c = reinterpret_cast<const char*>(&_hash);
        for (int i = 0; i < 8; i++) {
            unsigned t = (unsigned)(c[i]) & 0x55;
            *y |= (geoBitSets.hashedToNormal[t] << (4 * i));

            t = ((unsigned)(c[i]) >> 1) & 0x55;
            *x |= (geoBitSets.hashedToNormal[t] << (4 * i));
        }
    }

    void GeoHash::unhash_slow(unsigned *x, unsigned *y) const {
        *x = 0;
        *y = 0;
        for (unsigned i = 0; i < _bits; i++) {
            if (getBitX(i))
                *x |= mask32For(i);
            if (getBitY(i))
                *y |= mask32For(i);
        }
    }

    void GeoHash::unhash(unsigned *x, unsigned *y) const {
        unhash_fast(x, y);
    }

    /** Is the 'bit'-th most significant bit set?  (NOT the least significant) */
    bool GeoHash::isBitSet(unsigned val, unsigned bit) {
        return mask32For(bit) & val;
    }

    /** Return a GeoHash with one bit of precision lost. */
    GeoHash GeoHash::up() const {
        return GeoHash(_hash, _bits - 1);
    }

    bool GeoHash::hasPrefix(const GeoHash& other) const {
        verify(other._bits <= _bits);
        if (other._bits == 0)
            return true;

        long long x = other._hash ^ _hash;
        // We only care about the leftmost other._bits (well, really _bits*2 since we have x and
        // y)
        x = x >> (64 - (other._bits * 2));
        return x == 0;
    }

    string GeoHash::toString() const {
        StringBuilder buf;
        for (unsigned x = 0; x < _bits * 2; x++)
            buf.append((_hash & mask64For(x)) ? "1" : "0");
        return buf.str();
    }

    string GeoHash::toStringHex1() const {
        stringstream ss;
        ss << hex << _hash;
        return ss.str();
    }

    void GeoHash::setBit(unsigned pos, bool value) {
        verify(pos < _bits * 2);
        const long long mask = mask64For(pos);
        if (value)
            _hash |= mask;
        else  // if (_hash & mask)
            _hash &= ~mask;
    }

    bool GeoHash::getBit(unsigned pos) const {
        return _hash & mask64For(pos);
    }

    bool GeoHash::getBitX(unsigned pos) const {
        verify(pos < 32);
        return getBit(pos * 2);
    }

    bool GeoHash::getBitY(unsigned pos) const {
        verify(pos < 32);
        return getBit((pos * 2) + 1);
    }

    // TODO(hk): Comment this.
    BSONObj GeoHash::wrap(const char* name) const {
        BSONObjBuilder b(20);
        appendToBuilder(&b, name);
        BSONObj o = b.obj();
        if ('\0' == name[0]) verify(o.objsize() == 20);
        return o;
    }

    // Do we have a non-trivial GeoHash?
    bool GeoHash::constrains() const {
        return _bits > 0;
    }

    // Could our GeoHash have higher precision?
    bool GeoHash::canRefine() const {
       return _bits < 32;
    }

    /**
     * Hashing works like this:
     * Divide the world into 4 buckets.  Label each one as such:
     *  -----------------
     *  |       |       |
     *  |       |       |
     *  | 0,1   | 1,1   |
     *  -----------------
     *  |       |       |
     *  |       |       |
     *  | 0,0   | 1,0   |
     *  -----------------
     * We recursively divide each cell, furthermore.
     * The functions below tell us what quadrant we're in *at the finest level
     * of the subdivision.*
     */
    bool GeoHash::atMinX() const {
        return (_hash & geoBitSets.allX[_bits]) == 0;
    }
    bool GeoHash::atMinY() const {
        return (_hash & geoBitSets.allY[_bits]) == 0;
    }
    bool GeoHash::atMaxX() const {
        return (_hash & geoBitSets.allX[_bits]) == geoBitSets.allX[_bits];
    }
    bool GeoHash::atMaxY() const {
        return (_hash & geoBitSets.allY[_bits]) == geoBitSets.allY[_bits];
    }

    // TODO(hk): comment better
    void GeoHash::move(int x, int y) {
        verify(_bits);
        _move(0, x);
        _move(1, y);
    }

    // TODO(hk): comment much better
    void GeoHash::_move(unsigned offset, int d) {
        if (d == 0)
            return;
        verify(d <= 1 && d>= -1); // TEMP

        bool from, to;
        if (d > 0) {
            from = 0;
            to = 1;
        }
        else {
            from = 1;
            to = 0;
        }

        unsigned pos = (_bits * 2) - 1;
        if (offset == 0)
            pos--;
        while (true) {
            if (getBit(pos) == from) {
                setBit(pos , to);
                return;
            }

            if (pos < 2) {
                // overflow
                for (; pos < (_bits * 2) ; pos += 2) {
                    setBit(pos , from);
                }
                return;
            }

            setBit(pos , from);
            pos -= 2;
        }

        verify(0);
    }

    GeoHash& GeoHash::operator=(const GeoHash& h) {
        _hash = h._hash;
        _bits = h._bits;
        return *this;
    }

    bool GeoHash::operator==(const GeoHash& h) const {
        return _hash == h._hash && _bits == h._bits;
    }

    bool GeoHash::operator!=(const GeoHash& h) const {
        return !(*this == h);
    }

    bool GeoHash::operator<(const GeoHash& h) const {
        if (_hash != h._hash) return _hash < h._hash;
        return _bits < h._bits;
    }

    // Append the hash in s to our current hash.  We expect s to be '0' or '1' or '\0',
    // though we also treat non-'1' values as '0'.
    GeoHash& GeoHash::operator+=(const char* s) {
        unsigned pos = _bits * 2;
        _bits += strlen(s) / 2;
        verify(_bits <= 32);
        while ('\0' != s[0]) {
            if (s[0] == '1')
                setBit(pos , 1);
            pos++;
            s++;
        }
        return *this;
    }

    GeoHash GeoHash::operator+(const char *s) const {
        GeoHash n = *this;
        n += s;
        return n;
    }

    GeoHash GeoHash::operator+(const std::string& s) const {
       return operator+(s.c_str());
    }

    /* 
     * Keep the upper _bits*2 bits of _hash, clear the lower bits.
     * Maybe there's junk in there?  Not sure why this is done.
     */
    void GeoHash::clearUnusedBits() {
        static long long FULL = 0xFFFFFFFFFFFFFFFFLL;
        long long mask = FULL << (64 - (_bits * 2));
        _hash &= mask;
    }

    // Append the hash to the builder provided.
    void GeoHash::appendToBuilder(BSONObjBuilder* b, const char * name) const {
        char buf[8];
        _copyAndReverse(buf, (char*)&_hash);
        b->appendBinData(name, 8, bdtCustom, buf);
    }

    long long GeoHash::getHash() const {
        return _hash;
    }

    unsigned GeoHash::getBits() const {
        return _bits;
    }

    GeoHash GeoHash::commonPrefix(const GeoHash& other) const {
        unsigned i = 0;
        for (; i < _bits && i < other._bits; i++) {
            if (getBitX(i) == other.getBitX(i) && getBitY(i) == other.getBitY(i))
                continue;
            break;
        }
        // i is how many bits match between this and other.
        return GeoHash(_hash, i);
    }

    // Binary data is stored in some particular byte ordering that requires this.
    void GeoHash::_copyAndReverse(char *dst, const char *src) {
        for (unsigned a = 0; a < 8; a++) {
            dst[a] = src[7 - a];
        }
    }

    GeoHashConverter::GeoHashConverter(const Parameters &params) : _params(params) {
        // TODO(hk): What do we require of the values in params?

        // Compute how much error there is so it can be used as a fudge factor.
        GeoHash a(0, 0, _params.bits);
        GeoHash b = a;
        b.move(1, 1);

        // Epsilon is 1/100th of a bucket size
        // TODO:  Can we actually find error bounds for the sqrt function?
        double epsilon = 0.001 / _params.scaling;
        _error = distanceBetweenHashes(a, b) + epsilon;

        // Error in radians
        _errorSphere = deg2rad(_error);
    }

    double GeoHashConverter::distanceBetweenHashes(const GeoHash& a, const GeoHash& b) const {
        double ax, ay, bx, by;
        unhash(a, &ax, &ay);
        unhash(b, &bx, &by);

        double dx = bx - ax;
        double dy = by - ay;

        return sqrt((dx * dx) + (dy * dy));
    }

    /**
     * Hashing functions.  Convert the following types (which have a double precision point)
     * to a GeoHash:
     * BSONElement
     * BSONObj
     * Point
     * double, double
     */
     
    GeoHash GeoHashConverter::hash(const Point &p) const {
        return hash(p.x, p.y);
    }

    GeoHash GeoHashConverter::hash(const BSONElement& e) const {
        if (e.isABSONObj())
            return hash(e.embeddedObject());
        return GeoHash(e, _params.bits);
    }

    GeoHash GeoHashConverter::hash(const BSONObj& o) const {
        return hash(o, NULL);
    }

    // src is printed out as debugging information.  Maybe it is actually somehow the 'source' of o?
    GeoHash GeoHashConverter::hash(const BSONObj& o, const BSONObj* src) const {
        BSONObjIterator i(o);
        uassert(13067, 
                str::stream() << "geo field is empty"
                              << (src ? causedBy((*src).toString()) : ""),
                i.more());

        BSONElement x = i.next();
        uassert(13068, 
                str::stream() << "geo field only has 1 element"
                              << causedBy(src ? (*src).toString() : x.toString()),
                i.more());

        BSONElement y = i.next();
        uassert(13026, 
                str::stream() << "geo values must be 'legacy coordinate pairs' for 2d indexes"
                              << causedBy(src ? (*src).toString() :
                                           BSON_ARRAY(x << y).toString()),
                x.isNumber() && y.isNumber());

        uassert(13027, 
                str::stream() << "point not in interval of [ " << _params.min << ", " 
                              << _params.max << " ]"
                              << causedBy(src ? (*src).toString() :
                                          BSON_ARRAY(x.number() << y.number()).toString()),
                x.number() <= _params.max && x.number() >= _params.min &&
                y.number() <= _params.max && y.number() >= _params.min);

        return GeoHash(convertToHashScale(x.number()), convertToHashScale(y.number()),
                       _params.bits);
    }

    GeoHash GeoHashConverter::hash(double x, double y) const {
        uassert(16433,
                str::stream() << "point not in interval of [ " << _params.min << ", " 
                              << _params.max << " ]"
                              << causedBy(BSON_ARRAY(x << y).toString()),
                x <= _params.max && x >= _params.min &&
                y <= _params.max && y >= _params.min);

        return GeoHash(convertToHashScale(x), convertToHashScale(y) , _params.bits);
    }

    /**
     * Unhashing functions.  These convert from a "discretized" GeoHash to the "continuous"
     * doubles according to our scaling parameters.
     * 
     * Possible outputs:
     * double, double
     * Point
     * BSONObj
     */
    // TODO(hk): these should have consistent naming
    Point GeoHashConverter::unhashToPoint(const GeoHash &h) const {
        Point point;
        unhash(h, &point.x, &point.y);
        return point;
    }

    Point GeoHashConverter::unhashToPoint(const BSONElement &e) const {
        return unhashToPoint(hash(e));
    }

    BSONObj GeoHashConverter::unhashToBSONObj(const GeoHash& h) const {
        unsigned x, y;
        h.unhash(&x, &y);
        BSONObjBuilder b;
        b.append("x", convertFromHashScale(x));
        b.append("y", convertFromHashScale(y));
        return b.obj();
    }

    void GeoHashConverter::unhash(const GeoHash &h, double *x, double *y) const {
        unsigned a, b;
        h.unhash(&a, &b);
        *x = convertFromHashScale(a);
        *y = convertFromHashScale(b);
    }

    double GeoHashConverter::sizeOfDiag(const GeoHash& a) const {
        GeoHash b = a;
        b.move(1, 1);
        return distanceBetweenHashes(a, b);
    }

    double GeoHashConverter::sizeEdge(const GeoHash& a) const {
        if (!a.constrains())
            return _params.max - _params.min;

        double ax, ay, bx, by;
        GeoHash b = a;
        b.move(1, 1);
        unhash(a, &ax, &ay);
        unhash(b, &bx, &by);

        // _min and _max are a singularity
        if (bx == _params.min)
            bx = _params.max;

        return fabs(ax - bx);
    }

    // Convert from an unsigned in [0, (max-min)*scaling] to [min, max]
    double GeoHashConverter::convertFromHashScale(unsigned in) const {
        double x = in;
        x /= _params.scaling;
        x += _params.min;
        return x;
    }

    // Convert from a double that is [min, max] to an unsigned in [0, (max-min)*scaling]
    unsigned GeoHashConverter::convertToHashScale(double in) const {
        verify(in <= _params.max && in >= _params.min);

        if (in == _params.max) {
            // prevent aliasing with _min by moving inside the "box"
            // makes 180 == 179.999 (roughly)
            in -= _error / 2;
        }

        in -= _params.min;
        verify(in >= 0);
        return static_cast<unsigned>(in * _params.scaling);
    }
}  // namespace mongo
