// Copyright 2014, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY ARM LIMITED AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL ARM LIMITED BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "js-config.h"

#ifdef JS_SIMULATOR_ARM64

#include "jit/arm64/vixl/Debugger-vixl.h"

#include "mozilla/Unused.h"
#include "mozilla/Vector.h"

#include "js/AllocPolicy.h"

namespace vixl {

// List of commands supported by the debugger.
#define DEBUG_COMMAND_LIST(C)  \
C(HelpCommand)                 \
C(ContinueCommand)             \
C(StepCommand)                 \
C(DisasmCommand)               \
C(PrintCommand)                \
C(ExamineCommand)

// Debugger command lines are broken up in token of different type to make
// processing easier later on.
class Token {
 public:
  virtual ~Token() {}

  // Token type.
  virtual bool IsRegister() const { return false; }
  virtual bool IsFPRegister() const { return false; }
  virtual bool IsIdentifier() const { return false; }
  virtual bool IsAddress() const { return false; }
  virtual bool IsInteger() const { return false; }
  virtual bool IsFormat() const { return false; }
  virtual bool IsUnknown() const { return false; }
  // Token properties.
  virtual bool CanAddressMemory() const { return false; }
  virtual uint8_t* ToAddress(Debugger* debugger) const = 0;
  virtual void Print(FILE* out = stdout) const = 0;

  static Token* Tokenize(const char* arg);
};

typedef mozilla::Vector<Token*, 0, js::SystemAllocPolicy> TokenVector;

// Tokens often hold one value.
template<typename T> class ValueToken : public Token {
 public:
  explicit ValueToken(T value) : value_(value) {}
  ValueToken() {}

  T value() const { return value_; }

  virtual uint8_t* ToAddress(Debugger* debugger) const override {
    USE(debugger);
    VIXL_ABORT();
  }

 protected:
  T value_;
};

// Integer registers (X or W) and their aliases.
// Format: wn or xn with 0 <= n < 32 or a name in the aliases list.
class RegisterToken : public ValueToken<const Register> {
 public:
  explicit RegisterToken(const Register reg)
      : ValueToken<const Register>(reg) {}

  virtual bool IsRegister() const override { return true; }
  virtual bool CanAddressMemory() const override { return value().Is64Bits(); }
  virtual uint8_t* ToAddress(Debugger* debugger) const override;
  virtual void Print(FILE* out = stdout) const override;
  const char* Name() const;

  static Token* Tokenize(const char* arg);
  static RegisterToken* Cast(Token* tok) {
    VIXL_ASSERT(tok->IsRegister());
    return reinterpret_cast<RegisterToken*>(tok);
  }

 private:
  static const int kMaxAliasNumber = 4;
  static const char* kXAliases[kNumberOfRegisters][kMaxAliasNumber];
  static const char* kWAliases[kNumberOfRegisters][kMaxAliasNumber];
};

// Floating point registers (D or S).
// Format: sn or dn with 0 <= n < 32.
class FPRegisterToken : public ValueToken<const FPRegister> {
 public:
  explicit FPRegisterToken(const FPRegister fpreg)
      : ValueToken<const FPRegister>(fpreg) {}

  virtual bool IsFPRegister() const override { return true; }
  virtual void Print(FILE* out = stdout) const override;

  static Token* Tokenize(const char* arg);
  static FPRegisterToken* Cast(Token* tok) {
    VIXL_ASSERT(tok->IsFPRegister());
    return reinterpret_cast<FPRegisterToken*>(tok);
  }
};


// Non-register identifiers.
// Format: Alphanumeric string starting with a letter.
class IdentifierToken : public ValueToken<char*> {
 public:
  explicit IdentifierToken(const char* name) {
    size_t size = strlen(name) + 1;
    value_ = (char*)js_malloc(size);
    strncpy(value_, name, size);
  }
  virtual ~IdentifierToken() { js_free(value_); }

  virtual bool IsIdentifier() const override { return true; }
  virtual bool CanAddressMemory() const override { return strcmp(value(), "pc") == 0; }
  virtual uint8_t* ToAddress(Debugger* debugger) const override;
  virtual void Print(FILE* out = stdout) const override;

  static Token* Tokenize(const char* arg);
  static IdentifierToken* Cast(Token* tok) {
    VIXL_ASSERT(tok->IsIdentifier());
    return reinterpret_cast<IdentifierToken*>(tok);
  }
};

// 64-bit address literal.
// Format: 0x... with up to 16 hexadecimal digits.
class AddressToken : public ValueToken<uint8_t*> {
 public:
  explicit AddressToken(uint8_t* address) : ValueToken<uint8_t*>(address) {}

  virtual bool IsAddress() const override { return true; }
  virtual bool CanAddressMemory() const override { return true; }
  virtual uint8_t* ToAddress(Debugger* debugger) const override;
  virtual void Print(FILE* out = stdout) const override;

  static Token* Tokenize(const char* arg);
  static AddressToken* Cast(Token* tok) {
    VIXL_ASSERT(tok->IsAddress());
    return reinterpret_cast<AddressToken*>(tok);
  }
};


// 64-bit decimal integer literal.
// Format: n.
class IntegerToken : public ValueToken<int64_t> {
 public:
  explicit IntegerToken(int64_t value) : ValueToken<int64_t>(value) {}

  virtual bool IsInteger() const override { return true; }
  virtual void Print(FILE* out = stdout) const override;

