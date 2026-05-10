//===- LLDMapFile.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the /lldmap option. It shows lists in order and
// hierarchically the output sections, input sections, input files and
// symbol:
//
//   Address  Size     Align Out     File    Symbol
//   00201000 00000015     4 .text
//   00201000 0000000e     4         test.o:(.text)
//   0020100e 00000000     0                 local
//   00201005 00000000     0                 f(int)
//
//===----------------------------------------------------------------------===//

#include "LLDMapFile.h"
#include "COFFLinkerContext.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;
using namespace lld;
using namespace lld::coff;

using SymbolMapTy = DenseMap<const Chunk *, SmallVector<Defined *, 4>>;

static constexpr char indent8[] = "        ";          // 8 spaces
static constexpr char indent16[] = "                "; // 16 spaces

// Print out the first three columns of a line.
static void writeHeader(raw_ostream &os, uint64_t addr, uint64_t size,
                        uint64_t align) {
  os << format("%08llx %08llx %5lld ", addr, size, align);
}

static InputFile *getFileForSymbol(Defined *sym) {
  if (auto *impSym = dyn_cast<DefinedImportData>(sym))
    return impSym->file;
  if (auto *thunkSym = dyn_cast<DefinedImportThunk>(sym))
    return thunkSym->wrappedSym->file;
  if (auto *coffSym = dyn_cast<DefinedCOFF>(sym))
    return coffSym->getFile();
  return nullptr;
}

static std::string getFileDescription(InputFile *file) {
  if (!file)
    return "<linker-defined>";

  SmallString<128> fileDescr;
  if (!file->parentName.empty()) {
    fileDescr = sys::path::filename(file->parentName);
    sys::path::replace_extension(fileDescr, "");
    fileDescr += ":";
  }
  fileDescr += sys::path::filename(file->getName());
  return std::string(fileDescr);
}

// Returns a list of all symbols that we want to print out.
static std::vector<Defined *> getSymbols(const COFFLinkerContext &ctx) {
  DenseSet<Defined *> seen;
  std::vector<Defined *> v;
  auto addSymbol = [&](Defined *sym) {
    if (!sym || !sym->getChunk() || !ctx.getOutputSection(sym->getChunk()) ||
        !seen.insert(sym).second)
      return;
    v.push_back(sym);
  };

  for (ObjFile *file : ctx.objFileInstances)
    for (Symbol *b : file->getSymbols()) {
      if (!b || !b->isLive())
        continue;
      if (auto *sym = dyn_cast<DefinedCOFF>(b)) {
        COFFSymbolRef symRef = sym->getCOFFSymbol();
        if (!symRef.isSectionDefinition() &&
            symRef.getStorageClass() != llvm::COFF::IMAGE_SYM_CLASS_LABEL)
          addSymbol(sym);
      } else {
        addSymbol(dyn_cast<Defined>(b));
      }
    }

  for (ImportFile *file : ctx.importFileInstances) {
    if (!file->live)
      continue;
    addSymbol(file->impSym);
    if (file->thunkSym && file->thunkSym->isLive())
      addSymbol(file->thunkSym);
    if (file->auxThunkSym && file->auxThunkSym->isLive())
      addSymbol(file->auxThunkSym);
    if (file->impchkThunk)
      addSymbol(file->impchkThunk->sym);
    addSymbol(file->impECSym);
    addSymbol(file->auxImpCopySym);
  }
  return v;
}

// Returns a map from chunks to their symbols.
static SymbolMapTy getChunkSyms(ArrayRef<Defined *> syms) {
  SymbolMapTy ret;
  for (Defined *s : syms)
    ret[s->getChunk()].push_back(s);

  // Sort symbols by address.
  for (auto &it : ret) {
    SmallVectorImpl<Defined *> &v = it.second;
    llvm::stable_sort(v, [](Defined *a, Defined *b) {
      return a->getRVA() < b->getRVA();
    });
  }
  return ret;
}

// Construct a map from symbols to their stringified representations.
static DenseMap<Defined *, std::string>
getSymbolStrings(const COFFLinkerContext &ctx, ArrayRef<Defined *> syms) {
  std::vector<std::string> str(syms.size());
  parallelFor((size_t)0, syms.size(), [&](size_t i) {
    raw_string_ostream os(str[i]);
    writeHeader(os, syms[i]->getRVA(), 0, 0);
    os << indent16 << syms[i]->getName();
  });

  DenseMap<Defined *, std::string> ret;
  for (size_t i = 0, e = syms.size(); i < e; ++i)
    ret[syms[i]] = std::move(str[i]);
  return ret;
}

void lld::coff::writeLLDMapFile(const COFFLinkerContext &ctx) {
  if (ctx.config.lldmapFile.empty())
    return;

  llvm::TimeTraceScope timeScope(".lldmap file");
  std::error_code ec;
  raw_fd_ostream os(ctx.config.lldmapFile, ec, sys::fs::OF_None);
  if (ec)
    fatal("cannot open " + ctx.config.lldmapFile + ": " + ec.message());

  // Collect symbol info that we want to print out.
  std::vector<Defined *> syms = getSymbols(ctx);
  SymbolMapTy chunkSyms = getChunkSyms(syms);
  DenseMap<Defined *, std::string> symStr = getSymbolStrings(ctx, syms);

  // Print out the header line.
  os << "Address  Size     Align Out     In      Symbol\n";

  // Print out file contents.
  for (OutputSection *sec : ctx.outputSections) {
    writeHeader(os, sec->getRVA(), sec->getVirtualSize(), /*align=*/pageSize);
    os << sec->name << '\n';

    for (Chunk *c : sec->chunks) {
      auto it = chunkSyms.find(c);
      if (!isa<SectionChunk>(c) && it == chunkSyms.end())
        continue;

      writeHeader(os, c->getRVA(), c->getSize(), c->getAlignment());
      if (auto *sc = dyn_cast<SectionChunk>(c)) {
        os << indent8 << sc->file->getName() << ":(" << sc->getSectionName()
           << ")\n";
      } else {
        std::string label =
            getFileDescription(it == chunkSyms.end() || it->second.empty()
                                   ? nullptr
                                   : getFileForSymbol(it->second.front()));
        if (label == "<linker-defined>") {
          StringRef debugName = c->getDebugName();
          if (!debugName.empty())
            label = debugName.str();
        }
        os << indent8 << label << '\n';
      }

      if (it == chunkSyms.end())
        continue;
      for (Defined *sym : it->second)
        os << symStr[sym] << '\n';
    }
  }
}
