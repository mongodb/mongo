// © 2017 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

// norms.cpp
// created: 2017jun04 Markus W. Scherer
// (pulled out of n2builder.cpp)

#include "unicode/utypes.h"

#if !UCONFIG_NO_NORMALIZATION

#include <stdio.h>
#include <stdlib.h>
#include "unicode/errorcode.h"
#include "unicode/umutablecptrie.h"
#include "unicode/unistr.h"
#include "unicode/utf16.h"
#include "normalizer2impl.h"
#include "norms.h"
#include "toolutil.h"
#include "uvectr32.h"

U_NAMESPACE_BEGIN

void BuilderReorderingBuffer::append(UChar32 c, uint8_t cc) {
    if(cc==0 || fLength==0 || ccAt(fLength-1)<=cc) {
        if(cc==0) {
            fLastStarterIndex=fLength;
        }
        fArray[fLength++]=(c<<8)|cc;
        return;
    }
    // Let this character bubble back to its canonical order.
    int32_t i=fLength-1;
    while(i>fLastStarterIndex && ccAt(i)>cc) {
        --i;
    }
    ++i;  // after the last starter or prevCC<=cc
    // Move this and the following characters forward one to make space.
    for(int32_t j=fLength; i<j; --j) {
        fArray[j]=fArray[j-1];
    }
    fArray[i]=(c<<8)|cc;
    ++fLength;
    fDidReorder=TRUE;
}

void BuilderReorderingBuffer::toString(UnicodeString &dest) const {
    dest.remove();
    for(int32_t i=0; i<fLength; ++i) {
        dest.append(charAt(i));
    }
}

UChar32 Norm::combine(UChar32 trail) const {
    int32_t length;
    const CompositionPair *pairs=getCompositionPairs(length);
    for(int32_t i=0; i<length; ++i) {
        if(trail==pairs[i].trail) {
            return pairs[i].composite;
        }
        if(trail<pairs[i].trail) {
            break;
        }
    }
    return U_SENTINEL;
}

Norms::Norms(UErrorCode &errorCode) {
    normTrie = umutablecptrie_open(0, 0, &errorCode);
    normMem=utm_open("gennorm2 normalization structs", 10000, 0x110100, sizeof(Norm));
    // Default "inert" Norm struct at index 0. Practically immutable.
    norms=allocNorm();
    norms->type=Norm::INERT;
}

Norms::~Norms() {
    umutablecptrie_close(normTrie);
    int32_t normsLength=utm_countItems(normMem);
    for(int32_t i=1; i<normsLength; ++i) {
        delete norms[i].mapping;
        delete norms[i].rawMapping;
        delete norms[i].compositions;
    }
    utm_close(normMem);
}

Norm *Norms::allocNorm() {
    Norm *p=(Norm *)utm_alloc(normMem);
    norms=(Norm *)utm_getStart(normMem);  // in case it got reallocated
    return p;
}

Norm *Norms::getNorm(UChar32 c) {
    uint32_t i = umutablecptrie_get(normTrie, c);
    if(i==0) {
        return nullptr;
    }
    return norms+i;
}

const Norm *Norms::getNorm(UChar32 c) const {
    uint32_t i = umutablecptrie_get(normTrie, c);
    if(i==0) {
        return nullptr;
    }
    return norms+i;
}

const Norm &Norms::getNormRef(UChar32 c) const {
    return norms[umutablecptrie_get(normTrie, c)];
}

Norm *Norms::createNorm(UChar32 c) {
    uint32_t i=umutablecptrie_get(normTrie, c);
    if(i!=0) {
        return norms+i;
    } else {
        /* allocate Norm */
        Norm *p=allocNorm();
        IcuToolErrorCode errorCode("gennorm2/createNorm()");
        umutablecptrie_set(normTrie, c, (uint32_t)(p - norms), errorCode);
        return p;
    }
}

