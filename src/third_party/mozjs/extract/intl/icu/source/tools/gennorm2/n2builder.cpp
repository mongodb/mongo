// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2009-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  n2builder.cpp
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2009nov25
*   created by: Markus W. Scherer
*
* Builds Normalizer2 data and writes a binary .nrm file.
* For the file format see source/common/normalizer2impl.h.
*/

#include "unicode/utypes.h"
#include "n2builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "unicode/errorcode.h"
#include "unicode/localpointer.h"
#include "unicode/putil.h"
#include "unicode/ucptrie.h"
#include "unicode/udata.h"
#include "unicode/umutablecptrie.h"
#include "unicode/uniset.h"
#include "unicode/unistr.h"
#include "unicode/usetiter.h"
#include "unicode/ustring.h"
#include "charstr.h"
#include "extradata.h"
#include "hash.h"
#include "normalizer2impl.h"
#include "norms.h"
#include "toolutil.h"
#include "unewdata.h"
#include "uvectr32.h"
#include "writesrc.h"

#if !UCONFIG_NO_NORMALIZATION

/* UDataInfo cf. udata.h */
static UDataInfo dataInfo={
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    U_SIZEOF_UCHAR,
    0,

    { 0x4e, 0x72, 0x6d, 0x32 }, /* dataFormat="Nrm2" */
    { 4, 0, 0, 0 },             /* formatVersion */
    { 11, 0, 0, 0 }             /* dataVersion (Unicode version) */
};

U_NAMESPACE_BEGIN

class HangulIterator {
public:
    struct Range {
        UChar32 start, end;
    };

    HangulIterator() : rangeIndex(0) {}
    const Range *nextRange() {
        if(rangeIndex<UPRV_LENGTHOF(ranges)) {
            return ranges+rangeIndex++;
        } else {
            return NULL;
        }
    }
private:
    static const Range ranges[4];
    int32_t rangeIndex;
};

const HangulIterator::Range HangulIterator::ranges[4]={
    { Hangul::JAMO_L_BASE, Hangul::JAMO_L_END },
    { Hangul::JAMO_V_BASE, Hangul::JAMO_V_END },
    // JAMO_T_BASE+1: not U+11A7
    { Hangul::JAMO_T_BASE+1, Hangul::JAMO_T_END },
    { Hangul::HANGUL_BASE, Hangul::HANGUL_END },
};

Normalizer2DataBuilder::Normalizer2DataBuilder(UErrorCode &errorCode) :
        norms(errorCode),
        phase(0), overrideHandling(OVERRIDE_PREVIOUS), optimization(OPTIMIZE_NORMAL),
        norm16TrieBytes(nullptr), norm16TrieLength(0) {
    memset(unicodeVersion, 0, sizeof(unicodeVersion));
    memset(indexes, 0, sizeof(indexes));
    memset(smallFCD, 0, sizeof(smallFCD));
}

Normalizer2DataBuilder::~Normalizer2DataBuilder() {
    delete[] norm16TrieBytes;
}

void
Normalizer2DataBuilder::setUnicodeVersion(const char *v) {
    UVersionInfo nullVersion={ 0, 0, 0, 0 };
    UVersionInfo version;
    u_versionFromString(version, v);
    if( 0!=memcmp(version, unicodeVersion, U_MAX_VERSION_LENGTH) &&
        0!=memcmp(nullVersion, unicodeVersion, U_MAX_VERSION_LENGTH)
    ) {
        char buffer[U_MAX_VERSION_STRING_LENGTH];
        u_versionToString(unicodeVersion, buffer);
        fprintf(stderr, "gennorm2 error: multiple inconsistent Unicode version numbers %s vs. %s\n",
                buffer, v);
        exit(U_ILLEGAL_ARGUMENT_ERROR);
    }
    memcpy(unicodeVersion, version, U_MAX_VERSION_LENGTH);
}

Norm *Normalizer2DataBuilder::checkNormForMapping(Norm *p, UChar32 c) {
    if(p!=NULL) {
        if(p->mappingType!=Norm::NONE) {
            if( overrideHandling==OVERRIDE_NONE ||
                (overrideHandling==OVERRIDE_PREVIOUS && p->mappingPhase==phase)
            ) {
                fprintf(stderr,
                        "error in gennorm2 phase %d: "
                        "not permitted to override mapping for U+%04lX from phase %d\n",
                        (int)phase, (long)c, (int)p->mappingPhase);
                exit(U_INVALID_FORMAT_ERROR);
            }
            delete p->mapping;
            p->mapping=NULL;
        }
        p->mappingPhase=phase;
    }
    return p;
}

void Normalizer2DataBuilder::setOverrideHandling(OverrideHandling oh) {
    overrideHandling=oh;
    ++phase;
}

void Normalizer2DataBuilder::setCC(UChar32 c, uint8_t cc) {
    norms.createNorm(c)->cc=cc;
    norms.ccSet.add(c);
}

static UBool isWellFormed(const UnicodeString &s) {
    UErrorCode errorCode=U_ZERO_ERROR;
    u_strToUTF8(NULL, 0, NULL, toUCharPtr(s.getBuffer()), s.length(), &errorCode);
    return U_SUCCESS(errorCode) || errorCode==U_BUFFER_OVERFLOW_ERROR;
}

void Normalizer2DataBuilder::setOneWayMapping(UChar32 c, const UnicodeString &m) {
    if(!isWellFormed(m)) {
        fprintf(stderr,
                "error in gennorm2 phase %d: "
                "illegal one-way mapping from U+%04lX to malformed string\n",
                (int)phase, (long)c);
        exit(U_INVALID_FORMAT_ERROR);
    }
    Norm *p=checkNormForMapping(norms.createNorm(c), c);
    p->mapping=new UnicodeString(m);
    p->mappingType=Norm::ONE_WAY;
    p->setMappingCP();
    norms.mappingSet.add(c);
}

