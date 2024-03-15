/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Zyan Disassembler Library (Zydis)
 *
 * Original Author : Code taken from examples on zydis github README.md.
 *
 * Modified by Mozilla.
 *
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
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "zydis/ZydisAPI.h"

void zydisDisassemble(const uint8_t* code, size_t codeLen,
                      void(*println)(const char*)) {
  ZydisDecoder decoder;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

  ZydisFormatter formatter;
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_ATT);
  ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_TRUE);

  ZyanU64 runtime_address = 0;
  ZyanUSize offset = 0;
  const ZyanUSize length = (ZyanUSize)codeLen;
  ZydisDecodedInstruction instruction;
  ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
  char buffer[1024];
  while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset, length - offset,
                                             &instruction, operands)))
  {
#  define LIMIT 48
#  define LIMSTR "48"

    // We format the tag and the address and the bytes in a field of LIMIT
    // characters and start the menmonic at position LIMIT.  If the
    // tag+address+bytes would be too long we put the mnemonic + operands on the
    // next line.

    // Emit address
    sprintf(buffer, "%08" PRIX64 "  ", runtime_address);

    // Emit bytes
    for (size_t i = 0; i < instruction.length; i++) {
      sprintf(buffer+strlen(buffer), "%s%02x", i == 0 ? "" : " ", *(code + offset + i));
    }

    // Pad with at least one space
    sprintf(buffer+strlen(buffer), " ");

    // Pad out to the limit if necessary
    if (strlen(buffer) < LIMIT) {
      char* cur_end = buffer + strlen(buffer);
      size_t spaces = LIMIT - strlen(buffer);
      memset(cur_end, ' ', spaces);
      cur_end[spaces] = '\0';
    }

    // If too long then flush and provide an appropriate indent
    if (strlen(buffer) > LIMIT) {
      println(buffer);
      sprintf(buffer, "%-" LIMSTR "s", "");
    }

    // Emit instruction mnemonic + operands
    size_t used = strlen(buffer);
    ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                    instruction.operand_count_visible, buffer + used,
                                    sizeof(buffer) - used, runtime_address, ZYAN_NULL);
    println(buffer);

    offset += instruction.length;
    runtime_address += instruction.length;

#  undef LIMIT
#  undef LIMSTR
  }
}
