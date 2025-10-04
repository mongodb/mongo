#ifndef VIXL_A64_MOZ_CACHING_DECODER_A64_H_
#define VIXL_A64_MOZ_CACHING_DECODER_A64_H_

#include "mozilla/HashTable.h"

#include "jit/arm64/vixl/Decoder-vixl.h"
#include "js/AllocPolicy.h"

#ifdef DEBUG
#define JS_CACHE_SIMULATOR_ARM64 1
#endif

#ifdef JS_CACHE_SIMULATOR_ARM64
namespace vixl {

// This enumeration list the different kind of instructions which can be
// decoded. These kind correspond to the set of visitor defined by the default
// Decoder.
enum class InstDecodedKind : uint8_t {
  NotDecodedYet,
#define DECLARE(E) E,
  VISITOR_LIST(DECLARE)
#undef DECLARE
};

// A SinglePageDecodeCache is used to store the decoded kind of all instructions
// in an executable page of code. Each time an instruction is decoded, its
// decoded kind is recorded in this structure. The previous instruction value is
// also recorded in this structure when using a debug build.
//
// The next time the same offset is visited, the instruction would be decoded
// using the previously recorded decode kind. It is also compared against the
// previously recorded bits of the instruction to check for potential missing
// cache invalidations, in debug builds.
//
// This structure stores the equivalent of a single page of code to have better
// memory locality when using the simulator. As opposed to having a hash-table
// for all instructions. However a hash-table is used by the CachingDecoder to
// map the prefixes of page addresses to these SinglePageDecodeCaches.
class SinglePageDecodeCache {
 public:
  static const uintptr_t PageSize = 1 << 12;
  static const uintptr_t PageMask = PageSize - 1;
  static const uintptr_t InstSize = vixl::kInstructionSize;
  static const uintptr_t InstMask = InstSize - 1;
  static const uintptr_t InstPerPage = PageSize / InstSize;

  SinglePageDecodeCache(const Instruction* inst)
    : pageStart_(PageStart(inst))
  {
    memset(&decodeCache_, int(InstDecodedKind::NotDecodedYet), sizeof(decodeCache_));
  }
  // Compute the start address of the page which contains this instruction.
  static uintptr_t PageStart(const Instruction* inst) {
    return uintptr_t(inst) & ~PageMask;
  }
  // Returns whether the instruction decoded kind is stored in this
  // SinglePageDecodeCache.
  bool contains(const Instruction* inst) {
    return pageStart_ == PageStart(inst);
  }
  void clearDecode(const Instruction* inst) {
    uintptr_t offset = (uintptr_t(inst) & PageMask) / InstSize;
    decodeCache_[offset] = InstDecodedKind::NotDecodedYet;
  }
  InstDecodedKind* decodePtr(const Instruction* inst) {
    uintptr_t offset = (uintptr_t(inst) & PageMask) / InstSize;
    uint32_t instValue = *reinterpret_cast<const uint32_t*>(inst);
    instCache_[offset] = instValue;
    return &decodeCache_[offset];
  }
  InstDecodedKind decode(const Instruction* inst) const {
    uintptr_t offset = (uintptr_t(inst) & PageMask) / InstSize;
    InstDecodedKind val = decodeCache_[offset];
    uint32_t instValue = *reinterpret_cast<const uint32_t*>(inst);
    MOZ_ASSERT_IF(val != InstDecodedKind::NotDecodedYet,
                  instCache_[offset] == instValue);
    return val;
  }

 private:
  // Record the address at which the corresponding code page starts.
  const uintptr_t pageStart_;

  // Cache what instruction got decoded previously, in order to assert if we see
  // any stale instructions after.
  uint32_t instCache_[InstPerPage];

  // Cache the decoding of the instruction such that we can skip the decoding
  // part.
  InstDecodedKind decodeCache_[InstPerPage];
};

// A DecoderVisitor which will record which visitor function should be called
// the next time we want to decode the same instruction.
class CachingDecoderVisitor : public DecoderVisitor {
 public:
  CachingDecoderVisitor() = default;
  virtual ~CachingDecoderVisitor() {}

#define DECLARE(A) virtual void Visit##A(const Instruction* instr) { \
    if (last_) { \
      MOZ_ASSERT(*last_ == InstDecodedKind::NotDecodedYet); \
      *last_ = InstDecodedKind::A; \
      last_ = nullptr; \
    } \
  };

  VISITOR_LIST(DECLARE)
#undef DECLARE

  void setDecodePtr(InstDecodedKind* ptr) {
    last_ = ptr;
  }

 private:
  InstDecodedKind* last_;
};

// The Caching decoder works by extending the default vixl Decoder class. It
// extends it by overloading the Decode function.
//
// The overloaded Decode function checks whether the instruction given as
// argument got decoded before or since it got invalidated. If it was not
// previously decoded, the value of the instruction is recorded as well as the
// kind of instruction. Otherwise, the value of the instruction is checked
// against the previously recorded value and the instruction kind is used to
// skip the decoding visitor and resume the execution of instruction.
//
// The caching decoder stores the equivalent of a page of executable code in a
// hash-table. Each SinglePageDecodeCache stores an array of decoded kind as
// well as the value of the previously decoded instruction.
//
// When testing if an instruction was decoded before, we check if the address of
// the instruction is contained in the last SinglePageDecodeCache. If it is not,
// then the hash-table entry is queried and created if necessary, and the last
// SinglePageDecodeCache is updated. Then, the last SinglePageDecodeCache
// necessary contains the decoded kind of the instruction given as argument.
//
// The caching decoder add an extra function for flushing the cache, which is in
// charge of clearing the decoded kind of instruction in the range of addresses
// given as argument. This is indirectly called by
// CPU::EnsureIAndDCacheCoherency.
class CachingDecoder : public Decoder {
  using ICacheMap = mozilla::HashMap<uintptr_t, SinglePageDecodeCache*>;
 public:
  CachingDecoder()
      : lastPage_(nullptr)
  {
    PrependVisitor(&cachingDecoder_);
  }
  ~CachingDecoder() {
    RemoveVisitor(&cachingDecoder_);
  }

  void Decode(const Instruction* instr);
  void Decode(Instruction* instr) {
    Decode(const_cast<const Instruction*>(instr));
  }

  void FlushICache(void* start, size_t size);

 private:
  // Record the type of the decoded instruction, to avoid decoding it a second
  // time the next time we execute it.
  CachingDecoderVisitor cachingDecoder_;

  // Store the mapping of Instruction pointer to the corresponding
  // SinglePageDecodeCache.
  ICacheMap iCache_;

  // Record the last SinglePageDecodeCache seen, such that we can quickly access
  // it for the next instruction.
  SinglePageDecodeCache* lastPage_;
};

}
#endif // !JS_CACHE_SIMULATOR_ARM64
#endif // !VIXL_A64_MOZ_CACHING_DECODER_A64_H_