void Normalizer2DataBuilder::setRoundTripMapping(UChar32 c, const UnicodeString &m) {
    if(U_IS_SURROGATE(c)) {
        fprintf(stderr,
                "error in gennorm2 phase %d: "
                "illegal round-trip mapping from surrogate code point U+%04lX\n",
                (int)phase, (long)c);
        exit(U_INVALID_FORMAT_ERROR);
    }
    if(!isWellFormed(m)) {
        fprintf(stderr,
                "error in gennorm2 phase %d: "
                "illegal round-trip mapping from U+%04lX to malformed string\n",
                (int)phase, (long)c);
        exit(U_INVALID_FORMAT_ERROR);
    }
    int32_t numCP=u_countChar32(toUCharPtr(m.getBuffer()), m.length());
    if(numCP!=2) {
        fprintf(stderr,
                "error in gennorm2 phase %d: "
                "illegal round-trip mapping from U+%04lX to %d!=2 code points\n",
                (int)phase, (long)c, (int)numCP);
        exit(U_INVALID_FORMAT_ERROR);
    }
    Norm *p=checkNormForMapping(norms.createNorm(c), c);
    p->mapping=new UnicodeString(m);
    p->mappingType=Norm::ROUND_TRIP;
    p->mappingCP=U_SENTINEL;
    norms.mappingSet.add(c);
}

void Normalizer2DataBuilder::removeMapping(UChar32 c) {
    // createNorm(c), not getNorm(c), to record a non-mapping and detect conflicting data.
    Norm *p=checkNormForMapping(norms.createNorm(c), c);
    p->mappingType=Norm::REMOVED;
    norms.mappingSet.add(c);
}

UBool Normalizer2DataBuilder::mappingHasCompBoundaryAfter(const BuilderReorderingBuffer &buffer,
                                                          Norm::MappingType mappingType) const {
    if(buffer.isEmpty()) {
        return FALSE;  // Maps-to-empty-string is no boundary of any kind.
    }
    int32_t lastStarterIndex=buffer.lastStarterIndex();
    if(lastStarterIndex<0) {
        return FALSE;  // no starter
    }
    const int32_t lastIndex=buffer.length()-1;
    if(mappingType==Norm::ONE_WAY && lastStarterIndex<lastIndex && buffer.ccAt(lastIndex)>1) {
        // One-way mapping where after the last starter is at least one combining mark
        // with a combining class greater than 1,
        // which means that another combining mark can reorder before it.
        // By contrast, in a round-trip mapping this does not prevent a boundary as long as
        // the starter or composite does not combine-forward with a following combining mark.
        return FALSE;
    }
    UChar32 starter=buffer.charAt(lastStarterIndex);
    if(lastStarterIndex==0 && norms.combinesBack(starter)) {
        // The last starter is at the beginning of the mapping and combines backward.
        return FALSE;
    }
    if(Hangul::isJamoL(starter) ||
            (Hangul::isJamoV(starter) &&
            0<lastStarterIndex && Hangul::isJamoL(buffer.charAt(lastStarterIndex-1)))) {
        // A Jamo leading consonant or an LV pair combines-forward if it is at the end,
        // otherwise it is blocked.
        return lastStarterIndex!=lastIndex;
    }
    // Note: There can be no Hangul syllable in the fully decomposed mapping.

    // Multiple starters can combine into one.
    // Look for the first of the last sequence of starters, excluding Jamos.
    int32_t i=lastStarterIndex;
    UChar32 c;
    while(0<i && buffer.ccAt(i-1)==0 && !Hangul::isJamo(c=buffer.charAt(i-1))) {
        starter=c;
        --i;
    }
    // Compose as far as possible, and see if further compositions with
    // characters following this mapping are possible.
    const Norm *starterNorm=norms.getNorm(starter);
    if(i==lastStarterIndex &&
            (starterNorm==nullptr || starterNorm->compositions==nullptr)) {
        return TRUE;  // The last starter does not combine forward.
    }
    uint8_t prevCC=0;
    while(++i<buffer.length()) {
        uint8_t cc=buffer.ccAt(i);  // !=0 if after last starter
        if(i>lastStarterIndex && norms.combinesWithCCBetween(*starterNorm, prevCC, cc)) {
            // The starter combines with a mark that reorders before the current one.
            return FALSE;
        }
        UChar32 c=buffer.charAt(i);
        if(starterNorm!=nullptr && (prevCC<cc || prevCC==0) &&
                norms.getNormRef(c).combinesBack && (starter=starterNorm->combine(c))>=0) {
            // The starter combines with c into a composite replacement starter.
            starterNorm=norms.getNorm(starter);
            if(i>=lastStarterIndex &&
                    (starterNorm==nullptr || starterNorm->compositions==nullptr)) {
                return TRUE;  // The composite does not combine further.
            }
            // Keep prevCC because we "removed" the combining mark.
        } else if(cc==0) {
            starterNorm=norms.getNorm(c);
            if(i==lastStarterIndex &&
                    (starterNorm==nullptr || starterNorm->compositions==nullptr)) {
                return TRUE;  // The new starter does not combine forward.
            }
            prevCC=0;
        } else {
            prevCC=cc;
        }
    }
    if(prevCC==0) {
        return FALSE;  // forward-combining starter at the very end
    }
    if(norms.combinesWithCCBetween(*starterNorm, prevCC, 256)) {
        // The starter combines with another mark.
        return FALSE;
    }
    return TRUE;
}