  static Token* Tokenize(const char* arg);
  static IntegerToken* Cast(Token* tok) {
    VIXL_ASSERT(tok->IsInteger());
    return reinterpret_cast<IntegerToken*>(tok);
  }
};

// Literal describing how to print a chunk of data (up to 64 bits).
// Format: .ln
// where l (letter) is one of
//  * x: hexadecimal
//  * s: signed integer
//  * u: unsigned integer
//  * f: floating point
//  * i: instruction
// and n (size) is one of 8, 16, 32 and 64. n should be omitted for
// instructions.
class FormatToken : public Token {
 public:
  FormatToken() {}

  virtual bool IsFormat() const override { return true; }
  virtual int SizeOf() const = 0;
  virtual char type_code() const = 0;
  virtual void PrintData(void* data, FILE* out = stdout) const = 0;
  virtual void Print(FILE* out = stdout) const override = 0;

  virtual uint8_t* ToAddress(Debugger* debugger) const override {
    USE(debugger);
    VIXL_ABORT();
  }

  static Token* Tokenize(const char* arg);
  static FormatToken* Cast(Token* tok) {
    VIXL_ASSERT(tok->IsFormat());
    return reinterpret_cast<FormatToken*>(tok);
  }
};


template<typename T> class Format : public FormatToken {
 public:
  Format(const char* fmt, char type_code) : fmt_(fmt), type_code_(type_code) {}

  virtual int SizeOf() const override { return sizeof(T); }
  virtual char type_code() const override { return type_code_; }
  virtual void PrintData(void* data, FILE* out = stdout) const override {
    T value;
    memcpy(&value, data, sizeof(value));
    fprintf(out, fmt_, value);
  }
  virtual void Print(FILE* out = stdout) const override;

 private:
  const char* fmt_;
  char type_code_;
};

// Tokens which don't fit any of the above.
class UnknownToken : public Token {
 public:
  explicit UnknownToken(const char* arg) {
    size_t size = strlen(arg) + 1;
    unknown_ = (char*)js_malloc(size);
    strncpy(unknown_, arg, size);
  }
  virtual ~UnknownToken() { js_free(unknown_); }
  virtual uint8_t* ToAddress(Debugger* debugger) const override {
    USE(debugger);
    VIXL_ABORT();
  }

  virtual bool IsUnknown() const override { return true; }
  virtual void Print(FILE* out = stdout) const override;

 private:
  char* unknown_;
};


// All debugger commands must subclass DebugCommand and implement Run, Print
// and Build. Commands must also define kHelp and kAliases.
class DebugCommand {
 public:
  explicit DebugCommand(Token* name) : name_(IdentifierToken::Cast(name)) {}
  DebugCommand() : name_(NULL) {}
  virtual ~DebugCommand() { js_delete(name_); }

  const char* name() { return name_->value(); }
  // Run the command on the given debugger. The command returns true if
  // execution should move to the next instruction.
  virtual bool Run(Debugger * debugger) = 0;
  virtual void Print(FILE* out = stdout);

  static bool Match(const char* name, const char** aliases);
  static DebugCommand* Parse(char* line);
  static void PrintHelp(const char** aliases,
                        const char* args,
                        const char* help);

 private:
  IdentifierToken* name_;
};

// For all commands below see their respective kHelp and kAliases in
// debugger-a64.cc
class HelpCommand : public DebugCommand {
 public:
  explicit HelpCommand(Token* name) : DebugCommand(name) {}

  virtual bool Run(Debugger* debugger) override;

  static DebugCommand* Build(TokenVector&& args);

  static const char* kHelp;
  static const char* kAliases[];
  static const char* kArguments;
};


class ContinueCommand : public DebugCommand {
 public:
  explicit ContinueCommand(Token* name) : DebugCommand(name) {}

  virtual bool Run(Debugger* debugger) override;

  static DebugCommand* Build(TokenVector&& args);

  static const char* kHelp;
  static const char* kAliases[];
  static const char* kArguments;
};


class StepCommand : public DebugCommand {
 public:
  StepCommand(Token* name, IntegerToken* count)
      : DebugCommand(name), count_(count) {}
  virtual ~StepCommand() { js_delete(count_); }

  int64_t count() { return count_->value(); }
  virtual bool Run(Debugger* debugger) override;
  virtual void Print(FILE* out = stdout) override;

  static DebugCommand* Build(TokenVector&& args);

  static const char* kHelp;
  static const char* kAliases[];
  static const char* kArguments;

 private:
  IntegerToken* count_;
};

class DisasmCommand : public DebugCommand {
 public:
  static DebugCommand* Build(TokenVector&& args);

  static const char* kHelp;
  static const char* kAliases[];
  static const char* kArguments;
};


class PrintCommand : public DebugCommand {
 public:
  PrintCommand(Token* name, Token* target, FormatToken* format)
      : DebugCommand(name), target_(target), format_(format) {}
  virtual ~PrintCommand() {
    js_delete(target_);
    js_delete(format_);
  }

  Token* target() { return target_; }
  FormatToken* format() { return format_; }
  virtual bool Run(Debugger* debugger) override;
  virtual void Print(FILE* out = stdout) override;

  static DebugCommand* Build(TokenVector&& args);

  static const char* kHelp;
  static const char* kAliases[];
  static const char* kArguments;

 private:
  Token* target_;
  FormatToken* format_;
};

class ExamineCommand : public DebugCommand {
 public:
  ExamineCommand(Token* name,
                 Token* target,
                 FormatToken* format,
                 IntegerToken* count)
      : DebugCommand(name), target_(target), format_(format), count_(count) {}
  virtual ~ExamineCommand() {
    js_delete(target_);
    js_delete(format_);
    js_delete(count_);
  }

