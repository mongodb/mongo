/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * @brief   Implements the `ZydisFormatterToken` type and provides functions to use it.
 */

#ifndef ZYDIS_FORMATTER_TOKEN_H
#define ZYDIS_FORMATTER_TOKEN_H

#include "zydis/ZydisExportConfig.h"
#include "zydis/Zycore/String.h"
#include "zydis/Zycore/Types.h"
#include "zydis/Zydis/Status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Token types                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @biref   Defines the `ZydisTokenType` data-type.
 */
typedef ZyanU8 ZydisTokenType;

#define ZYDIS_TOKEN_INVALID             0x00
/**
 * @brief   A whitespace character.
 */
#define ZYDIS_TOKEN_WHITESPACE          0x01
/**
 * @brief   A delimiter character (like `','`, `':'`, `'+'`, `'-'`, `'*'`).
 */
#define ZYDIS_TOKEN_DELIMITER           0x02
/**
 * @brief   An opening parenthesis character (like `'('`, `'['`, `'{'`).
 */
#define ZYDIS_TOKEN_PARENTHESIS_OPEN    0x03
/**
 * @brief   A closing parenthesis character (like `')'`, `']'`, `'}'`).
 */
#define ZYDIS_TOKEN_PARENTHESIS_CLOSE   0x04
/**
 * @brief   A prefix literal (like `"LOCK"`, `"REP"`).
 */
#define ZYDIS_TOKEN_PREFIX              0x05
/**
 * @brief   A mnemonic literal (like `"MOV"`, `"VCMPPSD"`, `"LCALL"`).
 */
#define ZYDIS_TOKEN_MNEMONIC            0x06
/**
 * @brief   A register literal (like `"RAX"`, `"DS"`, `"%ECX"`).
 */
#define ZYDIS_TOKEN_REGISTER            0x07
/**
 * @brief   An absolute address literal (like `0x00400000`).
 */
#define ZYDIS_TOKEN_ADDRESS_ABS         0x08
/**
 * @brief   A relative address literal (like `-0x100`).
 */
#define ZYDIS_TOKEN_ADDRESS_REL         0x09
/**
 * @brief   A displacement literal (like `0xFFFFFFFF`, `-0x100`, `+0x1234`).
 */
#define ZYDIS_TOKEN_DISPLACEMENT        0x0A
/**
 * @brief   An immediate literal (like `0xC0`, `-0x1234`, `$0x0000`).
 */
#define ZYDIS_TOKEN_IMMEDIATE           0x0B
/**
 * @brief   A typecast literal (like `DWORD PTR`).
 */
#define ZYDIS_TOKEN_TYPECAST            0x0C
/**
 * @brief   A decorator literal (like `"Z"`, `"1TO4"`).
 */
#define ZYDIS_TOKEN_DECORATOR           0x0D
/**
 * @brief   A symbol literal.
 */
#define ZYDIS_TOKEN_SYMBOL              0x0E

/**
 * @brief   The base for user-defined token types.
 */
#define ZYDIS_TOKEN_USER                0x80

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Token                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

#pragma pack(push, 1)

/**
 * @brief   Defines the `ZydisFormatterToken` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZydisFormatterToken_
{
    /**
     * @brief   The token type.
     */
    ZydisTokenType type;
    /**
     * @brief   An offset to the next token, or `0`.
     */
    ZyanU8 next;
} ZydisFormatterToken;

#pragma pack(pop)

/**
 * @brief   Defines the `ZydisFormatterTokenConst` data-type.
 */
typedef const ZydisFormatterToken ZydisFormatterTokenConst;

/* ---------------------------------------------------------------------------------------------- */
/* Buffer                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisFormatterBuffer` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZydisFormatterBuffer_
{
    /**
     * @brief   `ZYAN_TRUE`, if the buffer contains a token stream or `ZYAN_FALSE, if it contains
     *          a simple string.
     */
    ZyanBool is_token_list;
    /**
     * @brief   The remaining capacity of the buffer.
     */
    ZyanUSize capacity;
    /**
     * @brief   The `ZyanString` instance that refers to the literal value of the most recently
     *          added token.
     */
    ZyanString string;
} ZydisFormatterBuffer;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Token                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns the `type` and the string `value` of the given `token`.
 *
 * @param   token   A pointer to the `ZydisFormatterToken` struct.
 * @param   type    Receives the token type.
 * @param   value   Receives a pointer to the string value of the token.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterTokenGetValue(const ZydisFormatterToken* token,
    ZydisTokenType* type, ZyanConstCharPointer* value);

/**
 * @brief   Obtains the next `token` linked to the passed one.
 *
 * @param   token   Receives a pointer to the next `ZydisFormatterToken` struct linked to the
 *                  passed one.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterTokenNext(ZydisFormatterTokenConst** token);

/* ---------------------------------------------------------------------------------------------- */
/* Buffer                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns the current (most recently added) token.
 *
 * @param   buffer  A pointer to the `ZydisFormatterBuffer` struct.
 * @param   token   Receives a pointer to the current token.
 *
 * @return  A zyan status code.
 *
 * This function returns `ZYAN_STATUS_INVALID_OPERATION`, if the buffer does not contain at least
 * one token.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterBufferGetToken(const ZydisFormatterBuffer* buffer,
    ZydisFormatterTokenConst** token);

/**
 * @brief   Returns the `ZyanString` instance associated with the given buffer.
 *
 * @param   buffer  A pointer to the `ZydisFormatterBuffer` struct.
 * @param   string  Receives a pointer to the `ZyanString` instance associated with the given
 *                  buffer.
 *
 * @return  A zyan status code.
 *
 * This function returns `ZYAN_STATUS_INVALID_OPERATION`, if the buffer does not contain at least
 * one token.
 *
 * The returned string always refers to the literal value of the current (most recently added)
 * token and will remain valid until the buffer is destroyed.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterBufferGetString(ZydisFormatterBuffer* buffer,
    ZyanString** string);

/**
 * @brief   Appends a new token to the `buffer`.
 *
 * @param   buffer  A pointer to the `ZydisFormatterBuffer` struct.
 * @param   type    The type of the new token.
 *
 * @return  A zyan status code.
 *
 * Note that the `ZyanString` instance returned by `ZydisFormatterBufferGetString` will
 * automatically be updated by calling this function.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterBufferAppend(ZydisFormatterBuffer* buffer,
    ZydisTokenType type);

/**
 * @brief   Returns a snapshot of the buffer-state.
 *
 * @param   buffer  A pointer to the `ZydisFormatterBuffer` struct.
 * @param   state   Receives a snapshot of the buffer-state.
 *
 * @return  A zyan status code.
 *
 * Note that the buffer-state is saved inside the buffer itself and thus becomes invalid as soon
 * as the buffer gets overwritten or destroyed.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterBufferRemember(const ZydisFormatterBuffer* buffer,
    ZyanUPointer* state);

/**
 * @brief   Restores a previously saved buffer-state.
 *
 * @param   buffer  A pointer to the `ZydisFormatterBuffer` struct.
 * @param   state   The buffer-state to restore.
 *
 * @return  A zyan status code.
 *
 * All tokens added after obtaining the given `state` snapshot will be removed. This function
 * does NOT restore any string content.
 *
 * Note that the `ZyanString` instance returned by `ZydisFormatterBufferGetString` will
 * automatically be updated by calling this function.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterBufferRestore(ZydisFormatterBuffer* buffer,
    ZyanUPointer state);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_FORMATTER_TOKEN_H */