UBool Normalizer2DataBuilder::mappingRecomposes(const BuilderReorderingBuffer &buffer) const {
    if(buffer.lastStarterIndex()<0) {
        return FALSE;  // no starter
    }
    const Norm *starterNorm=nullptr;
    uint8_t prevCC=0;
    for(int32_t i=0; i<buffer.length(); ++i) {
        UChar32 c=buffer.charAt(i);
        uint8_t cc=buffer.ccAt(i);
        if(starterNorm!=nullptr && (prevCC<cc || prevCC==0) &&
                norms.getNormRef(c).combinesBack && starterNorm->combine(c)>=0) {
            return TRUE;  // normal composite
        } else if(cc==0) {
            if(Hangul::isJamoL(c)) {
                if((i+1)<buffer.length() && Hangul::isJamoV(buffer.charAt(i+1))) {
                    return TRUE;  // Hangul syllable
                }
                starterNorm=nullptr;
            } else {
                starterNorm=norms.getNorm(c);
            }
        }
        prevCC=cc;
    }
    return FALSE;
}

void Normalizer2DataBuilder::postProcess(Norm &norm) {
    // Prerequisites: Compositions are built, mappings are recursively decomposed.
    // Mappings are not yet in canonical order.
    //
    // This function works on a Norm struct. We do not know which code point(s) map(s) to it.
    // Therefore, we cannot compute algorithmic mapping deltas here.
    // Error conditions are checked, but printed later when we do know the offending code point.
    if(norm.hasMapping()) {
        if(norm.mapping->length()>Normalizer2Impl::MAPPING_LENGTH_MASK) {
            norm.error="mapping longer than maximum of 31";
            return;
        }
        // Ensure canonical order.
        BuilderReorderingBuffer buffer;
        if(norm.rawMapping!=nullptr) {
            norms.reorder(*norm.rawMapping, buffer);
            buffer.reset();
        }
        norms.reorder(*norm.mapping, buffer);
        if(buffer.isEmpty()) {
            // A character that is deleted (maps to an empty string) must
            // get the worst-case lccc and tccc values because arbitrary
            // characters on both sides will become adjacent.
            norm.leadCC=1;
            norm.trailCC=0xff;
        } else {
            norm.leadCC=buffer.ccAt(0);
            norm.trailCC=buffer.ccAt(buffer.length()-1);
        }

        norm.hasCompBoundaryBefore=
            !buffer.isEmpty() && norm.leadCC==0 && !norms.combinesBack(buffer.charAt(0));
        norm.hasCompBoundaryAfter=
            norm.compositions==nullptr && mappingHasCompBoundaryAfter(buffer, norm.mappingType);

        if(norm.combinesBack) {
            norm.error="combines-back and decomposes, not possible in Unicode normalization";
        } else if(norm.mappingType==Norm::ROUND_TRIP) {
            if(norm.compositions!=NULL) {
                norm.type=Norm::YES_NO_COMBINES_FWD;
            } else {
                norm.type=Norm::YES_NO_MAPPING_ONLY;
            }
        } else {  // one-way mapping
            if(norm.compositions!=NULL) {
                norm.error="combines-forward and has a one-way mapping, "
                           "not possible in Unicode normalization";
            } else if(buffer.isEmpty()) {
                norm.type=Norm::NO_NO_EMPTY;
            } else if(!norm.hasCompBoundaryBefore) {
                norm.type=Norm::NO_NO_COMP_NO_MAYBE_CC;
            } else if(mappingRecomposes(buffer)) {
                norm.type=Norm::NO_NO_COMP_BOUNDARY_BEFORE;
            } else {
                // The mapping is comp-normalized.
                norm.type=Norm::NO_NO_COMP_YES;
            }
        }
    } else {  // no mapping
        norm.leadCC=norm.trailCC=norm.cc;

        norm.hasCompBoundaryBefore=
            norm.cc==0 && !norm.combinesBack;
        norm.hasCompBoundaryAfter=
            norm.cc==0 && !norm.combinesBack && norm.compositions==nullptr;

        if(norm.combinesBack) {
            if(norm.compositions!=nullptr) {
                // Earlier code checked ccc=0.
                norm.type=Norm::MAYBE_YES_COMBINES_FWD;
            } else {
                norm.type=Norm::MAYBE_YES_SIMPLE;  // any ccc
            }
        } else if(norm.compositions!=nullptr) {
            // Earlier code checked ccc=0.
            norm.type=Norm::YES_YES_COMBINES_FWD;
        } else if(norm.cc!=0) {
            norm.type=Norm::YES_YES_WITH_CC;
        } else {
            norm.type=Norm::INERT;
        }
    }
}

class Norm16Writer : public Norms::Enumerator {
public:
    Norm16Writer(UMutableCPTrie *trie, Norms &n, Normalizer2DataBuilder &b) :
            Norms::Enumerator(n), builder(b), norm16Trie(trie) {}
    void rangeHandler(UChar32 start, UChar32 end, Norm &norm) U_OVERRIDE {
        builder.writeNorm16(norm16Trie, start, end, norm);
    }
    Normalizer2DataBuilder &builder;
    UMutableCPTrie *norm16Trie;
};

void Normalizer2DataBuilder::setSmallFCD(UChar32 c) {
    UChar32 lead= c<=0xffff ? c : U16_LEAD(c);
    smallFCD[lead>>8]|=(uint8_t)1<<((lead>>5)&7);
}