  Token* target() { return target_; }
  FormatToken* format() { return format_; }
  IntegerToken* count() { return count_; }
  virtual bool Run(Debugger* debugger) override;
  virtual void Print(FILE* out = stdout) override;

  static DebugCommand* Build(TokenVector&& args);

  static const char* kHelp;
  static const char* kAliases[];
  static const char* kArguments;

 private:
  Token* target_;
  FormatToken* format_;
  IntegerToken* count_;
};

// Commands which name does not match any of the known commnand.
class UnknownCommand : public DebugCommand {
 public:
  explicit UnknownCommand(TokenVector&& args) : args_(Move(args)) {}
  virtual ~UnknownCommand();

  virtual bool Run(Debugger* debugger) override;

 private:
  TokenVector args_;
};

// Commands which name match a known command but the syntax is invalid.
class InvalidCommand : public DebugCommand {
 public:
  InvalidCommand(TokenVector&& args, int index, const char* cause)
      : args_(Move(args)), index_(index), cause_(cause) {}
  virtual ~InvalidCommand();

  virtual bool Run(Debugger* debugger) override;

 private:
  TokenVector args_;
  int index_;
  const char* cause_;
};

const char* HelpCommand::kAliases[] = { "help", NULL };
const char* HelpCommand::kArguments = NULL;
const char* HelpCommand::kHelp = "  Print this help.";

const char* ContinueCommand::kAliases[] = { "continue", "c", NULL };
const char* ContinueCommand::kArguments = NULL;
const char* ContinueCommand::kHelp = "  Resume execution.";

const char* StepCommand::kAliases[] = { "stepi", "si", NULL };
const char* StepCommand::kArguments = "[n = 1]";
const char* StepCommand::kHelp = "  Execute n next instruction(s).";

const char* DisasmCommand::kAliases[] = { "disasm", "di", NULL };
const char* DisasmCommand::kArguments = "[n = 10]";
const char* DisasmCommand::kHelp =
  "  Disassemble n instruction(s) at pc.\n"
  "  This command is equivalent to x pc.i [n = 10]."
;

const char* PrintCommand::kAliases[] = { "print", "p", NULL };
const char* PrintCommand::kArguments =  "<entity>[.format]";
const char* PrintCommand::kHelp =
  "  Print the given entity according to the given format.\n"
  "  The format parameter only affects individual registers; it is ignored\n"
  "  for other entities.\n"
  "  <entity> can be one of the following:\n"
  "   * A register name (such as x0, s1, ...).\n"
  "   * 'regs', to print all integer (W and X) registers.\n"
  "   * 'fpregs' to print all floating-point (S and D) registers.\n"
  "   * 'sysregs' to print all system registers (including NZCV).\n"
  "   * 'pc' to print the current program counter.\n"
;

const char* ExamineCommand::kAliases[] = { "m", "mem", "x", NULL };
const char* ExamineCommand::kArguments = "<addr>[.format] [n = 10]";
const char* ExamineCommand::kHelp =
  "  Examine memory. Print n items of memory at address <addr> according to\n"
  "  the given [.format].\n"
  "  Addr can be an immediate address, a register name or pc.\n"
  "  Format is made of a type letter: 'x' (hexadecimal), 's' (signed), 'u'\n"
  "  (unsigned), 'f' (floating point), i (instruction) and a size in bits\n"
  "  when appropriate (8, 16, 32, 64)\n"
  "  E.g 'x sp.x64' will print 10 64-bit words from the stack in\n"
  "  hexadecimal format."
;

const char* RegisterToken::kXAliases[kNumberOfRegisters][kMaxAliasNumber] = {
  { "x0", NULL },
  { "x1", NULL },
  { "x2", NULL },
  { "x3", NULL },
  { "x4", NULL },
  { "x5", NULL },
  { "x6", NULL },
  { "x7", NULL },
  { "x8", NULL },
  { "x9", NULL },
  { "x10", NULL },
  { "x11", NULL },
  { "x12", NULL },
  { "x13", NULL },
  { "x14", NULL },
  { "x15", NULL },
  { "ip0", "x16", NULL },
  { "ip1", "x17", NULL },
  { "x18", "pr", NULL },
  { "x19", NULL },
  { "x20", NULL },
  { "x21", NULL },
  { "x22", NULL },
  { "x23", NULL },
  { "x24", NULL },
  { "x25", NULL },
  { "x26", NULL },
  { "x27", NULL },
  { "x28", NULL },
  { "fp", "x29", NULL },
  { "lr", "x30", NULL },
  { "sp", NULL}
};

const char* RegisterToken::kWAliases[kNumberOfRegisters][kMaxAliasNumber] = {
  { "w0", NULL },
  { "w1", NULL },
  { "w2", NULL },
  { "w3", NULL },
  { "w4", NULL },
  { "w5", NULL },
  { "w6", NULL },
  { "w7", NULL },
  { "w8", NULL },
  { "w9", NULL },
  { "w10", NULL },
  { "w11", NULL },
  { "w12", NULL },
  { "w13", NULL },
  { "w14", NULL },
  { "w15", NULL },
  { "w16", NULL },
  { "w17", NULL },
  { "w18", NULL },
  { "w19", NULL },
  { "w20", NULL },
  { "w21", NULL },
  { "w22", NULL },
  { "w23", NULL },
  { "w24", NULL },
  { "w25", NULL },
  { "w26", NULL },
  { "w27", NULL },
  { "w28", NULL },
  { "w29", NULL },
  { "w30", NULL },
  { "wsp", NULL }
};


Debugger::Debugger(JSContext* cx, Decoder* decoder, FILE* stream)
    : Simulator(cx, decoder, stream),
      debug_parameters_(DBG_INACTIVE),
      pending_request_(false),
      steps_(0),
      last_command_(NULL) {
  disasm_ = js_new<PrintDisassembler>(stdout);
  printer_ = js_new<Decoder>();
  printer_->AppendVisitor(disasm_);
}


Debugger::~Debugger() {
  js_delete(disasm_);
  js_delete(printer_);
}


void Debugger::Run() {
  pc_modified_ = false;
  while (pc_ != kEndOfSimAddress) {
    if (pending_request()) RunDebuggerShell();
    ExecuteInstruction();
    LogAllWrittenRegisters();
  }
}


void Debugger::PrintInstructions(const void* address, int64_t count) {
  if (count == 0) {
    return;
  }

  const Instruction* from = Instruction::CastConst(address);
  if (count < 0) {
    count = -count;
    from -= (count - 1) * kInstructionSize;
  }
  const Instruction* to = from + count * kInstructionSize;

  for (const Instruction* current = from;
       current < to;
       current = current->NextInstruction()) {
    printer_->Decode(current);
  }
}


void Debugger::PrintMemory(const uint8_t* address,
                           const FormatToken* format,
                           int64_t count) {
  if (count == 0) {
    return;
  }

  const uint8_t* from = address;
  int size = format->SizeOf();
  if (count < 0) {
    count = -count;
    from -= (count - 1) * size;
  }
  const uint8_t* to = from + count * size;

  for (const uint8_t* current = from; current < to; current += size) {
    if (((current - from) % 8) == 0) {
      printf("\n%p: ", current);
    }

    uint64_t data = Memory::Read<uint64_t>(current);
    format->PrintData(&data);
    printf(" ");
  }
  printf("\n\n");
}


void Debugger::PrintRegister(const Register& target_reg,
                             const char* name,
                             const FormatToken* format) {
  const uint64_t reg_size = target_reg.size();
  const uint64_t format_size = format->SizeOf() * 8;
  const uint64_t count = reg_size / format_size;
  const uint64_t mask = 0xffffffffffffffff >> (64 - format_size);
  const uint64_t reg_value = reg<uint64_t>(target_reg.code(),
                                           Reg31IsStackPointer);
  VIXL_ASSERT(count > 0);

  printf("%s = ", name);
  for (uint64_t i = 1; i <= count; i++) {
    uint64_t data = reg_value >> (reg_size - (i * format_size));
    data &= mask;
    format->PrintData(&data);
    printf(" ");
  }
  printf("\n");
}


// TODO(all): fix this for vector registers.
void Debugger::PrintFPRegister(const FPRegister& target_fpreg,
                               const FormatToken* format) {
  const unsigned fpreg_size = target_fpreg.size();
  const uint64_t format_size = format->SizeOf() * 8;
  const uint64_t count = fpreg_size / format_size;
  const uint64_t mask = 0xffffffffffffffff >> (64 - format_size);
  const uint64_t fpreg_value = vreg<uint64_t>(fpreg_size, target_fpreg.code());
  VIXL_ASSERT(count > 0);

  if (target_fpreg.Is32Bits()) {
    printf("s%u = ", target_fpreg.code());
  } else {
    printf("d%u = ", target_fpreg.code());
  }
  for (uint64_t i = 1; i <= count; i++) {
    uint64_t data = fpreg_value >> (fpreg_size - (i * format_size));
    data &= mask;
    format->PrintData(&data);
    printf(" ");
  }
  printf("\n");
}


void Debugger::VisitException(const Instruction* instr) {
  switch (instr->Mask(ExceptionMask)) {
    case BRK:
      DoBreakpoint(instr);
      return;
    case HLT:
      VIXL_FALLTHROUGH();
    default: Simulator::VisitException(instr);
  }
}


// Read a command. A command will be at most kMaxDebugShellLine char long and
// ends with '\n\0'.
// TODO: Should this be a utility function?
char* Debugger::ReadCommandLine(const char* prompt, char* buffer, int length) {
  int fgets_calls = 0;
  char* end = NULL;

  printf("%s", prompt);
  fflush(stdout);

  do {
    if (fgets(buffer, length, stdin) == NULL) {
      printf(" ** Error while reading command. **\n");
      return NULL;
    }

    fgets_calls++;
    end = strchr(buffer, '\n');
  } while (end == NULL);

  if (fgets_calls != 1) {
    printf(" ** Command too long. **\n");
    return NULL;
  }

  // Remove the newline from the end of the command.
  VIXL_ASSERT(end[1] == '\0');
  VIXL_ASSERT((end - buffer) < (length - 1));
  end[0] = '\0';

  return buffer;
}


void Debugger::RunDebuggerShell() {
  if (IsDebuggerRunning()) {
    if (steps_ > 0) {
      // Finish stepping first.
      --steps_;
      return;
    }

    printf("Next: ");
    PrintInstructions(pc());
    bool done = false;
    while (!done) {
      char buffer[kMaxDebugShellLine];
      char* line = ReadCommandLine("vixl> ", buffer, kMaxDebugShellLine);

      if (line == NULL) continue;  // An error occurred.

      DebugCommand* command = DebugCommand::Parse(line);
      if (command != NULL) {
        last_command_ = command;
      }

      if (last_command_ != NULL) {
        done = last_command_->Run(this);
      } else {
        printf("No previous command to run!\n");
      }
    }

    if ((debug_parameters_ & DBG_BREAK) != 0) {
      // The break request has now been handled, move to next instruction.
      debug_parameters_ &= ~DBG_BREAK;
      increment_pc();
    }
  }
}


void Debugger::DoBreakpoint(const Instruction* instr) {
  VIXL_ASSERT(instr->Mask(ExceptionMask) == BRK);

  printf("Hit breakpoint at pc=%p.\n", reinterpret_cast<const void*>(instr));
  set_debug_parameters(debug_parameters() | DBG_BREAK | DBG_ACTIVE);
  // Make the shell point to the brk instruction.
  set_pc(instr);
}


static bool StringToUInt64(uint64_t* value, const char* line, int base = 10) {
  char* endptr = NULL;
  errno = 0;  // Reset errors.
  uint64_t parsed = strtoul(line, &endptr, base);

  if (errno == ERANGE) {
    // Overflow.
    return false;
  }

  if (endptr == line) {
    // No digits were parsed.
    return false;
  }

  if (*endptr != '\0') {
    // Non-digit characters present at the end.
    return false;
  }

  *value = parsed;
  return true;
}


static bool StringToInt64(int64_t* value, const char* line, int base = 10) {
  char* endptr = NULL;
  errno = 0;  // Reset errors.
  int64_t parsed = strtol(line, &endptr, base);

  if (errno == ERANGE) {
    // Overflow, undeflow.
    return false;
  }

  if (endptr == line) {
    // No digits were parsed.
    return false;
  }

  if (*endptr != '\0') {
    // Non-digit characters present at the end.
    return false;
  }

  *value = parsed;
  return true;
}


Token* Token::Tokenize(const char* arg) {
  if ((arg == NULL) || (*arg == '\0')) {
    return NULL;
  }

  // The order is important. For example Identifier::Tokenize would consider
  // any register to be a valid identifier.

  Token* token = RegisterToken::Tokenize(arg);
  if (token != NULL) {
    return token;
  }

  token = FPRegisterToken::Tokenize(arg);
  if (token != NULL) {
    return token;
  }

  token = IdentifierToken::Tokenize(arg);
  if (token != NULL) {
    return token;
  }

  token = AddressToken::Tokenize(arg);
  if (token != NULL) {
    return token;
  }

  token = IntegerToken::Tokenize(arg);
  if (token != NULL) {
    return token;
  }

  return js_new<UnknownToken>(arg);
}


uint8_t* RegisterToken::ToAddress(Debugger* debugger) const {
  VIXL_ASSERT(CanAddressMemory());
  uint64_t reg_value = debugger->xreg(value().code(), Reg31IsStackPointer);
  uint8_t* address = NULL;
  memcpy(&address, &reg_value, sizeof(address));
  return address;
}


void RegisterToken::Print(FILE* out) const {
  VIXL_ASSERT(value().IsValid());
  fprintf(out, "[Register %s]", Name());
}


const char* RegisterToken::Name() const {
  if (value().Is32Bits()) {
    return kWAliases[value().code()][0];
  } else {
    return kXAliases[value().code()][0];
  }
}


Token* RegisterToken::Tokenize(const char* arg) {
  for (unsigned i = 0; i < kNumberOfRegisters; i++) {
    // Is it a X register or alias?
    for (const char** current = kXAliases[i]; *current != NULL; current++) {
      if (strcmp(arg, *current) == 0) {
        return js_new<RegisterToken>(Register::XRegFromCode(i));
      }
    }

    // Is it a W register or alias?
    for (const char** current = kWAliases[i]; *current != NULL; current++) {
      if (strcmp(arg, *current) == 0) {
        return js_new<RegisterToken>(Register::WRegFromCode(i));
      }
    }
  }

  return NULL;
}


void FPRegisterToken::Print(FILE* out) const {
  VIXL_ASSERT(value().IsValid());
  char prefix = value().Is32Bits() ? 's' : 'd';
  fprintf(out, "[FPRegister %c%" PRIu32 "]", prefix, value().code());
}


Token* FPRegisterToken::Tokenize(const char* arg) {
  if (strlen(arg) < 2) {
    return NULL;
  }

  switch (*arg) {
    case 's':
    case 'd':
      const char* cursor = arg + 1;
      uint64_t code = 0;
      if (!StringToUInt64(&code, cursor)) {
        return NULL;
      }

      if (code > kNumberOfFPRegisters) {
        return NULL;
      }

      VRegister fpreg = NoVReg;
      switch (*arg) {
        case 's':
          fpreg = VRegister::SRegFromCode(static_cast<unsigned>(code));
          break;
        case 'd':
          fpreg = VRegister::DRegFromCode(static_cast<unsigned>(code));
          break;
        default: VIXL_UNREACHABLE();
      }

      return js_new<FPRegisterToken>(fpreg);
  }

  return NULL;
}


uint8_t* IdentifierToken::ToAddress(Debugger* debugger) const {
  VIXL_ASSERT(CanAddressMemory());
  const Instruction* pc_value = debugger->pc();
  uint8_t* address = NULL;
  memcpy(&address, &pc_value, sizeof(address));
  return address;
}

void IdentifierToken::Print(FILE* out) const {
  fprintf(out, "[Identifier %s]", value());
}


Token* IdentifierToken::Tokenize(const char* arg) {
  if (!isalpha(arg[0])) {
    return NULL;
  }

  const char* cursor = arg + 1;
  while ((*cursor != '\0') && isalnum(*cursor)) {
    ++cursor;
  }

  if (*cursor == '\0') {
    return js_new<IdentifierToken>(arg);
  }

  return NULL;
}


uint8_t* AddressToken::ToAddress(Debugger* debugger) const {
  USE(debugger);
  return value();
}


void AddressToken::Print(FILE* out) const {
  fprintf(out, "[Address %p]", value());
}


Token* AddressToken::Tokenize(const char* arg) {
  if ((strlen(arg) < 3) || (arg[0] != '0') || (arg[1] != 'x')) {
    return NULL;
  }

  uint64_t ptr = 0;
  if (!StringToUInt64(&ptr, arg, 16)) {
    return NULL;
  }

  uint8_t* address = reinterpret_cast<uint8_t*>(ptr);
  return js_new<AddressToken>(address);
}


void IntegerToken::Print(FILE* out) const {
  fprintf(out, "[Integer %" PRId64 "]", value());
}


Token* IntegerToken::Tokenize(const char* arg) {
  int64_t value = 0;
  if (!StringToInt64(&value, arg)) {
    return NULL;
  }

  return js_new<IntegerToken>(value);
}


Token* FormatToken::Tokenize(const char* arg) {
  size_t length = strlen(arg);
  switch (arg[0]) {
    case 'x':
    case 's':
    case 'u':
    case 'f':
      if (length == 1) return NULL;
      break;
    case 'i':
      if (length == 1) return js_new<Format<uint32_t>>("%08" PRIx32, 'i');
      VIXL_FALLTHROUGH();
    default: return NULL;
  }

  char* endptr = NULL;
  errno = 0;  // Reset errors.
  uint64_t count = strtoul(arg + 1, &endptr, 10);

  if (errno != 0) {
    // Overflow, etc.
    return NULL;
  }

  if (endptr == arg) {
    // No digits were parsed.
    return NULL;
  }

  if (*endptr != '\0') {
    // There are unexpected (non-digit) characters after the number.
    return NULL;
  }

  switch (arg[0]) {
    case 'x':
      switch (count) {
        case 8: return js_new<Format<uint8_t>>("%02" PRIx8, 'x');
        case 16: return js_new<Format<uint16_t>>("%04" PRIx16, 'x');
        case 32: return js_new<Format<uint32_t>>("%08" PRIx32, 'x');
        case 64: return js_new<Format<uint64_t>>("%016" PRIx64, 'x');
        default: return NULL;
      }
    case 's':
      switch (count) {
        case 8: return js_new<Format<int8_t>>("%4" PRId8, 's');
        case 16: return js_new<Format<int16_t>>("%6" PRId16, 's');
        case 32: return js_new<Format<int32_t>>("%11" PRId32, 's');
        case 64: return js_new<Format<int64_t>>("%20" PRId64, 's');
        default: return NULL;
      }
    case 'u':
      switch (count) {
        case 8: return js_new<Format<uint8_t>>("%3" PRIu8, 'u');
        case 16: return js_new<Format<uint16_t>>("%5" PRIu16, 'u');
        case 32: return js_new<Format<uint32_t>>("%10" PRIu32, 'u');
        case 64: return js_new<Format<uint64_t>>("%20" PRIu64, 'u');
        default: return NULL;
      }
    case 'f':
      switch (count) {
        case 32: return js_new<Format<float>>("%13g", 'f');
        case 64: return js_new<Format<double>>("%13g", 'f');
        default: return NULL;
      }
    default:
      VIXL_UNREACHABLE();
      return NULL;
  }
}


template<typename T>
void Format<T>::Print(FILE* out) const {
  unsigned size = sizeof(T) * 8;
  fprintf(out, "[Format %c%u - %s]", type_code_, size, fmt_);
}


void UnknownToken::Print(FILE* out) const {
  fprintf(out, "[Unknown %s]", unknown_);
}


void DebugCommand::Print(FILE* out) {
  fprintf(out, "%s", name());
}


bool DebugCommand::Match(const char* name, const char** aliases) {
  for (const char** current = aliases; *current != NULL; current++) {
    if (strcmp(name, *current) == 0) {
       return true;
    }
  }

  return false;
}


DebugCommand* DebugCommand::Parse(char* line) {
  using mozilla::Unused;
  TokenVector args;

  for (char* chunk = strtok(line, " \t");
       chunk != NULL;
       chunk = strtok(NULL, " \t")) {
    char* dot = strchr(chunk, '.');
    if (dot != NULL) {
      // 'Token.format'.
      Token* format = FormatToken::Tokenize(dot + 1);
      if (format != NULL) {
        *dot = '\0';
        Unused << args.append(Token::Tokenize(chunk));
        Unused << args.append(format);
      } else {
        // Error while parsing the format, push the UnknownToken so an error
        // can be accurately reported.
        Unused << args.append(Token::Tokenize(chunk));
      }
    } else {
      Unused << args.append(Token::Tokenize(chunk));
    }
  }

  if (args.empty()) {
    return NULL;
  }

  if (!args[0]->IsIdentifier()) {
    return js_new<InvalidCommand>(Move(args), 0, "command name is not valid");
  }

  const char* name = IdentifierToken::Cast(args[0])->value();
  #define RETURN_IF_MATCH(Command)       \
  if (Match(name, Command::kAliases)) {  \
    return Command::Build(Move(args));   \
  }
  DEBUG_COMMAND_LIST(RETURN_IF_MATCH);
  #undef RETURN_IF_MATCH

  return js_new<UnknownCommand>(Move(args));
}


void DebugCommand::PrintHelp(const char** aliases,
                             const char* args,
                             const char* help) {
  VIXL_ASSERT(aliases[0] != NULL);
  VIXL_ASSERT(help != NULL);

  printf("\n----\n\n");
  for (const char** current = aliases; *current != NULL; current++) {
    if (args != NULL) {
      printf("%s %s\n", *current, args);
    } else {
      printf("%s\n", *current);
    }
  }
  printf("\n%s\n", help);
}


bool HelpCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());
  USE(debugger);

  #define PRINT_HELP(Command)                     \
    DebugCommand::PrintHelp(Command::kAliases,    \
                            Command::kArguments,  \
                            Command::kHelp);
  DEBUG_COMMAND_LIST(PRINT_HELP);
  #undef PRINT_HELP
  printf("\n----\n\n");

  return false;
}