void Norms::reorder(UnicodeString &mapping, BuilderReorderingBuffer &buffer) const {
    int32_t length=mapping.length();
    U_ASSERT(length<=Normalizer2Impl::MAPPING_LENGTH_MASK);
    const char16_t *s=mapping.getBuffer();
    int32_t i=0;
    UChar32 c;
    while(i<length) {
        U16_NEXT(s, i, length, c);
        buffer.append(c, getCC(c));
    }
    if(buffer.didReorder()) {
        buffer.toString(mapping);
    }
}

UBool Norms::combinesWithCCBetween(const Norm &norm, uint8_t lowCC, int32_t highCC) const {
    if((highCC-lowCC)>=2) {
        int32_t length;
        const CompositionPair *pairs=norm.getCompositionPairs(length);
        for(int32_t i=0; i<length; ++i) {
            uint8_t trailCC=getCC(pairs[i].trail);
            if(lowCC<trailCC && trailCC<highCC) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

void Norms::enumRanges(Enumerator &e) {
    UChar32 start = 0, end;
    uint32_t i;
    while ((end = umutablecptrie_getRange(normTrie, start, UCPMAP_RANGE_NORMAL, 0,
                                          nullptr, nullptr, &i)) >= 0) {
        if (i > 0) {
            e.rangeHandler(start, end, norms[i]);
        }
        start = end + 1;
    }
}

Norms::Enumerator::~Enumerator() {}

void CompositionBuilder::rangeHandler(UChar32 start, UChar32 end, Norm &norm) {
    if(norm.mappingType!=Norm::ROUND_TRIP) { return; }
    if(start!=end) {
        fprintf(stderr,
                "gennorm2 error: same round-trip mapping for "
                "more than 1 code point U+%04lX..U+%04lX\n",
                (long)start, (long)end);
        exit(U_INVALID_FORMAT_ERROR);
    }
    if(norm.cc!=0) {
        fprintf(stderr,
                "gennorm2 error: "
                "U+%04lX has a round-trip mapping and ccc!=0, "
                "not possible in Unicode normalization\n",
                (long)start);
        exit(U_INVALID_FORMAT_ERROR);
    }
    // setRoundTripMapping() ensured that there are exactly two code points.
    const UnicodeString &m=*norm.mapping;
    UChar32 lead=m.char32At(0);
    UChar32 trail=m.char32At(m.length()-1);
    if(norms.getCC(lead)!=0) {
        fprintf(stderr,
                "gennorm2 error: "
                "U+%04lX's round-trip mapping's starter U+%04lX has ccc!=0, "
                "not possible in Unicode normalization\n",
                (long)start, (long)lead);
        exit(U_INVALID_FORMAT_ERROR);
    }
    // Flag for trailing character.
    norms.createNorm(trail)->combinesBack=TRUE;
    // Insert (trail, composite) pair into compositions list for the lead character.
    IcuToolErrorCode errorCode("gennorm2/addComposition()");
    Norm *leadNorm=norms.createNorm(lead);
    UVector32 *compositions=leadNorm->compositions;
    int32_t i;
    if(compositions==nullptr) {
        compositions=leadNorm->compositions=new UVector32(errorCode);
        i=0;  // "insert" the first pair at index 0
    } else {
        // Insertion sort, and check for duplicate trail characters.
        int32_t length;
        const CompositionPair *pairs=leadNorm->getCompositionPairs(length);
        for(i=0; i<length; ++i) {
            if(trail==pairs[i].trail) {
                fprintf(stderr,
                        "gennorm2 error: same round-trip mapping for "
                        "more than 1 code point (e.g., U+%04lX) to U+%04lX + U+%04lX\n",
                        (long)start, (long)lead, (long)trail);
                exit(U_INVALID_FORMAT_ERROR);
            }
            if(trail<pairs[i].trail) {
                break;
            }
        }
    }
    compositions->insertElementAt(trail, 2*i, errorCode);
    compositions->insertElementAt(start, 2*i+1, errorCode);
}

void Decomposer::rangeHandler(UChar32 start, UChar32 end, Norm &norm) {
    if(!norm.hasMapping()) { return; }
    const UnicodeString &m=*norm.mapping;
    UnicodeString *decomposed=nullptr;
    const UChar *s=toUCharPtr(m.getBuffer());
    int32_t length=m.length();
    int32_t prev, i=0;
    UChar32 c;
    while(i<length) {
        prev=i;
        U16_NEXT(s, i, length, c);
        if(start<=c && c<=end) {
            fprintf(stderr,
                    "gennorm2 error: U+%04lX maps to itself directly or indirectly\n",
                    (long)c);
            exit(U_INVALID_FORMAT_ERROR);
        }
        const Norm &cNorm=norms.getNormRef(c);
        if(cNorm.hasMapping()) {
            if(norm.mappingType==Norm::ROUND_TRIP) {
                if(prev==0) {
                    if(cNorm.mappingType!=Norm::ROUND_TRIP) {
                        fprintf(stderr,
                                "gennorm2 error: "
                                "U+%04lX's round-trip mapping's starter "
                                "U+%04lX one-way-decomposes, "
                                "not possible in Unicode normalization\n",
                                (long)start, (long)c);
                        exit(U_INVALID_FORMAT_ERROR);
                    }
                    uint8_t myTrailCC=norms.getCC(m.char32At(i));
                    UChar32 cTrailChar=cNorm.mapping->char32At(cNorm.mapping->length()-1);
                    uint8_t cTrailCC=norms.getCC(cTrailChar);
                    if(cTrailCC>myTrailCC) {
                        fprintf(stderr,
                                "gennorm2 error: "
                                "U+%04lX's round-trip mapping's starter "
                                "U+%04lX decomposes and the "
                                "inner/earlier tccc=%hu > outer/following tccc=%hu, "
                                "not possible in Unicode normalization\n",
                                (long)start, (long)c,
                                (short)cTrailCC, (short)myTrailCC);
                        exit(U_INVALID_FORMAT_ERROR);
                    }
                } else {
                    fprintf(stderr,
                            "gennorm2 error: "
                            "U+%04lX's round-trip mapping's non-starter "
                            "U+%04lX decomposes, "
                            "not possible in Unicode normalization\n",
                            (long)start, (long)c);
                    exit(U_INVALID_FORMAT_ERROR);
                }
            }
            if(decomposed==nullptr) {
                decomposed=new UnicodeString(m, 0, prev);
            }
            decomposed->append(*cNorm.mapping);
        } else if(Hangul::isHangul(c)) {
            UChar buffer[3];
            int32_t hangulLength=Hangul::decompose(c, buffer);
            if(norm.mappingType==Norm::ROUND_TRIP && prev!=0) {
                fprintf(stderr,
                        "gennorm2 error: "
                        "U+%04lX's round-trip mapping's non-starter "
                        "U+%04lX decomposes, "
                        "not possible in Unicode normalization\n",
                        (long)start, (long)c);
                exit(U_INVALID_FORMAT_ERROR);
            }
            if(decomposed==nullptr) {
                decomposed=new UnicodeString(m, 0, prev);
            }
            decomposed->append(buffer, hangulLength);
        } else if(decomposed!=nullptr) {
            decomposed->append(m, prev, i-prev);
        }
    }
    if(decomposed!=nullptr) {
        if(norm.rawMapping==nullptr) {
            // Remember the original mapping when decomposing recursively.
            norm.rawMapping=norm.mapping;
        } else {
            delete norm.mapping;
        }
        norm.mapping=decomposed;
        // Not  norm.setMappingCP();  because the original mapping
        // is most likely to be encodable as a delta.
        didDecompose|=TRUE;
    }
}

U_NAMESPACE_END

#endif // #if !UCONFIG_NO_NORMALIZATION