void Normalizer2DataBuilder::writeNorm16(UMutableCPTrie *norm16Trie, UChar32 start, UChar32 end, Norm &norm) {
    if((norm.leadCC|norm.trailCC)!=0) {
        for(UChar32 c=start; c<=end; ++c) {
            setSmallFCD(c);
        }
    }

    int32_t norm16;
    switch(norm.type) {
    case Norm::INERT:
        norm16=Normalizer2Impl::INERT;
        break;
    case Norm::YES_YES_COMBINES_FWD:
        norm16=norm.offset*2;
        break;
    case Norm::YES_NO_COMBINES_FWD:
        norm16=indexes[Normalizer2Impl::IX_MIN_YES_NO]+norm.offset*2;
        break;
    case Norm::YES_NO_MAPPING_ONLY:
        norm16=indexes[Normalizer2Impl::IX_MIN_YES_NO_MAPPINGS_ONLY]+norm.offset*2;
        break;
    case Norm::NO_NO_COMP_YES:
        norm16=indexes[Normalizer2Impl::IX_MIN_NO_NO]+norm.offset*2;
        break;
    case Norm::NO_NO_COMP_BOUNDARY_BEFORE:
        norm16=indexes[Normalizer2Impl::IX_MIN_NO_NO_COMP_BOUNDARY_BEFORE]+norm.offset*2;
        break;
    case Norm::NO_NO_COMP_NO_MAYBE_CC:
        norm16=indexes[Normalizer2Impl::IX_MIN_NO_NO_COMP_NO_MAYBE_CC]+norm.offset*2;
        break;
    case Norm::NO_NO_EMPTY:
        norm16=indexes[Normalizer2Impl::IX_MIN_NO_NO_EMPTY]+norm.offset*2;
        break;
    case Norm::NO_NO_DELTA:
        {
            // Positive offset from minNoNoDelta, shifted left for additional bits.
            int32_t offset=(norm.offset+Normalizer2Impl::MAX_DELTA)<<Normalizer2Impl::DELTA_SHIFT;
            if(norm.trailCC==0) {
                // DELTA_TCCC_0==0
            } else if(norm.trailCC==1) {
                offset|=Normalizer2Impl::DELTA_TCCC_1;
            } else {
                offset|=Normalizer2Impl::DELTA_TCCC_GT_1;
            }
            norm16=getMinNoNoDelta()+offset;
            break;
        }
    case Norm::MAYBE_YES_COMBINES_FWD:
        norm16=indexes[Normalizer2Impl::IX_MIN_MAYBE_YES]+norm.offset*2;
        break;
    case Norm::MAYBE_YES_SIMPLE:
        norm16=Normalizer2Impl::MIN_NORMAL_MAYBE_YES+norm.cc*2;  // ccc=0..255
        break;
    case Norm::YES_YES_WITH_CC:
        U_ASSERT(norm.cc!=0);
        norm16=Normalizer2Impl::MIN_YES_YES_WITH_CC-2+norm.cc*2;  // ccc=1..255
        break;
    default:  // Should not occur.
        exit(U_INTERNAL_PROGRAM_ERROR);
    }
    U_ASSERT((norm16&1)==0);
    if(norm.hasCompBoundaryAfter) {
        norm16|=Normalizer2Impl::HAS_COMP_BOUNDARY_AFTER;
    }
    IcuToolErrorCode errorCode("gennorm2/writeNorm16()");
    umutablecptrie_setRange(norm16Trie, start, end, (uint32_t)norm16, errorCode);

    // Set the minimum code points for real data lookups in the quick check loops.
    UBool isDecompNo=
            (Norm::YES_NO_COMBINES_FWD<=norm.type && norm.type<=Norm::NO_NO_DELTA) ||
            norm.cc!=0;
    if(isDecompNo && start<indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]) {
        indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]=start;
    }
    UBool isCompNoMaybe= norm.type>=Norm::NO_NO_COMP_YES;
    if(isCompNoMaybe && start<indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]) {
        indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]=start;
    }
    if(norm.leadCC!=0 && start<indexes[Normalizer2Impl::IX_MIN_LCCC_CP]) {
        indexes[Normalizer2Impl::IX_MIN_LCCC_CP]=start;
    }
}

void Normalizer2DataBuilder::setHangulData(UMutableCPTrie *norm16Trie) {
    HangulIterator hi;
    const HangulIterator::Range *range;
    // Check that none of the Hangul/Jamo code points have data.
    while((range=hi.nextRange())!=NULL) {
        for(UChar32 c=range->start; c<=range->end; ++c) {
            if(umutablecptrie_get(norm16Trie, c)>Normalizer2Impl::INERT) {
                fprintf(stderr,
                        "gennorm2 error: "
                        "illegal mapping/composition/ccc data for Hangul or Jamo U+%04lX\n",
                        (long)c);
                exit(U_INVALID_FORMAT_ERROR);
            }
        }
    }
    // Set data for algorithmic runtime handling.
    IcuToolErrorCode errorCode("gennorm2/setHangulData()");

    // Jamo V/T are maybeYes
    if(Hangul::JAMO_V_BASE<indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]) {
        indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]=Hangul::JAMO_V_BASE;
    }
    umutablecptrie_setRange(norm16Trie, Hangul::JAMO_L_BASE, Hangul::JAMO_L_END,
                            Normalizer2Impl::JAMO_L, errorCode);
    umutablecptrie_setRange(norm16Trie, Hangul::JAMO_V_BASE, Hangul::JAMO_V_END,
                            Normalizer2Impl::JAMO_VT, errorCode);
    // JAMO_T_BASE+1: not U+11A7
    umutablecptrie_setRange(norm16Trie, Hangul::JAMO_T_BASE+1, Hangul::JAMO_T_END,
                            Normalizer2Impl::JAMO_VT, errorCode);

    // Hangul LV encoded as minYesNo
    uint32_t lv=indexes[Normalizer2Impl::IX_MIN_YES_NO];
    // Hangul LVT encoded as minYesNoMappingsOnly|HAS_COMP_BOUNDARY_AFTER
    uint32_t lvt=indexes[Normalizer2Impl::IX_MIN_YES_NO_MAPPINGS_ONLY]|
        Normalizer2Impl::HAS_COMP_BOUNDARY_AFTER;
    if(Hangul::HANGUL_BASE<indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]) {
        indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]=Hangul::HANGUL_BASE;
    }
    // Set the first LV, then write all other Hangul syllables as LVT,
    // then overwrite the remaining LV.
    umutablecptrie_set(norm16Trie, Hangul::HANGUL_BASE, lv, errorCode);
    umutablecptrie_setRange(norm16Trie, Hangul::HANGUL_BASE+1, Hangul::HANGUL_END, lvt, errorCode);
    UChar32 c=Hangul::HANGUL_BASE;
    while((c+=Hangul::JAMO_T_COUNT)<=Hangul::HANGUL_END) {
        umutablecptrie_set(norm16Trie, c, lv, errorCode);
    }
    errorCode.assertSuccess();
}