DebugCommand* HelpCommand::Build(TokenVector&& args) {
  if (args.length() != 1) {
    return js_new<InvalidCommand>(Move(args), -1, "too many arguments");
  }

  return js_new<HelpCommand>(args[0]);
}


bool ContinueCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());

  debugger->set_debug_parameters(debugger->debug_parameters() & ~DBG_ACTIVE);
  return true;
}


DebugCommand* ContinueCommand::Build(TokenVector&& args) {
  if (args.length() != 1) {
    return js_new<InvalidCommand>(Move(args), -1, "too many arguments");
  }

  return js_new<ContinueCommand>(args[0]);
}


bool StepCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());

  int64_t steps = count();
  if (steps < 0) {
    printf(" ** invalid value for steps: %" PRId64 " (<0) **\n", steps);
  } else if (steps > 1) {
    debugger->set_steps(steps - 1);
  }

  return true;
}


void StepCommand::Print(FILE* out) {
  fprintf(out, "%s %" PRId64 "", name(), count());
}


DebugCommand* StepCommand::Build(TokenVector&& args) {
  IntegerToken* count = NULL;
  switch (args.length()) {
    case 1: {  // step [1]
      count = js_new<IntegerToken>(1);
      break;
    }
    case 2: {  // step n
      Token* first = args[1];
      if (!first->IsInteger()) {
        return js_new<InvalidCommand>(Move(args), 1, "expects int");
      }
      count = IntegerToken::Cast(first);
      break;
    }
    default:
      return js_new<InvalidCommand>(Move(args), -1, "too many arguments");
  }

  return js_new<StepCommand>(args[0], count);
}


