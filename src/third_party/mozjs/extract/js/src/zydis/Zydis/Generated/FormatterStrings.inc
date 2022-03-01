#pragma pack(push, 1)
static const ZydisShortString STR_ADD = ZYDIS_MAKE_SHORTSTRING("+");
static const struct ZydisPredefinedTokenADD_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_ADD = { 4, 2, { ZYDIS_TOKEN_DELIMITER, 0, '+', '\0' } };
static const ZydisPredefinedToken* const TOK_ADD = (const ZydisPredefinedToken* const)&TOK_DATA_ADD;
static const ZydisShortString STR_ADDR_RELATIVE = ZYDIS_MAKE_SHORTSTRING("$");
static const struct ZydisPredefinedTokenADDR_RELATIVE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_ADDR_RELATIVE = { 4, 2, { ZYDIS_TOKEN_ADDRESS_REL, 0, '$', '\0' } };
static const ZydisPredefinedToken* const TOK_ADDR_RELATIVE = (const ZydisPredefinedToken* const)&TOK_DATA_ADDR_RELATIVE;
static const ZydisShortString STR_DECO_1TO2 = ZYDIS_MAKE_SHORTSTRING(" {1to2}");
static const struct ZydisPredefinedTokenDECO_1TO2_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_1TO2 = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, '1', 't', 'o', '2', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_1TO2 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_1TO2;
static const ZydisShortString STR_DECO_1TO4 = ZYDIS_MAKE_SHORTSTRING(" {1to4}");
static const struct ZydisPredefinedTokenDECO_1TO4_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_1TO4 = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, '1', 't', 'o', '4', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_1TO4 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_1TO4;
static const ZydisShortString STR_DECO_1TO8 = ZYDIS_MAKE_SHORTSTRING(" {1to8}");
static const struct ZydisPredefinedTokenDECO_1TO8_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_1TO8 = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, '1', 't', 'o', '8', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_1TO8 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_1TO8;
static const ZydisShortString STR_DECO_1TO16 = ZYDIS_MAKE_SHORTSTRING(" {1to16}");
static const struct ZydisPredefinedTokenDECO_1TO16_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[20];
} TOK_DATA_DECO_1TO16 = { 20, 18, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 6, '1', 't', 'o', '1', '6', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_1TO16 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_1TO16;
static const ZydisShortString STR_DECO_4TO8 = ZYDIS_MAKE_SHORTSTRING(" {4to8}");
static const struct ZydisPredefinedTokenDECO_4TO8_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_4TO8 = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, '4', 't', 'o', '8', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_4TO8 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_4TO8;
static const ZydisShortString STR_DECO_4TO16 = ZYDIS_MAKE_SHORTSTRING(" {4to16}");
static const struct ZydisPredefinedTokenDECO_4TO16_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[20];
} TOK_DATA_DECO_4TO16 = { 20, 18, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 6, '4', 't', 'o', '1', '6', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_4TO16 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_4TO16;
static const ZydisShortString STR_DECO_AAAA = ZYDIS_MAKE_SHORTSTRING(" {aaaa}");
static const struct ZydisPredefinedTokenDECO_AAAA_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_AAAA = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'a', 'a', 'a', 'a', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_AAAA = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_AAAA;
static const ZydisShortString STR_DECO_BADC = ZYDIS_MAKE_SHORTSTRING(" {badc}");
static const struct ZydisPredefinedTokenDECO_BADC_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_BADC = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'b', 'a', 'd', 'c', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_BADC = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_BADC;
static const ZydisShortString STR_DECO_BBBB = ZYDIS_MAKE_SHORTSTRING(" {bbbb}");
static const struct ZydisPredefinedTokenDECO_BBBB_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_BBBB = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'b', 'b', 'b', 'b', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_BBBB = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_BBBB;
static const ZydisShortString STR_DECO_BEGIN = ZYDIS_MAKE_SHORTSTRING(" {");
static const struct ZydisPredefinedTokenDECO_BEGIN_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[8];
} TOK_DATA_DECO_BEGIN = { 8, 6, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 0, '{', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_BEGIN = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_BEGIN;
static const ZydisShortString STR_DECO_CCCC = ZYDIS_MAKE_SHORTSTRING(" {cccc}");
static const struct ZydisPredefinedTokenDECO_CCCC_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_CCCC = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'c', 'c', 'c', 'c', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_CCCC = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_CCCC;
static const ZydisShortString STR_DECO_CDAB = ZYDIS_MAKE_SHORTSTRING(" {cdab}");
static const struct ZydisPredefinedTokenDECO_CDAB_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_CDAB = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'c', 'd', 'a', 'b', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_CDAB = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_CDAB;
static const ZydisShortString STR_DECO_DACB = ZYDIS_MAKE_SHORTSTRING(" {dacb}");
static const struct ZydisPredefinedTokenDECO_DACB_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_DACB = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'd', 'a', 'c', 'b', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_DACB = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_DACB;
static const ZydisShortString STR_DECO_DDDD = ZYDIS_MAKE_SHORTSTRING(" {dddd}");
static const struct ZydisPredefinedTokenDECO_DDDD_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_DDDD = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'd', 'd', 'd', 'd', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_DDDD = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_DDDD;
static const ZydisShortString STR_DECO_EH = ZYDIS_MAKE_SHORTSTRING(" {cdab}");
static const struct ZydisPredefinedTokenDECO_EH_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[19];
} TOK_DATA_DECO_EH = { 19, 17, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 5, 'c', 'd', 'a', 'b', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_EH = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_EH;
static const ZydisShortString STR_DECO_END = ZYDIS_MAKE_SHORTSTRING("}");
static const struct ZydisPredefinedTokenDECO_END_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_DECO_END = { 4, 2, { ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_END = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_END;
static const ZydisShortString STR_DECO_FLOAT16 = ZYDIS_MAKE_SHORTSTRING(" {float16}");
static const struct ZydisPredefinedTokenDECO_FLOAT16_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[22];
} TOK_DATA_DECO_FLOAT16 = { 22, 20, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 8, 'f', 'l', 'o', 'a', 't', '1', '6', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_FLOAT16 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_FLOAT16;
static const ZydisShortString STR_DECO_RD = ZYDIS_MAKE_SHORTSTRING(" {rd}");
static const struct ZydisPredefinedTokenDECO_RD_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[17];
} TOK_DATA_DECO_RD = { 17, 15, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 3, 'r', 'd', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RD = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RD;
static const ZydisShortString STR_DECO_RD_SAE = ZYDIS_MAKE_SHORTSTRING(" {rd-sae}");
static const struct ZydisPredefinedTokenDECO_RD_SAE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[21];
} TOK_DATA_DECO_RD_SAE = { 21, 19, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 7, 'r', 'd', '-', 's', 'a', 'e', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RD_SAE = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RD_SAE;
static const ZydisShortString STR_DECO_RN = ZYDIS_MAKE_SHORTSTRING(" {rn}");
static const struct ZydisPredefinedTokenDECO_RN_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[17];
} TOK_DATA_DECO_RN = { 17, 15, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 3, 'r', 'n', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RN = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RN;
static const ZydisShortString STR_DECO_RN_SAE = ZYDIS_MAKE_SHORTSTRING(" {rn-sae}");
static const struct ZydisPredefinedTokenDECO_RN_SAE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[21];
} TOK_DATA_DECO_RN_SAE = { 21, 19, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 7, 'r', 'n', '-', 's', 'a', 'e', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RN_SAE = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RN_SAE;
static const ZydisShortString STR_DECO_RU = ZYDIS_MAKE_SHORTSTRING(" {ru}");
static const struct ZydisPredefinedTokenDECO_RU_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[17];
} TOK_DATA_DECO_RU = { 17, 15, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 3, 'r', 'u', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RU = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RU;
static const ZydisShortString STR_DECO_RU_SAE = ZYDIS_MAKE_SHORTSTRING(" {ru-sae}");
static const struct ZydisPredefinedTokenDECO_RU_SAE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[21];
} TOK_DATA_DECO_RU_SAE = { 21, 19, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 7, 'r', 'u', '-', 's', 'a', 'e', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RU_SAE = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RU_SAE;
static const ZydisShortString STR_DECO_RZ = ZYDIS_MAKE_SHORTSTRING(" {rz}");
static const struct ZydisPredefinedTokenDECO_RZ_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[17];
} TOK_DATA_DECO_RZ = { 17, 15, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 3, 'r', 'z', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RZ = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RZ;
static const ZydisShortString STR_DECO_RZ_SAE = ZYDIS_MAKE_SHORTSTRING(" {rz-sae}");
static const struct ZydisPredefinedTokenDECO_RZ_SAE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[21];
} TOK_DATA_DECO_RZ_SAE = { 21, 19, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 7, 'r', 'z', '-', 's', 'a', 'e', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_RZ_SAE = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_RZ_SAE;
static const ZydisShortString STR_DECO_SAE = ZYDIS_MAKE_SHORTSTRING(" {sae}");
static const struct ZydisPredefinedTokenDECO_SAE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[18];
} TOK_DATA_DECO_SAE = { 18, 16, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 4, 's', 'a', 'e', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_SAE = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_SAE;
static const ZydisShortString STR_DECO_SINT8 = ZYDIS_MAKE_SHORTSTRING(" {sint8}");
static const struct ZydisPredefinedTokenDECO_SINT8_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[20];
} TOK_DATA_DECO_SINT8 = { 20, 18, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 6, 's', 'i', 'n', 't', '8', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_SINT8 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_SINT8;
static const ZydisShortString STR_DECO_SINT16 = ZYDIS_MAKE_SHORTSTRING(" {sint16}");
static const struct ZydisPredefinedTokenDECO_SINT16_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[21];
} TOK_DATA_DECO_SINT16 = { 21, 19, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 7, 's', 'i', 'n', 't', '1', '6', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_SINT16 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_SINT16;
static const ZydisShortString STR_DECO_UINT8 = ZYDIS_MAKE_SHORTSTRING(" {uint8}");
static const struct ZydisPredefinedTokenDECO_UINT8_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[20];
} TOK_DATA_DECO_UINT8 = { 20, 18, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 6, 'u', 'i', 'n', 't', '8', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_UINT8 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_UINT8;
static const ZydisShortString STR_DECO_UINT16 = ZYDIS_MAKE_SHORTSTRING(" {uint16}");
static const struct ZydisPredefinedTokenDECO_UINT16_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[21];
} TOK_DATA_DECO_UINT16 = { 21, 19, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 7, 'u', 'i', 'n', 't', '1', '6', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_UINT16 = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_UINT16;
static const ZydisShortString STR_DECO_ZERO = ZYDIS_MAKE_SHORTSTRING(" {z}");
static const struct ZydisPredefinedTokenDECO_ZERO_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[16];
} TOK_DATA_DECO_ZERO = { 16, 14, { ZYDIS_TOKEN_WHITESPACE, 2, ' ', '\0', ZYDIS_TOKEN_PARENTHESIS_OPEN, 2, '{', '\0', ZYDIS_TOKEN_DECORATOR, 2, 'z', '\0', ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, '}', '\0' } };
static const ZydisPredefinedToken* const TOK_DECO_ZERO = (const ZydisPredefinedToken* const)&TOK_DATA_DECO_ZERO;
static const ZydisShortString STR_DELIM_MEMORY = ZYDIS_MAKE_SHORTSTRING(",");
static const struct ZydisPredefinedTokenDELIM_MEMORY_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_DELIM_MEMORY = { 4, 2, { ZYDIS_TOKEN_DELIMITER, 0, ',', '\0' } };
static const ZydisPredefinedToken* const TOK_DELIM_MEMORY = (const ZydisPredefinedToken* const)&TOK_DATA_DELIM_MEMORY;
static const ZydisShortString STR_DELIM_MNEMONIC = ZYDIS_MAKE_SHORTSTRING(" ");
static const struct ZydisPredefinedTokenDELIM_MNEMONIC_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_DELIM_MNEMONIC = { 4, 2, { ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_DELIM_MNEMONIC = (const ZydisPredefinedToken* const)&TOK_DATA_DELIM_MNEMONIC;
static const ZydisShortString STR_DELIM_OPERAND = ZYDIS_MAKE_SHORTSTRING(", ");
static const struct ZydisPredefinedTokenDELIM_OPERAND_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[8];
} TOK_DATA_DELIM_OPERAND = { 8, 6, { ZYDIS_TOKEN_DELIMITER, 2, ',', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_DELIM_OPERAND = (const ZydisPredefinedToken* const)&TOK_DATA_DELIM_OPERAND;
static const ZydisShortString STR_DELIM_SEGMENT = ZYDIS_MAKE_SHORTSTRING(":");
static const struct ZydisPredefinedTokenDELIM_SEGMENT_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_DELIM_SEGMENT = { 4, 2, { ZYDIS_TOKEN_DELIMITER, 0, ':', '\0' } };
static const ZydisPredefinedToken* const TOK_DELIM_SEGMENT = (const ZydisPredefinedToken* const)&TOK_DATA_DELIM_SEGMENT;
static const ZydisShortString STR_FAR = ZYDIS_MAKE_SHORTSTRING(" far");
static const ZydisShortString STR_FAR_ATT = ZYDIS_MAKE_SHORTSTRING("l");
static const ZydisShortString STR_IMMEDIATE = ZYDIS_MAKE_SHORTSTRING("$");
static const struct ZydisPredefinedTokenIMMEDIATE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_IMMEDIATE = { 4, 2, { ZYDIS_TOKEN_IMMEDIATE, 0, '$', '\0' } };
static const ZydisPredefinedToken* const TOK_IMMEDIATE = (const ZydisPredefinedToken* const)&TOK_DATA_IMMEDIATE;
static const ZydisShortString STR_INVALID_MNEMONIC = ZYDIS_MAKE_SHORTSTRING("invalid");
static const struct ZydisPredefinedTokenINVALID_MNEMONIC_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[10];
} TOK_DATA_INVALID_MNEMONIC = { 10, 2, { ZYDIS_TOKEN_MNEMONIC, 0, 'i', 'n', 'v', 'a', 'l', 'i', 'd', '\0' } };
static const ZydisPredefinedToken* const TOK_INVALID_MNEMONIC = (const ZydisPredefinedToken* const)&TOK_DATA_INVALID_MNEMONIC;
static const ZydisShortString STR_INVALID_REG = ZYDIS_MAKE_SHORTSTRING("invalid");
static const struct ZydisPredefinedTokenINVALID_REG_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[10];
} TOK_DATA_INVALID_REG = { 10, 2, { ZYDIS_TOKEN_REGISTER, 0, 'i', 'n', 'v', 'a', 'l', 'i', 'd', '\0' } };
static const ZydisPredefinedToken* const TOK_INVALID_REG = (const ZydisPredefinedToken* const)&TOK_DATA_INVALID_REG;
static const ZydisShortString STR_MEMORY_BEGIN_ATT = ZYDIS_MAKE_SHORTSTRING("(");
static const struct ZydisPredefinedTokenMEMORY_BEGIN_ATT_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_MEMORY_BEGIN_ATT = { 4, 2, { ZYDIS_TOKEN_PARENTHESIS_OPEN, 0, '(', '\0' } };
static const ZydisPredefinedToken* const TOK_MEMORY_BEGIN_ATT = (const ZydisPredefinedToken* const)&TOK_DATA_MEMORY_BEGIN_ATT;
static const ZydisShortString STR_MEMORY_BEGIN_INTEL = ZYDIS_MAKE_SHORTSTRING("[");
static const struct ZydisPredefinedTokenMEMORY_BEGIN_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_MEMORY_BEGIN_INTEL = { 4, 2, { ZYDIS_TOKEN_PARENTHESIS_OPEN, 0, '[', '\0' } };
static const ZydisPredefinedToken* const TOK_MEMORY_BEGIN_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_MEMORY_BEGIN_INTEL;
static const ZydisShortString STR_MEMORY_END_ATT = ZYDIS_MAKE_SHORTSTRING(")");
static const struct ZydisPredefinedTokenMEMORY_END_ATT_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_MEMORY_END_ATT = { 4, 2, { ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, ')', '\0' } };
static const ZydisPredefinedToken* const TOK_MEMORY_END_ATT = (const ZydisPredefinedToken* const)&TOK_DATA_MEMORY_END_ATT;
static const ZydisShortString STR_MEMORY_END_INTEL = ZYDIS_MAKE_SHORTSTRING("]");
static const struct ZydisPredefinedTokenMEMORY_END_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_MEMORY_END_INTEL = { 4, 2, { ZYDIS_TOKEN_PARENTHESIS_CLOSE, 0, ']', '\0' } };
static const ZydisPredefinedToken* const TOK_MEMORY_END_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_MEMORY_END_INTEL;
static const ZydisShortString STR_MUL = ZYDIS_MAKE_SHORTSTRING("*");
static const struct ZydisPredefinedTokenMUL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_MUL = { 4, 2, { ZYDIS_TOKEN_DELIMITER, 0, '*', '\0' } };
static const ZydisPredefinedToken* const TOK_MUL = (const ZydisPredefinedToken* const)&TOK_DATA_MUL;
static const ZydisShortString STR_NEAR = ZYDIS_MAKE_SHORTSTRING(" near");
static const ZydisShortString STR_PREF_BND = ZYDIS_MAKE_SHORTSTRING("bnd ");
static const struct ZydisPredefinedTokenPREF_BND_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[10];
} TOK_DATA_PREF_BND = { 10, 8, { ZYDIS_TOKEN_PREFIX, 4, 'b', 'n', 'd', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_BND = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_BND;
static const ZydisShortString STR_PREF_LOCK = ZYDIS_MAKE_SHORTSTRING("lock ");
static const struct ZydisPredefinedTokenPREF_LOCK_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[11];
} TOK_DATA_PREF_LOCK = { 11, 9, { ZYDIS_TOKEN_PREFIX, 5, 'l', 'o', 'c', 'k', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_LOCK = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_LOCK;
static const ZydisShortString STR_PREF_REP = ZYDIS_MAKE_SHORTSTRING("rep ");
static const struct ZydisPredefinedTokenPREF_REP_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[10];
} TOK_DATA_PREF_REP = { 10, 8, { ZYDIS_TOKEN_PREFIX, 4, 'r', 'e', 'p', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REP = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REP;
static const ZydisShortString STR_PREF_REPE = ZYDIS_MAKE_SHORTSTRING("repe ");
static const struct ZydisPredefinedTokenPREF_REPE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[11];
} TOK_DATA_PREF_REPE = { 11, 9, { ZYDIS_TOKEN_PREFIX, 5, 'r', 'e', 'p', 'e', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REPE = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REPE;
static const ZydisShortString STR_PREF_REPNE = ZYDIS_MAKE_SHORTSTRING("repne ");
static const struct ZydisPredefinedTokenPREF_REPNE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[12];
} TOK_DATA_PREF_REPNE = { 12, 10, { ZYDIS_TOKEN_PREFIX, 6, 'r', 'e', 'p', 'n', 'e', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REPNE = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REPNE;
static const ZydisShortString STR_PREF_REX_4A = ZYDIS_MAKE_SHORTSTRING("rex.wx ");
static const struct ZydisPredefinedTokenPREF_REX_4A_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[13];
} TOK_DATA_PREF_REX_4A = { 13, 11, { ZYDIS_TOKEN_PREFIX, 7, 'r', 'e', 'x', '.', 'w', 'x', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_4A = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4A;
static const ZydisShortString STR_PREF_REX_4B = ZYDIS_MAKE_SHORTSTRING("rex.wxb ");
static const struct ZydisPredefinedTokenPREF_REX_4B_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[14];
} TOK_DATA_PREF_REX_4B = { 14, 12, { ZYDIS_TOKEN_PREFIX, 8, 'r', 'e', 'x', '.', 'w', 'x', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_4B = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4B;
static const ZydisShortString STR_PREF_REX_4C = ZYDIS_MAKE_SHORTSTRING("rex.wr ");
static const struct ZydisPredefinedTokenPREF_REX_4C_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[13];
} TOK_DATA_PREF_REX_4C = { 13, 11, { ZYDIS_TOKEN_PREFIX, 7, 'r', 'e', 'x', '.', 'w', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_4C = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4C;
static const ZydisShortString STR_PREF_REX_4D = ZYDIS_MAKE_SHORTSTRING("rex.wrb ");
static const struct ZydisPredefinedTokenPREF_REX_4D_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[14];
} TOK_DATA_PREF_REX_4D = { 14, 12, { ZYDIS_TOKEN_PREFIX, 8, 'r', 'e', 'x', '.', 'w', 'r', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_4D = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4D;
static const ZydisShortString STR_PREF_REX_4E = ZYDIS_MAKE_SHORTSTRING("rex.wrx ");
static const struct ZydisPredefinedTokenPREF_REX_4E_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[14];
} TOK_DATA_PREF_REX_4E = { 14, 12, { ZYDIS_TOKEN_PREFIX, 8, 'r', 'e', 'x', '.', 'w', 'r', 'x', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_4E = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4E;
static const ZydisShortString STR_PREF_REX_4F = ZYDIS_MAKE_SHORTSTRING("rex.wrxb ");
static const struct ZydisPredefinedTokenPREF_REX_4F_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[15];
} TOK_DATA_PREF_REX_4F = { 15, 13, { ZYDIS_TOKEN_PREFIX, 9, 'r', 'e', 'x', '.', 'w', 'r', 'x', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_4F = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4F;
static const ZydisShortString STR_PREF_REX_40 = ZYDIS_MAKE_SHORTSTRING("rex ");
static const struct ZydisPredefinedTokenPREF_REX_40_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[10];
} TOK_DATA_PREF_REX_40 = { 10, 8, { ZYDIS_TOKEN_PREFIX, 4, 'r', 'e', 'x', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_40 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_40;
static const ZydisShortString STR_PREF_REX_41 = ZYDIS_MAKE_SHORTSTRING("rex.b ");
static const struct ZydisPredefinedTokenPREF_REX_41_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[12];
} TOK_DATA_PREF_REX_41 = { 12, 10, { ZYDIS_TOKEN_PREFIX, 6, 'r', 'e', 'x', '.', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_41 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_41;
static const ZydisShortString STR_PREF_REX_42 = ZYDIS_MAKE_SHORTSTRING("rex.x ");
static const struct ZydisPredefinedTokenPREF_REX_42_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[12];
} TOK_DATA_PREF_REX_42 = { 12, 10, { ZYDIS_TOKEN_PREFIX, 6, 'r', 'e', 'x', '.', 'x', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_42 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_42;
static const ZydisShortString STR_PREF_REX_43 = ZYDIS_MAKE_SHORTSTRING("rex.xb ");
static const struct ZydisPredefinedTokenPREF_REX_43_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[13];
} TOK_DATA_PREF_REX_43 = { 13, 11, { ZYDIS_TOKEN_PREFIX, 7, 'r', 'e', 'x', '.', 'x', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_43 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_43;
static const ZydisShortString STR_PREF_REX_44 = ZYDIS_MAKE_SHORTSTRING("rex.r ");
static const struct ZydisPredefinedTokenPREF_REX_44_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[12];
} TOK_DATA_PREF_REX_44 = { 12, 10, { ZYDIS_TOKEN_PREFIX, 6, 'r', 'e', 'x', '.', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_44 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_44;
static const ZydisShortString STR_PREF_REX_45 = ZYDIS_MAKE_SHORTSTRING("rex.rb ");
static const struct ZydisPredefinedTokenPREF_REX_45_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[13];
} TOK_DATA_PREF_REX_45 = { 13, 11, { ZYDIS_TOKEN_PREFIX, 7, 'r', 'e', 'x', '.', 'r', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_45 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_45;
static const ZydisShortString STR_PREF_REX_46 = ZYDIS_MAKE_SHORTSTRING("rex.rx ");
static const struct ZydisPredefinedTokenPREF_REX_46_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[13];
} TOK_DATA_PREF_REX_46 = { 13, 11, { ZYDIS_TOKEN_PREFIX, 7, 'r', 'e', 'x', '.', 'r', 'x', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_46 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_46;
static const ZydisShortString STR_PREF_REX_47 = ZYDIS_MAKE_SHORTSTRING("rex.rxb ");
static const struct ZydisPredefinedTokenPREF_REX_47_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[14];
} TOK_DATA_PREF_REX_47 = { 14, 12, { ZYDIS_TOKEN_PREFIX, 8, 'r', 'e', 'x', '.', 'r', 'x', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_47 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_47;
static const ZydisShortString STR_PREF_REX_48 = ZYDIS_MAKE_SHORTSTRING("rex.w ");
static const struct ZydisPredefinedTokenPREF_REX_48_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[12];
} TOK_DATA_PREF_REX_48 = { 12, 10, { ZYDIS_TOKEN_PREFIX, 6, 'r', 'e', 'x', '.', 'w', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_48 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_48;
static const ZydisShortString STR_PREF_REX_49 = ZYDIS_MAKE_SHORTSTRING("rex.wb ");
static const struct ZydisPredefinedTokenPREF_REX_49_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[13];
} TOK_DATA_PREF_REX_49 = { 13, 11, { ZYDIS_TOKEN_PREFIX, 7, 'r', 'e', 'x', '.', 'w', 'b', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_REX_49 = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_49;
static const ZydisShortString STR_PREF_SEG_CS = ZYDIS_MAKE_SHORTSTRING("cs ");
static const struct ZydisPredefinedTokenPREF_SEG_CS_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[9];
} TOK_DATA_PREF_SEG_CS = { 9, 7, { ZYDIS_TOKEN_PREFIX, 3, 'c', 's', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_SEG_CS = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_SEG_CS;
static const ZydisShortString STR_PREF_SEG_DS = ZYDIS_MAKE_SHORTSTRING("ds ");
static const struct ZydisPredefinedTokenPREF_SEG_DS_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[9];
} TOK_DATA_PREF_SEG_DS = { 9, 7, { ZYDIS_TOKEN_PREFIX, 3, 'd', 's', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_SEG_DS = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_SEG_DS;
static const ZydisShortString STR_PREF_SEG_ES = ZYDIS_MAKE_SHORTSTRING("es ");
static const struct ZydisPredefinedTokenPREF_SEG_ES_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[9];
} TOK_DATA_PREF_SEG_ES = { 9, 7, { ZYDIS_TOKEN_PREFIX, 3, 'e', 's', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_SEG_ES = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_SEG_ES;
static const ZydisShortString STR_PREF_SEG_FS = ZYDIS_MAKE_SHORTSTRING("fs ");
static const struct ZydisPredefinedTokenPREF_SEG_FS_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[9];
} TOK_DATA_PREF_SEG_FS = { 9, 7, { ZYDIS_TOKEN_PREFIX, 3, 'f', 's', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_SEG_FS = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_SEG_FS;
static const ZydisShortString STR_PREF_SEG_GS = ZYDIS_MAKE_SHORTSTRING("gs ");
static const struct ZydisPredefinedTokenPREF_SEG_GS_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[9];
} TOK_DATA_PREF_SEG_GS = { 9, 7, { ZYDIS_TOKEN_PREFIX, 3, 'g', 's', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_SEG_GS = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_SEG_GS;
static const ZydisShortString STR_PREF_SEG_SS = ZYDIS_MAKE_SHORTSTRING("ss ");
static const struct ZydisPredefinedTokenPREF_SEG_SS_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[9];
} TOK_DATA_PREF_SEG_SS = { 9, 7, { ZYDIS_TOKEN_PREFIX, 3, 's', 's', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_SEG_SS = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_SEG_SS;
static const ZydisShortString STR_PREF_XACQUIRE = ZYDIS_MAKE_SHORTSTRING("xacquire ");
static const struct ZydisPredefinedTokenPREF_XACQUIRE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[15];
} TOK_DATA_PREF_XACQUIRE = { 15, 13, { ZYDIS_TOKEN_PREFIX, 9, 'x', 'a', 'c', 'q', 'u', 'i', 'r', 'e', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_XACQUIRE = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_XACQUIRE;
static const ZydisShortString STR_PREF_XRELEASE = ZYDIS_MAKE_SHORTSTRING("xrelease ");
static const struct ZydisPredefinedTokenPREF_XRELEASE_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[15];
} TOK_DATA_PREF_XRELEASE = { 15, 13, { ZYDIS_TOKEN_PREFIX, 9, 'x', 'r', 'e', 'l', 'e', 'a', 's', 'e', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_PREF_XRELEASE = (const ZydisPredefinedToken* const)&TOK_DATA_PREF_XRELEASE;
static const ZydisShortString STR_REGISTER = ZYDIS_MAKE_SHORTSTRING("%");
static const struct ZydisPredefinedTokenREGISTER_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_REGISTER = { 4, 2, { ZYDIS_TOKEN_REGISTER, 0, '%', '\0' } };
static const ZydisPredefinedToken* const TOK_REGISTER = (const ZydisPredefinedToken* const)&TOK_DATA_REGISTER;
static const ZydisShortString STR_SHORT = ZYDIS_MAKE_SHORTSTRING(" short");
static const ZydisShortString STR_SIZE_8_ATT = ZYDIS_MAKE_SHORTSTRING("b");
static const ZydisShortString STR_SIZE_8_INTEL = ZYDIS_MAKE_SHORTSTRING("byte ptr ");
static const struct ZydisPredefinedTokenSIZE_8_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[15];
} TOK_DATA_SIZE_8_INTEL = { 15, 13, { ZYDIS_TOKEN_TYPECAST, 9, 'b', 'y', 't', 'e', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_8_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_8_INTEL;
static const ZydisShortString STR_SIZE_16_ATT = ZYDIS_MAKE_SHORTSTRING("w");
static const ZydisShortString STR_SIZE_16_INTEL = ZYDIS_MAKE_SHORTSTRING("word ptr ");
static const struct ZydisPredefinedTokenSIZE_16_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[15];
} TOK_DATA_SIZE_16_INTEL = { 15, 13, { ZYDIS_TOKEN_TYPECAST, 9, 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_16_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_16_INTEL;
static const ZydisShortString STR_SIZE_32_ATT = ZYDIS_MAKE_SHORTSTRING("l");
static const ZydisShortString STR_SIZE_32_INTEL = ZYDIS_MAKE_SHORTSTRING("dword ptr ");
static const struct ZydisPredefinedTokenSIZE_32_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[16];
} TOK_DATA_SIZE_32_INTEL = { 16, 14, { ZYDIS_TOKEN_TYPECAST, 10, 'd', 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_32_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_32_INTEL;
static const ZydisShortString STR_SIZE_48 = ZYDIS_MAKE_SHORTSTRING("fword ptr ");
static const struct ZydisPredefinedTokenSIZE_48_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[16];
} TOK_DATA_SIZE_48 = { 16, 14, { ZYDIS_TOKEN_TYPECAST, 10, 'f', 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_48 = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_48;
static const ZydisShortString STR_SIZE_64_ATT = ZYDIS_MAKE_SHORTSTRING("q");
static const ZydisShortString STR_SIZE_64_INTEL = ZYDIS_MAKE_SHORTSTRING("qword ptr ");
static const struct ZydisPredefinedTokenSIZE_64_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[16];
} TOK_DATA_SIZE_64_INTEL = { 16, 14, { ZYDIS_TOKEN_TYPECAST, 10, 'q', 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_64_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_64_INTEL;
static const ZydisShortString STR_SIZE_80 = ZYDIS_MAKE_SHORTSTRING("tbyte ptr ");
static const struct ZydisPredefinedTokenSIZE_80_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[16];
} TOK_DATA_SIZE_80 = { 16, 14, { ZYDIS_TOKEN_TYPECAST, 10, 't', 'b', 'y', 't', 'e', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_80 = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_80;
static const ZydisShortString STR_SIZE_128_ATT = ZYDIS_MAKE_SHORTSTRING("x");
static const ZydisShortString STR_SIZE_128_INTEL = ZYDIS_MAKE_SHORTSTRING("xmmword ptr ");
static const struct ZydisPredefinedTokenSIZE_128_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[18];
} TOK_DATA_SIZE_128_INTEL = { 18, 16, { ZYDIS_TOKEN_TYPECAST, 12, 'x', 'm', 'm', 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_128_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_128_INTEL;
static const ZydisShortString STR_SIZE_256_ATT = ZYDIS_MAKE_SHORTSTRING("y");
static const ZydisShortString STR_SIZE_256_INTEL = ZYDIS_MAKE_SHORTSTRING("ymmword ptr ");
static const struct ZydisPredefinedTokenSIZE_256_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[18];
} TOK_DATA_SIZE_256_INTEL = { 18, 16, { ZYDIS_TOKEN_TYPECAST, 12, 'y', 'm', 'm', 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_256_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_256_INTEL;
static const ZydisShortString STR_SIZE_512_ATT = ZYDIS_MAKE_SHORTSTRING("z");
static const ZydisShortString STR_SIZE_512_INTEL = ZYDIS_MAKE_SHORTSTRING("zmmword ptr ");
static const struct ZydisPredefinedTokenSIZE_512_INTEL_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[18];
} TOK_DATA_SIZE_512_INTEL = { 18, 16, { ZYDIS_TOKEN_TYPECAST, 12, 'z', 'm', 'm', 'w', 'o', 'r', 'd', ' ', 'p', 't', 'r', '\0', ZYDIS_TOKEN_WHITESPACE, 0, ' ', '\0' } };
static const ZydisPredefinedToken* const TOK_SIZE_512_INTEL = (const ZydisPredefinedToken* const)&TOK_DATA_SIZE_512_INTEL;
static const ZydisShortString STR_SUB = ZYDIS_MAKE_SHORTSTRING("-");
static const struct ZydisPredefinedTokenSUB_
{
  ZyanU8 size;
  ZyanU8 next;
  ZyanU8 data[4];
} TOK_DATA_SUB = { 4, 2, { ZYDIS_TOKEN_DELIMITER, 0, '-', '\0' } };
static const ZydisPredefinedToken* const TOK_SUB = (const ZydisPredefinedToken* const)&TOK_DATA_SUB;
static const ZydisShortString STR_WHITESPACE = ZYDIS_MAKE_SHORTSTRING(" ");
#pragma pack(pop)
