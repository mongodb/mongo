/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ModuleBuilder_h
#define vm_ModuleBuilder_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS

#include "jstypes.h"                // JS_PUBLIC_API
#include "frontend/EitherParser.h"  // js::frontend::EitherParser
#include "frontend/ParserAtom.h"    // js::frontend::TaggedParserAtomIndex
#include "frontend/Stencil.h"       // js::frontend::StencilModuleEntry
#include "frontend/TaggedParserAtomIndexHasher.h"  // frontend::TaggedParserAtomIndexHasher
#include "js/GCVector.h"                           // JS::GCVector
#include "js/HashTable.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSAtom;

namespace js {

namespace frontend {

class BinaryNode;
class ListNode;
class ParseNode;

}  // namespace frontend

// Process a module's parse tree to collate the import and export data used when
// creating a ModuleObject.
class MOZ_STACK_CLASS ModuleBuilder {
  explicit ModuleBuilder(FrontendContext* fc,
                         const frontend::EitherParser& eitherParser);

 public:
  template <class Parser>
  explicit ModuleBuilder(FrontendContext* fc, Parser* parser)
      : ModuleBuilder(fc, frontend::EitherParser(parser)) {}

  bool processImport(frontend::BinaryNode* importNode);
  bool processExport(frontend::ParseNode* exportNode);
  bool processExportFrom(frontend::BinaryNode* exportNode);

  enum class NoteExportedNameResult {
    OutOfMemory,
    Success,
    AlreadyDeclared,
  };

  NoteExportedNameResult noteExportedName(frontend::TaggedParserAtomIndex name);

  bool buildTables(frontend::StencilModuleMetadata& metadata);

  // During BytecodeEmitter we note top-level functions, and afterwards we must
  // call finishFunctionDecls on the list.
  bool noteFunctionDeclaration(FrontendContext* fc, uint32_t funIndex);
  void finishFunctionDecls(frontend::StencilModuleMetadata& metadata);

  void noteAsync(frontend::StencilModuleMetadata& metadata);

 private:
  using MaybeModuleRequestIndex = frontend::MaybeModuleRequestIndex;
  using ModuleRequestVector = frontend::StencilModuleMetadata::RequestVector;
  using RequestedModuleVector = frontend::StencilModuleMetadata::EntryVector;

  using AtomSet = HashSet<frontend::TaggedParserAtomIndex,
                          frontend::TaggedParserAtomIndexHasher>;
  using ModuleRequestMap =
      HashMap<frontend::StencilModuleRequest, uint32_t,
              frontend::StencilModuleRequestHasher, js::SystemAllocPolicy>;
  using RequestedModuleSet = HashSet<uint32_t, DefaultHasher<uint32_t>>;
  using ExportEntryVector = Vector<frontend::StencilModuleEntry>;
  using ImportEntryMap =
      HashMap<frontend::TaggedParserAtomIndex, frontend::StencilModuleEntry,
              frontend::TaggedParserAtomIndexHasher>;

  FrontendContext* fc_;
  frontend::EitherParser eitherParser_;

  // These are populated while parsing.
  ModuleRequestVector moduleRequests_;
  ModuleRequestMap moduleRequestIndexes_;
  // The set contains the ModuleRequestIndexes in requestedModules_.
  RequestedModuleSet requestedModuleIndexes_;
  RequestedModuleVector requestedModules_;
  ImportEntryMap importEntries_;
  ExportEntryVector exportEntries_;
  AtomSet exportNames_;

  // These are populated while emitting bytecode.
  FunctionDeclarationVector functionDecls_;

  frontend::StencilModuleEntry* importEntryFor(
      frontend::TaggedParserAtomIndex localName) const;

  bool processExportBinding(frontend::ParseNode* binding);
  bool processExportArrayBinding(frontend::ListNode* array);
  bool processExportObjectBinding(frontend::ListNode* obj);

  MaybeModuleRequestIndex appendModuleRequest(
      frontend::TaggedParserAtomIndex specifier,
      frontend::ListNode* attributeList);

  bool appendExportEntry(frontend::TaggedParserAtomIndex exportName,
                         frontend::TaggedParserAtomIndex localName,
                         frontend::ParseNode* node = nullptr);

  bool maybeAppendRequestedModule(MaybeModuleRequestIndex moduleRequest,
                                  frontend::ParseNode* node);

  void markUsedByStencil(frontend::TaggedParserAtomIndex name);

  [[nodiscard]] bool processAttributes(frontend::StencilModuleRequest& request,
                                       frontend::ListNode* attributeList);

  [[nodiscard]] bool isAttributeSupported(frontend::TaggedParserAtomIndex key);
};

template <typename T>
ArrayObject* CreateArray(JSContext* cx,
                         const JS::Rooted<JS::GCVector<T>>& vector);

}  // namespace js

#endif  // vm_ModuleBuilder_h