DebugCommand* DisasmCommand::Build(TokenVector&& args) {
  IntegerToken* count = NULL;
  switch (args.length()) {
    case 1: {  // disasm [10]
      count = js_new<IntegerToken>(10);
      break;
    }
    case 2: {  // disasm n
      Token* first = args[1];
      if (!first->IsInteger()) {
        return js_new<InvalidCommand>(Move(args), 1, "expects int");
      }

      count = IntegerToken::Cast(first);
      break;
    }
    default:
      return js_new<InvalidCommand>(Move(args), -1, "too many arguments");
  }

  Token* target = js_new<IdentifierToken>("pc");
  FormatToken* format = js_new<Format<uint32_t>>("%08" PRIx32, 'i');
  return js_new<ExamineCommand>(args[0], target, format, count);
}


void PrintCommand::Print(FILE* out) {
  fprintf(out, "%s ", name());
  target()->Print(out);
  if (format() != NULL) format()->Print(out);
}


bool PrintCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());

  Token* tok = target();
  if (tok->IsIdentifier()) {
    char* identifier = IdentifierToken::Cast(tok)->value();
    if (strcmp(identifier, "regs") == 0) {
      debugger->PrintRegisters();
    } else if (strcmp(identifier, "fpregs") == 0) {
      debugger->PrintVRegisters();
    } else if (strcmp(identifier, "sysregs") == 0) {
      debugger->PrintSystemRegisters();
    } else if (strcmp(identifier, "pc") == 0) {
      printf("pc = %16p\n", reinterpret_cast<const void*>(debugger->pc()));
    } else {
      printf(" ** Unknown identifier to print: %s **\n", identifier);
    }

    return false;
  }

  FormatToken* format_tok = format();
  VIXL_ASSERT(format_tok != NULL);
  if (format_tok->type_code() == 'i') {
    // TODO(all): Add support for instruction disassembly.
    printf(" ** unsupported format: instructions **\n");
    return false;
  }

  if (tok->IsRegister()) {
    RegisterToken* reg_tok = RegisterToken::Cast(tok);
    Register reg = reg_tok->value();
    debugger->PrintRegister(reg, reg_tok->Name(), format_tok);
    return false;
  }

  if (tok->IsFPRegister()) {
    FPRegister fpreg = FPRegisterToken::Cast(tok)->value();
    debugger->PrintFPRegister(fpreg, format_tok);
    return false;
  }

  VIXL_UNREACHABLE();
  return false;
}