LocalUCPTriePointer Normalizer2DataBuilder::processData() {
    // Build composition lists before recursive decomposition,
    // so that we still have the raw, pair-wise mappings.
    CompositionBuilder compBuilder(norms);
    norms.enumRanges(compBuilder);

    // Recursively decompose all mappings.
    Decomposer decomposer(norms);
    do {
        decomposer.didDecompose=FALSE;
        norms.enumRanges(decomposer);
    } while(decomposer.didDecompose);

    // Set the Norm::Type and other properties.
    int32_t normsLength=norms.length();
    for(int32_t i=1; i<normsLength; ++i) {
        postProcess(norms.getNormRefByIndex(i));
    }

    // Write the properties, mappings and composition lists to
    // appropriate parts of the "extra data" array.
    ExtraData extra(norms, optimization==OPTIMIZE_FAST);
    norms.enumRanges(extra);

    extraData=extra.yesYesCompositions;
    indexes[Normalizer2Impl::IX_MIN_YES_NO]=extraData.length()*2;
    extraData.append(extra.yesNoMappingsAndCompositions);
    indexes[Normalizer2Impl::IX_MIN_YES_NO_MAPPINGS_ONLY]=extraData.length()*2;
    extraData.append(extra.yesNoMappingsOnly);
    indexes[Normalizer2Impl::IX_MIN_NO_NO]=extraData.length()*2;
    extraData.append(extra.noNoMappingsCompYes);
    indexes[Normalizer2Impl::IX_MIN_NO_NO_COMP_BOUNDARY_BEFORE]=extraData.length()*2;
    extraData.append(extra.noNoMappingsCompBoundaryBefore);
    indexes[Normalizer2Impl::IX_MIN_NO_NO_COMP_NO_MAYBE_CC]=extraData.length()*2;
    extraData.append(extra.noNoMappingsCompNoMaybeCC);
    indexes[Normalizer2Impl::IX_MIN_NO_NO_EMPTY]=extraData.length()*2;
    extraData.append(extra.noNoMappingsEmpty);
    indexes[Normalizer2Impl::IX_LIMIT_NO_NO]=extraData.length()*2;

    // Pad the maybeYesCompositions length to a multiple of 4,
    // so that NO_NO_DELTA bits 2..1 can be used without subtracting the center.
    while(extra.maybeYesCompositions.length()&3) {
        extra.maybeYesCompositions.append((UChar)0);
    }
    extraData.insert(0, extra.maybeYesCompositions);
    indexes[Normalizer2Impl::IX_MIN_MAYBE_YES]=
        Normalizer2Impl::MIN_NORMAL_MAYBE_YES-
        extra.maybeYesCompositions.length()*2;

    // Pad to even length for 4-byte alignment of following data.
    if(extraData.length()&1) {
        extraData.append((UChar)0);
    }

    int32_t minNoNoDelta=getMinNoNoDelta();
    U_ASSERT((minNoNoDelta&7)==0);
    if(indexes[Normalizer2Impl::IX_LIMIT_NO_NO]>minNoNoDelta) {
        fprintf(stderr,
                "gennorm2 error: "
                "data structure overflow, too much mapping composition data\n");
        exit(U_BUFFER_OVERFLOW_ERROR);
    }

    // writeNorm16() and setHangulData() reduce these as needed.
    indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]=0x110000;
    indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]=0x110000;
    indexes[Normalizer2Impl::IX_MIN_LCCC_CP]=0x110000;

    IcuToolErrorCode errorCode("gennorm2/processData()");
    UMutableCPTrie *norm16Trie = umutablecptrie_open(
        Normalizer2Impl::INERT, Normalizer2Impl::INERT, errorCode);
    errorCode.assertSuccess();

    // Map each code point to its norm16 value,
    // including the properties that fit directly,
    // and the offset to the "extra data" if necessary.
    Norm16Writer norm16Writer(norm16Trie, norms, *this);
    norms.enumRanges(norm16Writer);
    // TODO: iterate via getRange() instead of callback?

    setHangulData(norm16Trie);

    // Look for the "worst" norm16 value of any supplementary code point
    // corresponding to a lead surrogate, and set it as that surrogate's value.
    // Enables UTF-16 quick check inner loops to look at only code units.
    //
    // We could be more sophisticated:
    // We could collect a bit set for whether there are values in the different
    // norm16 ranges (yesNo, maybeYes, yesYesWithCC etc.)
    // and select the best value that only breaks the composition and/or decomposition
    // inner loops if necessary.
    // However, that seems like overkill for an optimization for supplementary characters.
    //
    // First check that surrogate code *points* are inert.
    // The parser should have rejected values/mappings for them.
    uint32_t value;
    UChar32 end = umutablecptrie_getRange(norm16Trie, 0xd800, UCPMAP_RANGE_NORMAL, 0,
                                          nullptr, nullptr, &value);
    if (value != Normalizer2Impl::INERT || end < 0xdfff) {
        fprintf(stderr,
                "gennorm2 error: not all surrogate code points are inert: U+d800..U+%04x=%lx\n",
                (int)end, (long)value);
        exit(U_INTERNAL_PROGRAM_ERROR);
    }
    uint32_t maxNorm16 = 0;
    // ANDing values yields 0 bits where any value has a 0.
    // Used for worst-case HAS_COMP_BOUNDARY_AFTER.
    uint32_t andedNorm16 = 0;
    end = 0;
    for (UChar32 start = 0x10000;;) {
        if (start > end) {
            end = umutablecptrie_getRange(norm16Trie, start, UCPMAP_RANGE_NORMAL, 0,
                                          nullptr, nullptr, &value);
            if (end < 0) { break; }
        }
        if ((start & 0x3ff) == 0) {
            // Data for a new lead surrogate.
            maxNorm16 = andedNorm16 = value;
        } else {
            if (value > maxNorm16) {
                maxNorm16 = value;
            }
            andedNorm16 &= value;
        }
        // Intersect each range with the code points for one lead surrogate.
        UChar32 leadEnd = start | 0x3ff;
        if (leadEnd <= end) {
            // End of the supplementary block for a lead surrogate.
            if (maxNorm16 >= (uint32_t)indexes[Normalizer2Impl::IX_LIMIT_NO_NO]) {
                // Set noNo ("worst" value) if it got into "less-bad" maybeYes or ccc!=0.
                // Otherwise it might end up at something like JAMO_VT which stays in
                // the inner decomposition quick check loop.
                maxNorm16 = (uint32_t)indexes[Normalizer2Impl::IX_LIMIT_NO_NO];
            }
            maxNorm16 =
                (maxNorm16 & ~Normalizer2Impl::HAS_COMP_BOUNDARY_AFTER)|
                (andedNorm16 & Normalizer2Impl::HAS_COMP_BOUNDARY_AFTER);
            if (maxNorm16 != Normalizer2Impl::INERT) {
                umutablecptrie_set(norm16Trie, U16_LEAD(start), maxNorm16, errorCode);
            }
            if (value == Normalizer2Impl::INERT) {
                // Potentially skip inert supplementary blocks for several lead surrogates.
                start = (end + 1) & ~0x3ff;
            } else {
                start = leadEnd + 1;
            }
        } else {
            start = end + 1;
        }
    }

    // Adjust supplementary minimum code points to break quick check loops at their lead surrogates.
    // For an empty data file, minCP=0x110000 turns into 0xdc00 (first trail surrogate)
    // which is harmless.
    // As a result, the minimum code points are always BMP code points.
    int32_t minCP=indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP];
    if(minCP>=0x10000) {
        indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]=U16_LEAD(minCP);
    }
    minCP=indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP];
    if(minCP>=0x10000) {
        indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]=U16_LEAD(minCP);
    }
    minCP=indexes[Normalizer2Impl::IX_MIN_LCCC_CP];
    if(minCP>=0x10000) {
        indexes[Normalizer2Impl::IX_MIN_LCCC_CP]=U16_LEAD(minCP);
    }

    LocalUCPTriePointer builtTrie(
        umutablecptrie_buildImmutable(norm16Trie, UCPTRIE_TYPE_FAST, UCPTRIE_VALUE_BITS_16, errorCode));
    norm16TrieLength=ucptrie_toBinary(builtTrie.getAlias(), nullptr, 0, errorCode);
    if(errorCode.get()!=U_BUFFER_OVERFLOW_ERROR) {
        fprintf(stderr, "gennorm2 error: unable to build/serialize the normalization trie - %s\n",
                errorCode.errorName());
        exit(errorCode.reset());
    }
    umutablecptrie_close(norm16Trie);
    errorCode.reset();
    norm16TrieBytes=new uint8_t[norm16TrieLength];
    ucptrie_toBinary(builtTrie.getAlias(), norm16TrieBytes, norm16TrieLength, errorCode);
    errorCode.assertSuccess();

    int32_t offset=(int32_t)sizeof(indexes);
    indexes[Normalizer2Impl::IX_NORM_TRIE_OFFSET]=offset;
    offset+=norm16TrieLength;
    indexes[Normalizer2Impl::IX_EXTRA_DATA_OFFSET]=offset;
    offset+=extraData.length()*2;
    indexes[Normalizer2Impl::IX_SMALL_FCD_OFFSET]=offset;
    offset+=sizeof(smallFCD);
    int32_t totalSize=offset;
    for(int32_t i=Normalizer2Impl::IX_RESERVED3_OFFSET; i<=Normalizer2Impl::IX_TOTAL_SIZE; ++i) {
        indexes[i]=totalSize;
    }

    if(beVerbose) {
        printf("size of normalization trie:         %5ld bytes\n", (long)norm16TrieLength);
        printf("size of 16-bit extra data:          %5ld uint16_t\n", (long)extraData.length());
        printf("size of small-FCD data:             %5ld bytes\n", (long)sizeof(smallFCD));
        printf("size of binary data file contents:  %5ld bytes\n", (long)totalSize);
        printf("minDecompNoCodePoint:              U+%04lX\n", (long)indexes[Normalizer2Impl::IX_MIN_DECOMP_NO_CP]);
        printf("minCompNoMaybeCodePoint:           U+%04lX\n", (long)indexes[Normalizer2Impl::IX_MIN_COMP_NO_MAYBE_CP]);
        printf("minLcccCodePoint:                  U+%04lX\n", (long)indexes[Normalizer2Impl::IX_MIN_LCCC_CP]);
        printf("minYesNo: (with compositions)      0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_YES_NO]);
        printf("minYesNoMappingsOnly:              0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_YES_NO_MAPPINGS_ONLY]);
        printf("minNoNo: (comp-normalized)         0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_NO_NO]);
        printf("minNoNoCompBoundaryBefore:         0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_NO_NO_COMP_BOUNDARY_BEFORE]);
        printf("minNoNoCompNoMaybeCC:              0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_NO_NO_COMP_NO_MAYBE_CC]);
        printf("minNoNoEmpty:                      0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_NO_NO_EMPTY]);
        printf("limitNoNo:                         0x%04x\n", (int)indexes[Normalizer2Impl::IX_LIMIT_NO_NO]);
        printf("minNoNoDelta:                      0x%04x\n", (int)minNoNoDelta);
        printf("minMaybeYes:                       0x%04x\n", (int)indexes[Normalizer2Impl::IX_MIN_MAYBE_YES]);
    }

    UVersionInfo nullVersion={ 0, 0, 0, 0 };
    if(0==memcmp(nullVersion, unicodeVersion, 4)) {
        u_versionFromString(unicodeVersion, U_UNICODE_VERSION);
    }
    memcpy(dataInfo.dataVersion, unicodeVersion, 4);
    return builtTrie;
}

void Normalizer2DataBuilder::writeBinaryFile(const char *filename) {
    processData();

    IcuToolErrorCode errorCode("gennorm2/writeBinaryFile()");
    UNewDataMemory *pData=
        udata_create(NULL, NULL, filename, &dataInfo,
                     haveCopyright ? U_COPYRIGHT_STRING : NULL, errorCode);
    if(errorCode.isFailure()) {
        fprintf(stderr, "gennorm2 error: unable to create the output file %s - %s\n",
                filename, errorCode.errorName());
        exit(errorCode.reset());
    }
    udata_writeBlock(pData, indexes, sizeof(indexes));
    udata_writeBlock(pData, norm16TrieBytes, norm16TrieLength);
    udata_writeUString(pData, toUCharPtr(extraData.getBuffer()), extraData.length());
    udata_writeBlock(pData, smallFCD, sizeof(smallFCD));
    int32_t writtenSize=udata_finish(pData, errorCode);
    if(errorCode.isFailure()) {
        fprintf(stderr, "gennorm2: error %s writing the output file\n", errorCode.errorName());
        exit(errorCode.reset());
    }
    int32_t totalSize=indexes[Normalizer2Impl::IX_TOTAL_SIZE];
    if(writtenSize!=totalSize) {
        fprintf(stderr, "gennorm2 error: written size %ld != calculated size %ld\n",
            (long)writtenSize, (long)totalSize);
        exit(U_INTERNAL_PROGRAM_ERROR);
    }
}

void
Normalizer2DataBuilder::writeCSourceFile(const char *filename) {
    LocalUCPTriePointer norm16Trie = processData();

    IcuToolErrorCode errorCode("gennorm2/writeCSourceFile()");
    const char *basename=findBasename(filename);
    CharString path(filename, (int32_t)(basename-filename), errorCode);
    CharString dataName(basename, errorCode);
    const char *extension=strrchr(basename, '.');
    if(extension!=NULL) {
        dataName.truncate((int32_t)(extension-basename));
    }
    const char *name=dataName.data();
    errorCode.assertSuccess();

    FILE *f=usrc_create(path.data(), basename, 2016, "icu/source/tools/gennorm2/n2builder.cpp");
    if(f==NULL) {
        fprintf(stderr, "gennorm2/writeCSourceFile() error: unable to create the output file %s\n",
                filename);
        exit(U_FILE_ACCESS_ERROR);
    }
    fputs("#ifdef INCLUDED_FROM_NORMALIZER2_CPP\n\n", f);

    char line[100];
    sprintf(line, "static const UVersionInfo %s_formatVersion={", name);
    usrc_writeArray(f, line, dataInfo.formatVersion, 8, 4, "};\n");
    sprintf(line, "static const UVersionInfo %s_dataVersion={", name);
    usrc_writeArray(f, line, dataInfo.dataVersion, 8, 4, "};\n\n");
    sprintf(line, "static const int32_t %s_indexes[Normalizer2Impl::IX_COUNT]={\n", name);
    usrc_writeArray(f, line, indexes, 32, Normalizer2Impl::IX_COUNT, "\n};\n\n");

    usrc_writeUCPTrie(f, name, norm16Trie.getAlias());

    sprintf(line, "static const uint16_t %s_extraData[%%ld]={\n", name);
    usrc_writeArray(f, line, extraData.getBuffer(), 16, extraData.length(), "\n};\n\n");
    sprintf(line, "static const uint8_t %s_smallFCD[%%ld]={\n", name);
    usrc_writeArray(f, line, smallFCD, 8, sizeof(smallFCD), "\n};\n\n");

    fputs("#endif  // INCLUDED_FROM_NORMALIZER2_CPP\n", f);
    fclose(f);
}

namespace {

bool equalStrings(const UnicodeString *s1, const UnicodeString *s2) {
    if(s1 == nullptr) {
        return s2 == nullptr;
    } else if(s2 == nullptr) {
        return false;
    } else {
        return *s1 == *s2;
    }
}

const char *typeChars = "?-=>";

void writeMapping(FILE *f, const UnicodeString *m) {
    if(m != nullptr && !m->isEmpty()) {
        int32_t i = 0;
        UChar32 c = m->char32At(i);
        fprintf(f, "%04lX", (long)c);
        while((i += U16_LENGTH(c)) < m->length()) {
            c = m->char32At(i);
            fprintf(f, " %04lX", (long)c);
        }
    }
    fputs("\n", f);
}

}  // namespace

void
Normalizer2DataBuilder::writeDataFile(const char *filename, bool writeRemoved) const {
    // Do not processData() before writing the input-syntax data file.
    FILE *f = fopen(filename, "w");
    if(f == nullptr) {
        fprintf(stderr, "gennorm2/writeDataFile() error: unable to create the output file %s\n",
                filename);
        exit(U_FILE_ACCESS_ERROR);
        return;
    }

    if(unicodeVersion[0] != 0 || unicodeVersion[1] != 0 ||
            unicodeVersion[2] != 0 || unicodeVersion[3] != 0) {
        char uv[U_MAX_VERSION_STRING_LENGTH];
        u_versionToString(unicodeVersion, uv);
        fprintf(f, "* Unicode %s\n\n", uv);
    }

    UnicodeSetIterator ccIter(norms.ccSet);
    UChar32 start = U_SENTINEL;
    UChar32 end = U_SENTINEL;
    uint8_t prevCC = 0;
    bool done = false;
    bool didWrite = false;
    do {
        UChar32 c;
        uint8_t cc;
        if(ccIter.next() && !ccIter.isString()) {
            c = ccIter.getCodepoint();
            cc = norms.getCC(c);
        } else {
            c = 0x110000;
            cc = 0;
            done = true;
        }
        if(cc == prevCC && c == (end + 1)) {
            end = c;
        } else {
            if(prevCC != 0) {
                if(start == end) {
                    fprintf(f, "%04lX:%d\n", (long)start, (int)prevCC);
                } else {
                    fprintf(f, "%04lX..%04lX:%d\n", (long)start, (long)end, (int)prevCC);
                }
                didWrite = true;
            }
            start = end = c;
            prevCC = cc;
        }
    } while(!done);
    if(didWrite) {
        fputs("\n", f);
    }

    UnicodeSetIterator mIter(norms.mappingSet);
    start = U_SENTINEL;
    end = U_SENTINEL;
    const UnicodeString *prevMapping = nullptr;
    Norm::MappingType prevType = Norm::NONE;
    done = false;
    do {
        UChar32 c;
        const Norm *norm;
        if(mIter.next() && !mIter.isString()) {
            c = mIter.getCodepoint();
            norm = norms.getNorm(c);
        } else {
            c = 0x110000;
            norm = nullptr;
            done = true;
        }
        const UnicodeString *mapping;
        Norm::MappingType type;
        if(norm == nullptr) {
            mapping = nullptr;
            type = Norm::NONE;
        } else {
            type = norm->mappingType;
            if(type == Norm::NONE) {
                mapping = nullptr;
            } else {
                mapping = norm->mapping;
            }
        }
        if(type == prevType && equalStrings(mapping, prevMapping) && c == (end + 1)) {
            end = c;
        } else {
            if(writeRemoved ? prevType != Norm::NONE : prevType > Norm::REMOVED) {
                if(start == end) {
                    fprintf(f, "%04lX%c", (long)start, typeChars[prevType]);
                } else {
                    fprintf(f, "%04lX..%04lX%c", (long)start, (long)end, typeChars[prevType]);
                }
                writeMapping(f, prevMapping);
            }
            start = end = c;
            prevMapping = mapping;
            prevType = type;
        }
    } while(!done);

    fclose(f);
}

void
Normalizer2DataBuilder::computeDiff(const Normalizer2DataBuilder &b1,
                                    const Normalizer2DataBuilder &b2,
                                    Normalizer2DataBuilder &diff) {
    // Compute diff = b1 - b2
    // so that we should be able to get b1 = b2 + diff.
    if(0 != memcmp(b1.unicodeVersion, b2.unicodeVersion, U_MAX_VERSION_LENGTH)) {
        memcpy(diff.unicodeVersion, b1.unicodeVersion, U_MAX_VERSION_LENGTH);
    }

    UnicodeSet ccSet(b1.norms.ccSet);
    ccSet.addAll(b2.norms.ccSet);
    UnicodeSetIterator ccIter(ccSet);
    while(ccIter.next() && !ccIter.isString()) {
        UChar32 c = ccIter.getCodepoint();
        uint8_t cc1 = b1.norms.getCC(c);
        uint8_t cc2 = b2.norms.getCC(c);
        if(cc1 != cc2) {
            diff.setCC(c, cc1);
        }
    }

    UnicodeSet mSet(b1.norms.mappingSet);
    mSet.addAll(b2.norms.mappingSet);
    UnicodeSetIterator mIter(mSet);
    while(mIter.next() && !mIter.isString()) {
        UChar32 c = mIter.getCodepoint();
        const Norm *norm1 = b1.norms.getNorm(c);
        const Norm *norm2 = b2.norms.getNorm(c);
        const UnicodeString *mapping1;
        Norm::MappingType type1;
        if(norm1 == nullptr || !norm1->hasMapping()) {
            mapping1 = nullptr;
            type1 = Norm::NONE;
        } else {
            mapping1 = norm1->mapping;
            type1 = norm1->mappingType;
        }
        const UnicodeString *mapping2;
        Norm::MappingType type2;
        if(norm2 == nullptr || !norm2->hasMapping()) {
            mapping2 = nullptr;
            type2 = Norm::NONE;
        } else {
            mapping2 = norm2->mapping;
            type2 = norm2->mappingType;
        }
        if(type1 == type2 && equalStrings(mapping1, mapping2)) {
            // Nothing to do.
        } else if(type1 == Norm::NONE) {
            diff.removeMapping(c);
        } else if(type1 == Norm::ROUND_TRIP) {
            diff.setRoundTripMapping(c, *mapping1);
        } else if(type1 == Norm::ONE_WAY) {
            diff.setOneWayMapping(c, *mapping1);
        }
    }
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_NORMALIZATION */

/*
 * Hey, Emacs, please set the following:
 *
 * Local Variables:
 * indent-tabs-mode: nil
 * End:
 */
