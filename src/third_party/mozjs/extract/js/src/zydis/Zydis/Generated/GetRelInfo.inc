const ZydisEncoderRelInfo *ZydisGetRelInfo(ZydisMnemonic mnemonic)
{
    static const ZydisEncoderRelInfo info_lookup[9] =
    {
        { { { 0, 3, 6 }, { 0, 4, 5 }, { 0, 0, 5 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_FALSE },
        { { { 2, 4, 7 }, { 2, 5, 6 }, { 2, 0, 6 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_TRUE },
        { { { 2, 0, 0 }, { 3, 0, 0 }, { 0, 0, 0 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_FALSE },
        { { { 3, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_FALSE },
        { { { 0, 0, 0 }, { 0, 0, 0 }, { 5, 0, 7 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_FALSE },
        { { { 2, 3, 6 }, { 2, 4, 5 }, { 2, 0, 5 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_FALSE },
        { { { 0, 0, 0 }, { 0, 0, 0 }, { 2, 0, 0 } }, ZYDIS_SIZE_HINT_NONE, ZYAN_FALSE },
        { { { 2, 0, 0 }, { 2, 0, 0 }, { 2, 0, 0 } }, ZYDIS_SIZE_HINT_ASZ, ZYAN_FALSE },
        { { { 0, 4, 7 }, { 0, 5, 6 }, { 0, 5, 6 } }, ZYDIS_SIZE_HINT_OSZ, ZYAN_FALSE },
    };

    switch (mnemonic)
    {
    case ZYDIS_MNEMONIC_CALL:
        return &info_lookup[0];
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JZ:
        return &info_lookup[1];
    case ZYDIS_MNEMONIC_JCXZ:
        return &info_lookup[2];
    case ZYDIS_MNEMONIC_JECXZ:
        return &info_lookup[3];
    case ZYDIS_MNEMONIC_JKNZD:
    case ZYDIS_MNEMONIC_JKZD:
        return &info_lookup[4];
    case ZYDIS_MNEMONIC_JMP:
        return &info_lookup[5];
    case ZYDIS_MNEMONIC_JRCXZ:
        return &info_lookup[6];
    case ZYDIS_MNEMONIC_LOOP:
    case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
        return &info_lookup[7];
    case ZYDIS_MNEMONIC_XBEGIN:
        return &info_lookup[8];
    default:
        return ZYAN_NULL;
    }
}