DebugCommand* PrintCommand::Build(TokenVector&& args) {
  if (args.length() < 2) {
    return js_new<InvalidCommand>(Move(args), -1, "too few arguments");
  }

  Token* target = args[1];
  if (!target->IsRegister() &&
      !target->IsFPRegister() &&
      !target->IsIdentifier()) {
    return js_new<InvalidCommand>(Move(args), 1, "expects reg or identifier");
  }

  FormatToken* format = NULL;
  int target_size = 0;
  if (target->IsRegister()) {
    Register reg = RegisterToken::Cast(target)->value();
    target_size = reg.SizeInBytes();
  } else if (target->IsFPRegister()) {
    FPRegister fpreg = FPRegisterToken::Cast(target)->value();
    target_size = fpreg.SizeInBytes();
  }
  // If the target is an identifier there must be no format. This is checked
  // in the switch statement below.

  switch (args.length()) {
    case 2: {
      if (target->IsRegister()) {
        switch (target_size) {
          case 4: format = js_new<Format<uint32_t>>("%08" PRIx32, 'x'); break;
          case 8: format = js_new<Format<uint64_t>>("%016" PRIx64, 'x'); break;
          default: VIXL_UNREACHABLE();
        }
      } else if (target->IsFPRegister()) {
        switch (target_size) {
          case 4: format = js_new<Format<float>>("%8g", 'f'); break;
          case 8: format = js_new<Format<double>>("%8g", 'f'); break;
          default: VIXL_UNREACHABLE();
        }
      }
      break;
    }
    case 3: {
      if (target->IsIdentifier()) {
        return js_new<InvalidCommand>(Move(args), 2,
            "format is only allowed with registers");
      }

      Token* second = args[2];
      if (!second->IsFormat()) {
        return js_new<InvalidCommand>(Move(args), 2, "expects format");
      }
      format = FormatToken::Cast(second);

      if (format->SizeOf() > target_size) {
        return js_new<InvalidCommand>(Move(args), 2, "format too wide");
      }

      break;
    }
    default:
      return js_new<InvalidCommand>(Move(args), -1, "too many arguments");
  }

  return js_new<PrintCommand>(args[0], target, format);
}


