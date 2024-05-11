#include "mongo/util/icu.h"

#include <unicode/uchar.h>
#include <unicode/utf8.h>

namespace mongo {
namespace {

int getColumnWidth(UChar32 codepoint, bool ambiguousAsFullWidth) {
    const int eaw = u_getIntPropertyValue(codepoint, UCHAR_EAST_ASIAN_WIDTH);
    switch (eaw) {
        case U_EA_FULLWIDTH:
        case U_EA_WIDE:
            return 2;
        case U_EA_AMBIGUOUS:
            if (ambiguousAsFullWidth) {
                return 2;
            }
            [[fallthrough]];
        case U_EA_NEUTRAL:
            if (u_hasBinaryProperty(codepoint, UCHAR_EMOJI_PRESENTATION)) {
                return 2;
            }
            [[fallthrough]];
        case U_EA_HALFWIDTH:
        case U_EA_NARROW:
        default:
            const auto zero_width_mask = U_GC_CC_MASK |  // C0/C1 control code
                U_GC_CF_MASK |                           // Format control character
                U_GC_ME_MASK |                           // Enclosing mark
                U_GC_MN_MASK;                            // Nonspacing mark
            if (codepoint != 0x00AD &&                   // SOFT HYPHEN is Cf but not zero-width
                ((U_MASK(u_charType(codepoint)) & zero_width_mask) ||
                 u_hasBinaryProperty(codepoint, UCHAR_EMOJI_MODIFIER))) {
                return 0;
            }
            return 1;
    }
}
}  // namespace

// This is a modified version of GetStringWidth from node. We don't currently support wide strings
// in the test runner, so it has been converted to use the U8 cursor API. For more details on
// string width calculation see: https://www.unicode.org/reports/tr11/
int icuGetStringWidth(StringData value, bool ambiguousAsFullWidth, bool expandEmojiSequence) {
    const uint8_t* str = reinterpret_cast<const uint8_t*>(value.data());
    UChar32 output = 0;
    UChar32 previous;
    int offset = 0;
    int strLength = static_cast<int>(value.length());
    int width = 0;

    while (offset < strLength) {
        previous = output;
        U8_NEXT(str, offset, strLength, output);

        if (!expandEmojiSequence && offset > 0 && previous == 0x200d &&
            (u_hasBinaryProperty(output, UCHAR_EMOJI_PRESENTATION) ||
             u_hasBinaryProperty(output, UCHAR_EMOJI_MODIFIER))) {
            continue;
        }

        width += getColumnWidth(output, ambiguousAsFullWidth);
    }

    return width;
}

}  // namespace mongo
