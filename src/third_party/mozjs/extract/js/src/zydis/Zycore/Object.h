/***************************************************************************************************

  Zyan Core Library (Zycore-C)

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
 * Defines some generic object-related datatypes.
 */

#ifndef ZYCORE_OBJECT_H
#define ZYCORE_OBJECT_H

#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanMemberProcedure` function prototype.
 *
 * @param   object  A pointer to the object.
 */
typedef void (*ZyanMemberProcedure)(void* object);

/**
 * Defines the `ZyanConstMemberProcedure` function prototype.
 *
 * @param   object  A pointer to the object.
 */
typedef void (*ZyanConstMemberProcedure)(const void* object);

/**
 * Defines the `ZyanMemberFunction` function prototype.
 *
 * @param   object  A pointer to the object.
 *
 * @return  A zyan status code.
 */
typedef ZyanStatus (*ZyanMemberFunction)(void* object);

/**
 * Defines the `ZyanConstMemberFunction` function prototype.
 *
 * @param   object  A pointer to the object.
 *
 * @return  A zyan status code.
 */
typedef ZyanStatus (*ZyanConstMemberFunction)(const void* object);

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_OBJECT_H */