bool ExamineCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());

  uint8_t* address = target()->ToAddress(debugger);
  int64_t  amount = count()->value();
  if (format()->type_code() == 'i') {
    debugger->PrintInstructions(address, amount);
  } else {
    debugger->PrintMemory(address, format(), amount);
  }

  return false;
}


void ExamineCommand::Print(FILE* out) {
  fprintf(out, "%s ", name());
  format()->Print(out);
  target()->Print(out);
}


DebugCommand* ExamineCommand::Build(TokenVector&& args) {
  if (args.length() < 2) {
    return js_new<InvalidCommand>(Move(args), -1, "too few arguments");
  }

  Token* target = args[1];
  if (!target->CanAddressMemory()) {
    return js_new<InvalidCommand>(Move(args), 1, "expects address");
  }

  FormatToken* format = NULL;
  IntegerToken* count = NULL;

  switch (args.length()) {
    case 2: {  // mem addr[.x64] [10]
      format = js_new<Format<uint64_t>>("%016" PRIx64, 'x');
      count = js_new<IntegerToken>(10);
      break;
    }
    case 3: {  // mem addr.format [10]
               // mem addr[.x64] n
      Token* second = args[2];
      if (second->IsFormat()) {
        format = FormatToken::Cast(second);
        count = js_new<IntegerToken>(10);
        break;
      } else if (second->IsInteger()) {
        format = js_new<Format<uint64_t>>("%016" PRIx64, 'x');
        count = IntegerToken::Cast(second);
      } else {
        return js_new<InvalidCommand>(Move(args), 2, "expects format or integer");
      }
      VIXL_UNREACHABLE();
      break;
    }
    case 4: {  // mem addr.format n
      Token* second = args[2];
      Token* third = args[3];
      if (!second->IsFormat() || !third->IsInteger()) {
        return js_new<InvalidCommand>(Move(args), -1, "expects addr[.format] [n]");
      }
      format = FormatToken::Cast(second);
      count = IntegerToken::Cast(third);
      break;
    }
    default:
      return js_new<InvalidCommand>(Move(args), -1, "too many arguments");
  }

  return js_new<ExamineCommand>(args[0], target, format, count);
}


UnknownCommand::~UnknownCommand() {
  const size_t size = args_.length();
  for (size_t i = 0; i < size; ++i) {
    js_delete(args_[i]);
  }
}


bool UnknownCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());
  USE(debugger);

  printf(" ** Unknown Command:");
  const size_t size = args_.length();
  for (size_t i = 0; i < size; ++i) {
    printf(" ");
    args_[i]->Print(stdout);
  }
  printf(" **\n");

  return false;
}


InvalidCommand::~InvalidCommand() {
  const size_t size = args_.length();
  for (size_t i = 0; i < size; ++i) {
    js_delete(args_[i]);
  }
}


bool InvalidCommand::Run(Debugger* debugger) {
  VIXL_ASSERT(debugger->IsDebuggerRunning());
  USE(debugger);

  printf(" ** Invalid Command:");
  const size_t size = args_.length();
  for (size_t i = 0; i < size; ++i) {
    printf(" ");
    if (i == static_cast<size_t>(index_)) {
      printf(">>");
      args_[i]->Print(stdout);
      printf("<<");
    } else {
      args_[i]->Print(stdout);
    }
  }
  printf(" **\n");
  printf(" ** %s\n", cause_);

  return false;
}

}  // namespace vixl

#endif  // JS_SIMULATOR_ARM64
