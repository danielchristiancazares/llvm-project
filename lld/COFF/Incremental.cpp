#include "Incremental.h"
#include "COFFLinkerContext.h"
#include "IncrementalRedirects.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::object;

namespace lld::coff {

std::unique_ptr<IncrementalCoordinator> IncrementalCoordinator::makeDisabled() {
  return std::unique_ptr<IncrementalCoordinator>(
      new IncrementalCoordinator(State::make<IncrementalDisabled>()));
}

std::unique_ptr<IncrementalCoordinator> IncrementalCoordinator::makeFullImageBuild(
    IncrementalFallbackReason fallbackReason, std::string fallbackDetail,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(new IncrementalCoordinator(
      State::make<FullImageBuild>(
          FullImageBuild{fallbackReason, std::move(fallbackDetail),
                         std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator> IncrementalCoordinator::makeStateBackedLink(
    IncrementalBaselineData baseline, IncrementalPdbReusePolicy pdbReuse,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(new IncrementalCoordinator(
      State::make<StateBackedLink>(StateBackedLink{
          std::move(baseline), std::move(pdbReuse), std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator> IncrementalCoordinator::makeLayoutStableLink(
    IncrementalBaselineData baseline, IncrementalPdbReusePolicy pdbReuse,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(new IncrementalCoordinator(
      State::make<LayoutStableLink>(
          LayoutStableLink{std::move(baseline), std::move(pdbReuse),
                           std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator> IncrementalCoordinator::makeByteReuseLink(
    IncrementalBaselineData baseline, IncrementalReuseData reuse,
    IncrementalPdbReusePolicy pdbReuse,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(new IncrementalCoordinator(
      State::make<ByteReuseLink>(ByteReuseLink{std::move(baseline),
                                               std::move(reuse),
                                               std::move(pdbReuse),
                                               std::move(baselineEmission)})));
}

namespace {

struct IncrementalStateBuildResult {
  IncrementalBaselineEmission baselineEmission =
      IncrementalBaselineEmission::make<EmitNextBaseline>();
  IncrementalStateFile state;
};

static bool shouldEmitNextBaseline(
    const IncrementalBaselineEmission &baselineEmission) {
  return baselineEmission.match(
      [](const EmitNextBaseline &) { return true; },
      [](const SkipNextBaseline &) { return false; });
}

static bool shouldReusePdbMetadata(
    const IncrementalPdbReusePolicy &pdbReuse) {
  return pdbReuse.match([](const ReusePdbMetadata &) { return true; },
                        [](const RebuildPdbMetadata &) { return false; });
}

static void clearPendingIncrementalFallback(COFFLinkerContext &ctx) {
  ctx.pendingIncrementalFallback.reset();
}

static void logFullImageBuild(COFFLinkerContext &ctx,
                              IncrementalFallbackReason reason,
                              StringRef detail) {
  if (!ctx.config.verbose || reason == IncrementalFallbackReason::None)
    return;
  Log(ctx) << "incremental: fallback: "
           << incrementalFallbackReasonToString(reason);
  if (!detail.empty())
    Log(ctx) << "incremental: detail: " << detail;
}

static void installIncrementalCoordinatorImpl(
    COFFLinkerContext &ctx, std::unique_ptr<IncrementalCoordinator> coordinator) {
  ctx.incremental = std::move(coordinator);
  clearPendingIncrementalFallback(ctx);
}

static std::unique_ptr<IncrementalCoordinator>
buildFullImageBuild(COFFLinkerContext &ctx, IncrementalFallbackReason reason,
                    const Twine &detail,
                    IncrementalBaselineEmission baselineEmission) {
  std::string detailText = detail.str();
  logFullImageBuild(ctx, reason, detailText);
  return IncrementalCoordinator::makeFullImageBuild(
      reason, std::move(detailText), std::move(baselineEmission));
}

static std::unique_ptr<FullImageBuild> consumePendingIncrementalFallbackImpl(
    COFFLinkerContext &ctx, IncrementalBaselineEmission baselineEmission,
    IncrementalFallbackReason defaultReason, const Twine &defaultDetail) {
  if (!ctx.pendingIncrementalFallback)
    return std::make_unique<FullImageBuild>(FullImageBuild{
        defaultReason, defaultDetail.str(), std::move(baselineEmission)});

  std::unique_ptr<FullImageBuild> fallback = std::make_unique<FullImageBuild>(
      FullImageBuild{ctx.pendingIncrementalFallback->fallbackReason,
                     ctx.pendingIncrementalFallback->fallbackDetail,
                     std::move(baselineEmission)});
  clearPendingIncrementalFallback(ctx);
  return fallback;
}

static bool hasBitcodeInputs(COFFLinkerContext &ctx) {
  bool hasBitcode = false;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    hasBitcode |= !symtab.bitcodeFileInstances.empty();
  });
  return hasBitcode;
}

static std::unique_ptr<StateBackedLink>
takeStateBackedLink(COFFLinkerContext &ctx) {
  std::unique_ptr<StateBackedLink> result;
  ctx.incremental->match(
      [&](IncrementalDisabled &) {},
      [&](FullImageBuild &) {},
      [&](StateBackedLink &loaded) {
        result = std::make_unique<StateBackedLink>(std::move(loaded));
      },
      [&](LayoutStableLink &) {},
      [&](ByteReuseLink &) {});
  if (result)
    ctx.incremental = IncrementalCoordinator::makeDisabled();
  return result;
}

static std::unique_ptr<LayoutStableLink>
takeLayoutStableLink(COFFLinkerContext &ctx) {
  std::unique_ptr<LayoutStableLink> result;
  ctx.incremental->match(
      [&](IncrementalDisabled &) {},
      [&](FullImageBuild &) {},
      [&](StateBackedLink &) {},
      [&](LayoutStableLink &validated) {
        result = std::make_unique<LayoutStableLink>(std::move(validated));
      },
      [&](ByteReuseLink &) {});
  if (result)
    ctx.incremental = IncrementalCoordinator::makeDisabled();
  return result;
}

static ByteReuseLink *findActiveByteReuseLinkImpl(COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](IncrementalDisabled &) -> ByteReuseLink * { return nullptr; },
      [&](FullImageBuild &) -> ByteReuseLink * { return nullptr; },
      [&](StateBackedLink &) -> ByteReuseLink * { return nullptr; },
      [&](LayoutStableLink &) -> ByteReuseLink * { return nullptr; },
      [&](ByteReuseLink &reuse) -> ByteReuseLink * { return &reuse; });
}

static const ByteReuseLink *
findActiveByteReuseLinkImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) -> const ByteReuseLink * {
        return nullptr;
      },
      [&](const FullImageBuild &) -> const ByteReuseLink * { return nullptr; },
      [&](const StateBackedLink &) -> const ByteReuseLink * { return nullptr; },
      [&](const LayoutStableLink &) -> const ByteReuseLink * { return nullptr; },
      [&](const ByteReuseLink &reuse) -> const ByteReuseLink * {
        return &reuse;
      });
}

static IncrementalBaselineData *
findActiveIncrementalBaselineImpl(COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](IncrementalDisabled &) -> IncrementalBaselineData * { return nullptr; },
      [&](FullImageBuild &) -> IncrementalBaselineData * { return nullptr; },
      [&](StateBackedLink &loaded) -> IncrementalBaselineData * {
        return &loaded.baseline;
      },
      [&](LayoutStableLink &validated) -> IncrementalBaselineData * {
        return &validated.baseline;
      },
      [&](ByteReuseLink &reuse) -> IncrementalBaselineData * {
        return &reuse.baseline;
      });
}

static const IncrementalBaselineData *
findActiveIncrementalBaselineImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) -> const IncrementalBaselineData * {
        return nullptr;
      },
      [&](const FullImageBuild &) -> const IncrementalBaselineData * {
        return nullptr;
      },
      [&](const StateBackedLink &loaded) -> const IncrementalBaselineData * {
        return &loaded.baseline;
      },
      [&](const LayoutStableLink &validated)
          -> const IncrementalBaselineData * { return &validated.baseline; },
      [&](const ByteReuseLink &reuse) -> const IncrementalBaselineData * {
        return &reuse.baseline;
      });
}

static const IncrementalStateFile *
findActiveIncrementalLoadedStateImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) -> const IncrementalStateFile * {
        return nullptr;
      },
      [&](const FullImageBuild &) -> const IncrementalStateFile * {
        return nullptr;
      },
      [&](const StateBackedLink &loaded) -> const IncrementalStateFile * {
        return &loaded.baseline.state;
      },
      [&](const LayoutStableLink &validated) -> const IncrementalStateFile * {
        return &validated.baseline.state;
      },
      [&](const ByteReuseLink &reuse) -> const IncrementalStateFile * {
        return &reuse.baseline.state;
      });
}

static bool shouldEmitIncrementalBaselineImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) { return false; },
      [&](const FullImageBuild &fresh) {
        return shouldEmitNextBaseline(fresh.baselineEmission);
      },
      [&](const StateBackedLink &loaded) {
        return shouldEmitNextBaseline(loaded.baselineEmission);
      },
      [&](const LayoutStableLink &validated) {
        return shouldEmitNextBaseline(validated.baselineEmission);
      },
      [&](const ByteReuseLink &reuse) {
        return shouldEmitNextBaseline(reuse.baselineEmission);
      });
}

static bool shouldReuseIncrementalPdbMetadataImpl(
    const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) { return false; },
      [&](const FullImageBuild &) { return false; },
      [&](const StateBackedLink &loaded) {
        return shouldReusePdbMetadata(loaded.pdbReuse);
      },
      [&](const LayoutStableLink &validated) {
        return shouldReusePdbMetadata(validated.pdbReuse);
      },
      [&](const ByteReuseLink &reuse) {
        return shouldReusePdbMetadata(reuse.pdbReuse);
      });
}

static void appendSortedStrings(raw_ostream &os, const StringSet<> &set) {
  SmallVector<StringRef, 16> keys;
  keys.reserve(set.size());
  for (const auto &entry : set)
    keys.push_back(entry.getKey());
  llvm::sort(keys);
  for (StringRef key : keys)
    os << key << '\n';
}

static void appendSortedStringMap(raw_ostream &os,
                                  const StringMap<std::string> &map) {
  SmallVector<StringRef, 16> keys;
  keys.reserve(map.size());
  for (const auto &entry : map)
    keys.push_back(entry.getKey());
  llvm::sort(keys);
  for (StringRef key : keys)
    os << key << '=' << map.lookup(key) << '\n';
}

static void appendManifestInputs(raw_ostream &os,
                                 const std::vector<std::string> &inputs) {
  for (const std::string &input : inputs)
    os << input << '\n';
}

static void ensureIncrementalStatePath(Configuration &config) {
  if (!config.incrementalStatePath.empty() || config.outputFile.empty())
    return;
  config.incrementalStatePath = config.outputFile;
  sys::path::replace_extension(config.incrementalStatePath, ".llilk");
}

static std::string getIncrementalArchiveMemberKey(StringRef archiveName,
                                                  uint64_t archiveOffset,
                                                  StringRef memberName) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  os << archiveName << '\n' << archiveOffset << '\n' << memberName;
  return std::string(buffer);
}

static uint64_t computeIncrementalImportTopologyHash(COFFLinkerContext &ctx) {
  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  for (ImportFile *file : ctx.importFileInstances) {
    if (!file->live)
      continue;
    // Preserve the live import order because createImportTables() keeps the
    // first-seen DLL order, which affects the observable IAT layout.
    os << file->dllName << '\n'
       << file->externalName << '\n'
       << file->hdr->OrdinalHint << '\n'
       << file->hdr->TypeInfo << '\n';
  }
  return xxh3_64bits(buffer);
}

static uint64_t computeIncrementalExportTopologyHash(COFFLinkerContext &ctx) {
  SmallVector<std::string, 16> records;
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    for (const Export &e : symtab.exports) {
      std::string record;
      raw_string_ostream os(record);
      os << e.name << '\n'
         << e.extName << '\n'
         << e.exportAs << '\n'
         << e.importName << '\n'
         << e.forwardTo << '\n'
         << e.exportName << '\n'
         << e.symbolName << '\n'
         << e.ordinal << '\n'
         << e.noname << '\n'
         << e.data << '\n'
         << e.isPrivate << '\n'
         << e.constant << '\n'
         << unsigned(e.source) << '\n';
      records.push_back(std::move(record));
    }
  });
  llvm::sort(records);

  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  for (const std::string &record : records)
    os << record;
  return xxh3_64bits(buffer);
}

static uint64_t computeIncrementalResourceInputHash(COFFLinkerContext &ctx) {
  SmallVector<std::string, 8> records;
  for (MemoryBufferRef mb : ctx.driver.getResources()) {
    std::string record;
    raw_string_ostream os(record);
    os << mb.getBufferIdentifier() << '\n'
       << xxh3_64bits(mb.getBuffer()) << '\n';
    records.push_back(std::move(record));
  }
  llvm::sort(records);

  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  for (const std::string &record : records)
    os << record;
  return xxh3_64bits(buffer);
}

static IncrementalBaselineEmission
buildIncrementalLayoutTables(COFFLinkerContext &ctx,
                             const IncrementalInputIndexMap &inputIndices,
                             IncrementalStateFile &state) {
  enum class BaselineEmissionChoice : uint8_t {
    Emit = 1,
    Skip = 2,
  };

  BaselineEmissionChoice baselineEmission = BaselineEmissionChoice::Emit;
  SmallVector<OutputSection *, 16> activeSections;
  for (OutputSection *section : ctx.outputSections)
    if (section->getVirtualSize() != 0)
      activeSections.push_back(section);

  DenseMap<const OutputSection *, uint32_t> envelopeIndices;
  for (size_t sectionIndex = 0; sectionIndex < activeSections.size();
       ++sectionIndex) {
    OutputSection *section = activeSections[sectionIndex];
    IncrementalSectionLayoutKind layoutKind = classifyIncrementalSection(
        section->name, section->header.Characteristics);
    if (layoutKind == IncrementalSectionLayoutKind::ExactSectionLayout)
      continue;

    IncrementalSectionEnvelopeState envelope;
    envelope.name = section->name.str();
    envelope.characteristics = section->header.Characteristics;
    envelope.sectionRVA = section->getRVA();
    envelope.maxSectionEndRVA =
        sectionIndex + 1 < activeSections.size()
            ? activeSections[sectionIndex + 1]->getRVA()
            : state.sizeOfImage;
    envelope.layoutKind = layoutKind;
    uint64_t activeEndRVA = section->getRVA() + section->getVirtualSize();
    if (layoutKind == IncrementalSectionLayoutKind::TextFreeSlots) {
      activeEndRVA = section->getRVA();
      for (Chunk *chunk : section->chunks) {
        if (isa<IncrementalLongThunkChunkX64>(chunk))
          continue;
        activeEndRVA = std::max(activeEndRVA, chunk->getRVA() + chunk->getSize());
      }
    }
    envelope.activeEndRVA = activeEndRVA;
    envelopeIndices[section] = state.sectionEnvelopes.size();
    state.sectionEnvelopes.push_back(std::move(envelope));
  }

  for (OutputSection *section : activeSections) {
    auto envelopeIt = envelopeIndices.find(section);
    if (envelopeIt == envelopeIndices.end())
      continue;

    uint32_t envelopeIndex = envelopeIt->second;
    const IncrementalSectionEnvelopeState &envelope =
        state.sectionEnvelopes[envelopeIndex];

    if (isIncrementalFreeSlotLayout(envelope.layoutKind)) {
      auto suppressStateWrite = [&](const Twine &detail) {
        baselineEmission = BaselineEmissionChoice::Skip;
        if (ctx.config.verbose)
          Log(ctx) << "incremental: not writing state: " << detail;
      };
      auto isPersistedSlotChunk = [&](Chunk *chunk) {
        return isIncrementalPersistedSlotChunk(envelope.layoutKind, *chunk);
      };
      for (size_t chunkIndex = 0; chunkIndex < section->chunks.size();
           ++chunkIndex) {
        Chunk *chunk = section->chunks[chunkIndex];
        if (!isPersistedSlotChunk(chunk))
          continue;
        std::string key = getIncrementalChunkKey(inputIndices, *chunk);
        bool isPadding = isa<IncrementalPaddingChunk>(chunk);

        uint64_t slotEnd = envelope.activeEndRVA;
        for (size_t nextIndex = chunkIndex + 1; nextIndex < section->chunks.size();
             ++nextIndex) {
          Chunk *nextChunk = section->chunks[nextIndex];
          if (!isPersistedSlotChunk(nextChunk))
            continue;
          slotEnd = nextChunk->getRVA();
          break;
        }
        if (slotEnd < chunk->getRVA() ||
            slotEnd - chunk->getRVA() < chunk->getSize()) {
          suppressStateWrite("slot table builder found an invalid persisted range");
          return IncrementalBaselineEmission::make<SkipNextBaseline>();
        }

        IncrementalSlotRecordState slot;
        slot.envelopeIndex = envelopeIndex;
        slot.startRVA = chunk->getRVA();
        slot.capacity = slotEnd - chunk->getRVA();
        slot.committedSize = chunk->getSize();
        slot.minAlignment = chunk->getAlignment();
        slot.fillByte = isPadding ? cast<IncrementalPaddingChunk>(chunk)->getFillByte()
                                  : getIncrementalFillByte(envelope.layoutKind);
        slot.state = isPadding ? IncrementalSlotState::Free
                               : IncrementalSlotState::Occupied;
        if (!isPadding)
          slot.occupantKey = key;
        state.slotRecords.push_back(std::move(slot));

        if (isPadding)
          continue;

        IncrementalPlacementState placement;
        placement.key = key;
        placement.envelopeIndex = envelopeIndex;
        placement.kind = IncrementalPlacementKind::ExistingSlot;
        placement.startRVA = chunk->getRVA();
        placement.size = chunk->getSize();
        placement.alignment = chunk->getAlignment();
        state.placements.push_back(std::move(placement));
      }
      continue;
    }

    if (!isIncrementalPackedLayout(envelope.layoutKind))
      continue;

    IncrementalPackedSectionState packedSection;
    packedSection.envelopeIndex = envelopeIndex;
    packedSection.activePrefixSize =
        envelope.activeEndRVA - envelope.sectionRVA;
    packedSection.reserveSize =
        envelope.maxSectionEndRVA - envelope.activeEndRVA;
    for (Chunk *chunk : section->chunks) {
      if (chunk->getSize() == 0)
        continue;
      std::string key = getIncrementalChunkKey(inputIndices, *chunk);
      packedSection.recordKeys.push_back(key);

      IncrementalPlacementState placement;
      placement.key = key;
      placement.envelopeIndex = envelopeIndex;
      placement.kind = IncrementalPlacementKind::PackedPrefix;
      placement.startRVA = chunk->getRVA();
      placement.size = chunk->getSize();
      placement.alignment = chunk->getAlignment();
      state.placements.push_back(std::move(placement));
    }
    state.packedSections.push_back(std::move(packedSection));
  }

  if (baselineEmission == BaselineEmissionChoice::Skip)
    return IncrementalBaselineEmission::make<SkipNextBaseline>();
  return IncrementalBaselineEmission::make<EmitNextBaseline>();
}

static std::vector<IncrementalSymbolState>
buildIncrementalSymbolStates(COFFLinkerContext &ctx,
                             const IncrementalInputIndexMap &inputIndices) {
  std::vector<IncrementalSymbolState> states;
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    symtab.forEachSymbol([&](Symbol *sym) {
      auto *def = dyn_cast<Defined>(sym);
      if (!def)
        return;
      if (auto *coff = dyn_cast<DefinedCOFF>(sym))
        if (!coff->getCOFFSymbol().isExternal())
          return;

      IncrementalSymbolState state;
      state.name = sym->getName().str();

      if (auto *reg = dyn_cast<DefinedRegular>(sym)) {
        state.kind = IncrementalSymbolKind::Regular;
        if (auto *file = dyn_cast<ObjFile>(reg->getFile()))
          if (auto it = inputIndices.find(file); it != inputIndices.end())
            state.inputIndex = it->second;
        state.value = reg->getValue();
        if (SectionChunk *chunk = reg->getChunk())
          state.auxiliaryKey = getIncrementalChunkKey(inputIndices, *chunk);
      } else if (auto *common = dyn_cast<DefinedCommon>(sym)) {
        state.kind = IncrementalSymbolKind::Common;
        if (auto *file = dyn_cast<ObjFile>(common->getFile()))
          if (auto it = inputIndices.find(file); it != inputIndices.end())
            state.inputIndex = it->second;
        state.value = common->getChunk()->getSize();
        std::string key;
        raw_string_ostream os(key);
        os << common->getChunk()->getAlignment();
        state.auxiliaryKey = os.str();
      } else if (auto *imp = dyn_cast<DefinedImportData>(sym)) {
        state.kind = IncrementalSymbolKind::ImportData;
        state.value = imp->getOrdinal();
        std::string key;
        raw_string_ostream os(key);
        os << imp->getDLLName() << '\n' << imp->getExternalName() << '\n'
           << imp->file->hdr->TypeInfo;
        state.auxiliaryKey = os.str();
      } else if (auto *thunk = dyn_cast<DefinedImportThunk>(sym)) {
        state.kind = IncrementalSymbolKind::ImportThunk;
        state.auxiliaryKey = thunk->wrappedSym->getName().str();
      } else if (auto *localImport = dyn_cast<DefinedLocalImport>(sym)) {
        state.kind = IncrementalSymbolKind::LocalImport;
        if (Chunk *chunk = localImport->getChunk())
          state.auxiliaryKey = chunk->getDebugName().str();
      } else if (auto *absolute = dyn_cast<DefinedAbsolute>(sym)) {
        state.kind = IncrementalSymbolKind::Absolute;
        state.value = absolute->getVA();
      } else if (auto *synthetic = dyn_cast<DefinedSynthetic>(sym)) {
        state.kind = IncrementalSymbolKind::Synthetic;
        if (Chunk *chunk = synthetic->getChunk())
          state.auxiliaryKey = chunk->getDebugName().str();
      } else {
        return;
      }

      states.push_back(std::move(state));
    });
  });

  llvm::sort(states, [](const IncrementalSymbolState &lhs,
                        const IncrementalSymbolState &rhs) {
    return std::tie(lhs.name, lhs.kind, lhs.inputIndex, lhs.value,
                    lhs.auxiliaryKey) <
           std::tie(rhs.name, rhs.kind, rhs.inputIndex, rhs.value,
                    rhs.auxiliaryKey);
  });
  return states;
}

static IncrementalStateBuildResult
buildIncrementalState(COFFLinkerContext &ctx,
                      const IncrementalCurrentInputs &currentInputs,
                      const IncrementalReuseData *reuseData) {
  IncrementalStateFile state;
  state.layoutMode = IncrementalLayoutMode::Slotted;
  state.machine = ctx.config.machine;
  state.outputPath = ctx.config.outputFile;
  state.hardConfigHash = computeIncrementalHardConfigHash(ctx.config);
  state.softConfigHash = computeIncrementalSoftConfigHash(ctx.config);
  state.importTopologyHash = computeIncrementalImportTopologyHash(ctx);
  state.exportTopologyHash = computeIncrementalExportTopologyHash(ctx);
  state.resourceInputHash = computeIncrementalResourceInputHash(ctx);

  ErrorOr<std::unique_ptr<MemoryBuffer>> output = MemoryBuffer::getFile(
      ctx.config.outputFile, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!output) {
    report_fatal_error(createFileError(ctx.config.outputFile,
                                       errorCodeToError(output.getError())));
  }
  StringRef outputData = (*output)->getBuffer();
  state.outputSize = outputData.size();
  state.outputHash = xxh3_64bits(outputData);

  if (outputData.size() >= sizeof(dos_header)) {
    const uint8_t *buf =
        reinterpret_cast<const uint8_t *>(outputData.data());
    const dos_header *dos = reinterpret_cast<const dos_header *>(buf);
    uint32_t peOff = dos->AddressOfNewExeHeader;
    uint32_t headerOff =
        peOff + sizeof(llvm::COFF::PEMagic) + sizeof(coff_file_header);
    if (ctx.config.is64() &&
        outputData.size() >= headerOff + sizeof(pe32plus_header)) {
      const pe32plus_header *pe =
          reinterpret_cast<const pe32plus_header *>(buf + headerOff);
      state.sizeOfHeaders = pe->SizeOfHeaders;
      state.sizeOfImage = pe->SizeOfImage;
    } else if (outputData.size() >= headerOff + sizeof(pe32_header)) {
      const pe32_header *pe =
          reinterpret_cast<const pe32_header *>(buf + headerOff);
      state.sizeOfHeaders = pe->SizeOfHeaders;
      state.sizeOfImage = pe->SizeOfImage;
    }
  }

  state.inputs.reserve(ctx.objFileInstances.size());
  for (size_t i = 0; i < ctx.objFileInstances.size(); ++i) {
    IncrementalInputState input;
    input.name = currentInputs.names[i];
    input.parentName = currentInputs.parentNames[i];
    input.archiveOffset = currentInputs.archiveOffsets[i];
    input.contentHash = currentInputs.hashes[i];
    input.size = ctx.objFileInstances[i]->mb.getBufferSize();
    state.inputs.push_back(std::move(input));
  }

  for (OutputSection *section : ctx.outputSections) {
    IncrementalSectionState sectionState;
    sectionState.name = section->name.str();
    sectionState.characteristics = section->header.Characteristics;
    sectionState.rva = section->getRVA();
    sectionState.fileOffset = section->getFileOff();
    sectionState.virtualSize = section->getVirtualSize();
    sectionState.rawSize = section->getRawSize();
    sectionState.firstChunk = state.chunks.size();
    sectionState.chunkCount = section->chunks.size();
    state.sections.push_back(sectionState);

    uint64_t sectionEnd = section->getRVA() + section->getVirtualSize();
    for (size_t i = 0; i < section->chunks.size(); ++i) {
      Chunk *chunk = section->chunks[i];
      IncrementalChunkState chunkState;
      chunkState.kind = classifyIncrementalChunk(*chunk);
      chunkState.key = getIncrementalChunkKey(currentInputs.inputIndices, *chunk);
      chunkState.sectionIndex = state.sections.size() - 1;
      chunkState.outputCharacteristics = chunk->getOutputCharacteristics();
      chunkState.alignment = chunk->getAlignment();
      chunkState.rva = chunk->getRVA();
      chunkState.size = chunk->getSize();
      if (i + 1 < section->chunks.size())
        chunkState.slotCapacity =
            section->chunks[i + 1]->getRVA() - chunk->getRVA();
      else
        chunkState.slotCapacity = sectionEnd - chunk->getRVA();

      if (auto *sec = dyn_cast<SectionChunk>(chunk)) {
        auto it = currentInputs.inputIndices.find(sec->file);
        if (it != currentInputs.inputIndices.end())
          chunkState.inputIndex = it->second;
        chunkState.sectionNumber = sec->getSectionNumber();
        chunkState.contentHash = xxh3_64bits(sec->getContents());
        chunkState.symbolHash = computeIncrementalSymbolHash(*sec);
      }
      state.chunks.push_back(std::move(chunkState));
    }
  }

  IncrementalBaselineEmission baselineEmission = buildIncrementalLayoutTables(
      ctx, currentInputs.inputIndices, state);
  if (reuseData) {
    state.textRedirects = reuseData->currentTextRedirects;
    state.textThunkPool = reuseData->currentTextThunkPool;
  }
  state.symbols = buildIncrementalSymbolStates(ctx, currentInputs.inputIndices);
  return IncrementalStateBuildResult{std::move(baselineEmission),
                                     std::move(state)};
}

} // namespace

void installIncrementalCoordinator(
    COFFLinkerContext &ctx,
    std::unique_ptr<IncrementalCoordinator> coordinator) {
  installIncrementalCoordinatorImpl(ctx, std::move(coordinator));
}

std::unique_ptr<FullImageBuild> consumePendingIncrementalFallback(
    COFFLinkerContext &ctx, IncrementalBaselineEmission baselineEmission,
    IncrementalFallbackReason defaultReason, const Twine &defaultDetail) {
  std::unique_ptr<FullImageBuild> fallback = consumePendingIncrementalFallbackImpl(
      ctx, std::move(baselineEmission), defaultReason, defaultDetail);
  logFullImageBuild(ctx, fallback->fallbackReason, fallback->fallbackDetail);
  return fallback;
}

IncrementalBaselineData *findActiveIncrementalBaseline(COFFLinkerContext &ctx) {
  return findActiveIncrementalBaselineImpl(ctx);
}

const IncrementalBaselineData *
findActiveIncrementalBaseline(const COFFLinkerContext &ctx) {
  return findActiveIncrementalBaselineImpl(ctx);
}

ByteReuseLink *findActiveByteReuseLink(COFFLinkerContext &ctx) {
  return findActiveByteReuseLinkImpl(ctx);
}

const ByteReuseLink *findActiveByteReuseLink(const COFFLinkerContext &ctx) {
  return findActiveByteReuseLinkImpl(ctx);
}

const IncrementalStateFile *
findActiveIncrementalLoadedState(const COFFLinkerContext &ctx) {
  return findActiveIncrementalLoadedStateImpl(ctx);
}

bool shouldEmitIncrementalBaseline(const COFFLinkerContext &ctx) {
  return shouldEmitIncrementalBaselineImpl(ctx);
}

bool shouldReuseIncrementalPdbMetadata(const COFFLinkerContext &ctx) {
  return shouldReuseIncrementalPdbMetadataImpl(ctx);
}

IncrementalSectionLayoutKind classifyIncrementalSection(StringRef name,
                                                        uint32_t characteristics) {
  if (name == ".pdata")
    return IncrementalSectionLayoutKind::PackedPDataPrefix;
  if (name == ".xdata")
    return IncrementalSectionLayoutKind::PackedXDataPrefix;
  if (name == ".text")
    return IncrementalSectionLayoutKind::TextFreeSlots;
  if (name == ".rdata")
    return IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots;
  if (name == ".data")
    return IncrementalSectionLayoutKind::WritableDataFreeSlots;

  if ((characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE) &&
      (characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
    return IncrementalSectionLayoutKind::TextFreeSlots;
  return IncrementalSectionLayoutKind::ExactSectionLayout;
}

bool isIncrementalFreeSlotLayout(IncrementalSectionLayoutKind layoutKind) {
  switch (layoutKind) {
  case IncrementalSectionLayoutKind::TextFreeSlots:
  case IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots:
  case IncrementalSectionLayoutKind::WritableDataFreeSlots:
    return true;
  case IncrementalSectionLayoutKind::ExactSectionLayout:
  case IncrementalSectionLayoutKind::PackedPDataPrefix:
  case IncrementalSectionLayoutKind::PackedXDataPrefix:
    return false;
  }
  llvm_unreachable("unknown incremental section layout");
}

bool isIncrementalPackedLayout(IncrementalSectionLayoutKind layoutKind) {
  switch (layoutKind) {
  case IncrementalSectionLayoutKind::PackedPDataPrefix:
  case IncrementalSectionLayoutKind::PackedXDataPrefix:
    return true;
  case IncrementalSectionLayoutKind::ExactSectionLayout:
  case IncrementalSectionLayoutKind::TextFreeSlots:
  case IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots:
  case IncrementalSectionLayoutKind::WritableDataFreeSlots:
    return false;
  }
  llvm_unreachable("unknown incremental section layout");
}

uint8_t getIncrementalFillByte(IncrementalSectionLayoutKind layoutKind) {
  switch (layoutKind) {
  case IncrementalSectionLayoutKind::TextFreeSlots:
    return 0xCC;
  case IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots:
  case IncrementalSectionLayoutKind::WritableDataFreeSlots:
  case IncrementalSectionLayoutKind::PackedPDataPrefix:
  case IncrementalSectionLayoutKind::PackedXDataPrefix:
  case IncrementalSectionLayoutKind::ExactSectionLayout:
    return 0x00;
  }
  llvm_unreachable("unknown incremental section layout");
}

bool isIncrementalPersistedSlotChunk(IncrementalSectionLayoutKind layoutKind,
                                     const Chunk &chunk) {
  if (chunk.getSize() == 0)
    return false;
  return layoutKind != IncrementalSectionLayoutKind::TextFreeSlots ||
         !isa<IncrementalLongThunkChunkX64>(&chunk);
}

IncrementalFreeSlotSelection findBestFitIncrementalFreeSlot(
    ArrayRef<IncrementalSlotRecordState> slots, uint64_t size,
    uint32_t alignment) {
  size_t bestIndex = 0;
  bool found = false;
  for (size_t i = 0; i < slots.size(); ++i) {
    const IncrementalSlotRecordState &slot = slots[i];
    if (slot.capacity < size || slot.startRVA % alignment != 0)
      continue;
    if (!found || slot.capacity < slots[bestIndex].capacity ||
        (slot.capacity == slots[bestIndex].capacity &&
         slot.startRVA < slots[bestIndex].startRVA)) {
      bestIndex = i;
      found = true;
    }
  }
  if (!found)
    return IncrementalFreeSlotSelection::make<NoFreeSlotFit>();
  return IncrementalFreeSlotSelection::make<SelectedFreeSlot>(
      SelectedFreeSlot{bestIndex});
}

IncrementalTailReserveSelection allocateIncrementalTailReserve(
    uint64_t tailCursor, uint64_t maxSectionEndRVA, uint64_t size,
    uint32_t alignment) {
  uint64_t startRVA = alignTo(tailCursor, uint64_t(alignment));
  if (startRVA > maxSectionEndRVA || maxSectionEndRVA - startRVA < size)
    return IncrementalTailReserveSelection::make<TailReserveUnavailable>();
  return IncrementalTailReserveSelection::make<TailReserveStart>(
      TailReserveStart{startRVA});
}

IncrementalTextThunkSelection
chooseIncrementalTextThunkRVA(uint64_t oldPoolThunkRVA, uint64_t tailCursor,
                              uint64_t poolCursor, uint64_t poolEndRVA,
                              ArrayRef<uint64_t> claimedThunkRVAs,
                              ArrayRef<uint64_t> freedThunkRVAs) {
  constexpr uint64_t thunkSize = 16;
  auto isClaimed = [&](uint64_t rva) {
    return llvm::is_contained(claimedThunkRVAs, rva);
  };

  auto canReuse = [&](uint64_t rva) {
    return rva != 0 && rva % thunkSize == 0 && rva >= tailCursor &&
           rva >= poolCursor && rva <= poolEndRVA &&
           poolEndRVA - rva >= thunkSize && !isClaimed(rva);
  };

  if (canReuse(oldPoolThunkRVA))
    return IncrementalTextThunkSelection::make<SelectedPoolThunkRVA>(
        SelectedPoolThunkRVA{oldPoolThunkRVA});

  uint64_t bestFreedThunkRVA = 0;
  bool foundFreedThunk = false;
  for (uint64_t freedThunkRVA : freedThunkRVAs) {
    if (!canReuse(freedThunkRVA))
      continue;
    if (!foundFreedThunk || freedThunkRVA > bestFreedThunkRVA) {
      bestFreedThunkRVA = freedThunkRVA;
      foundFreedThunk = true;
    }
  }
  if (foundFreedThunk)
    return IncrementalTextThunkSelection::make<SelectedPoolThunkRVA>(
        SelectedPoolThunkRVA{bestFreedThunkRVA});

  uint64_t nextCursor = poolCursor;
  while (nextCursor > tailCursor && nextCursor - tailCursor >= thunkSize) {
    uint64_t candidate = (nextCursor - thunkSize) & ~(thunkSize - 1);
    if (candidate < tailCursor)
      break;
    if (candidate <= poolEndRVA && poolEndRVA - candidate >= thunkSize &&
        !isClaimed(candidate))
      return IncrementalTextThunkSelection::make<SelectedPoolThunkRVA>(
          SelectedPoolThunkRVA{candidate});
    nextCursor = candidate;
  }
  return IncrementalTextThunkSelection::make<PoolThunkUnavailable>();
}

void planIncrementalTextThunkAssignments(
    MutableArrayRef<IncrementalTextThunkPlanState> plans, uint64_t tailCursor,
    uint64_t &poolCursor, uint64_t &poolStart, uint64_t poolEndRVA,
    bool allowPoolThunks) {
  SmallVector<uint64_t, 8> claimedThunkRVAs;
  SmallVector<uint64_t, 8> freedThunkRVAs;
  auto releasePoolThunkRVA = [&](IncrementalTextThunkPlanState &plan) {
    if (plan.poolThunkRVA != 0 &&
        !llvm::is_contained(claimedThunkRVAs, plan.poolThunkRVA))
      freedThunkRVAs.push_back(plan.poolThunkRVA);
    plan.poolThunkRVA = 0;
    plan.targeting = IncrementalRedirectTargeting::DirectBodyTarget;
  };

  for (IncrementalTextThunkPlanState &plan : plans) {
    if (plan.engagement != IncrementalRedirectEngagement::Installed ||
        plan.bodyRVA == 0 || plan.bodyRVA == plan.redirectRVA)
      continue;

    bool needPool = !isIncrementalAmd64Rel32InRange(
        llvm::COFF::IMAGE_REL_AMD64_REL32, plan.redirectRVA + 1, plan.bodyRVA);
    if (!needPool) {
      releasePoolThunkRVA(plan);
      continue;
    }

    if (!allowPoolThunks) {
      plan.engagement = IncrementalRedirectEngagement::Deferred;
      releasePoolThunkRVA(plan);
      continue;
    }

    IncrementalTextThunkSelection poolThunkSelection =
        chooseIncrementalTextThunkRVA(plan.poolThunkRVA, tailCursor, poolCursor,
                                      poolEndRVA, claimedThunkRVAs,
                                      freedThunkRVAs);
    const SelectedPoolThunkRVA *poolThunkRVA = poolThunkSelection.match(
        [](const PoolThunkUnavailable &) -> const SelectedPoolThunkRVA * {
          return nullptr;
        },
        [](const SelectedPoolThunkRVA &selected)
            -> const SelectedPoolThunkRVA * { return &selected; });
    if (!poolThunkRVA) {
      plan.engagement = IncrementalRedirectEngagement::Deferred;
      releasePoolThunkRVA(plan);
      continue;
    }

    if (poolStart == 0 || poolThunkRVA->rva < poolStart)
      poolStart = poolThunkRVA->rva;
    poolCursor = std::min(poolCursor, poolThunkRVA->rva);
    claimedThunkRVAs.push_back(poolThunkRVA->rva);
    plan.poolThunkRVA = poolThunkRVA->rva;
    plan.targeting = IncrementalRedirectTargeting::PoolThunkTarget;
  }
}

bool isIncrementalAmd64Rel32InRange(uint16_t type, uint64_t sourceRVA,
                                    uint64_t targetRVA) {
  int64_t adjustment = 0;
  switch (type) {
  case llvm::COFF::IMAGE_REL_AMD64_REL32:
    adjustment = 4;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_1:
    adjustment = 5;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_2:
    adjustment = 6;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_3:
    adjustment = 7;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_4:
    adjustment = 8;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_5:
    adjustment = 9;
    break;
  default:
    return true;
  }
  return isInt<32>(int64_t(targetRVA) - int64_t(sourceRVA) - adjustment);
}

StringRef incrementalFallbackReasonToString(IncrementalFallbackReason reason) {
  switch (reason) {
  case IncrementalFallbackReason::None:
    return "None";
  case IncrementalFallbackReason::MissingState:
    return "MissingState";
  case IncrementalFallbackReason::InvalidState:
    return "InvalidState";
  case IncrementalFallbackReason::UnsupportedMachine:
    return "UnsupportedMachine";
  case IncrementalFallbackReason::LtoInput:
    return "LtoInput";
  case IncrementalFallbackReason::TailMergeEnabled:
    return "TailMergeEnabled";
  case IncrementalFallbackReason::ConfigChanged:
    return "ConfigChanged";
  case IncrementalFallbackReason::OutputMismatch:
    return "OutputMismatch";
  case IncrementalFallbackReason::LayoutChanged:
    return "LayoutChanged";
  case IncrementalFallbackReason::SlotOverflow:
    return "SlotOverflow";
  case IncrementalFallbackReason::MergeChunkParticipantChanged:
    return "MergeChunkParticipantChanged";
  case IncrementalFallbackReason::PackedSectionOverflow:
    return "PackedSectionOverflow";
  case IncrementalFallbackReason::Amd64Rel32OutOfRange:
    return "Amd64Rel32OutOfRange";
  }
  llvm_unreachable("unknown incremental fallback reason");
}

void setIncrementalFallback(COFFLinkerContext &ctx,
                            IncrementalFallbackReason reason,
                            const Twine &detail) {
  ctx.pendingIncrementalFallback = std::make_unique<PendingFullImageBuild>(
      PendingFullImageBuild{reason, detail.str()});
}

uint64_t computeIncrementalHardConfigHash(const Configuration &config) {
  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  os << uint32_t(config.machine) << '\n'
     << config.dll << '\n'
     << config.noEntry << '\n'
     << config.align << '\n'
     << config.fileAlign << '\n'
     << config.imageBase << '\n'
     << config.dynamicBase << '\n'
     << config.largeAddressAware << '\n'
     << config.highEntropyVA << '\n'
     << config.guardCF << '\n'
     << config.hotpatchCompat << '\n'
     << config.functionPadMin << '\n'
     << config.manifest << '\n'
     << config.manifestID << '\n'
     << config.manifestUAC << '\n'
     << config.manifestLevel << '\n'
     << config.manifestUIAccess << '\n'
     << config.manifestFile << '\n';
  for (const auto &entry : config.merge)
    os << "merge:" << entry.first << '=' << entry.second << '\n';
  for (const auto &entry : config.section)
    os << "section:" << entry.first << '=' << entry.second << '\n';
  for (const auto &entry : config.sectionOrder)
    os << "sectionorder:" << entry.first << '=' << entry.second << '\n';
  appendSortedStrings(os, config.delayLoads);
  appendManifestInputs(os, config.manifestInput);
  return xxh3_64bits(buffer);
}

uint64_t computeIncrementalSoftConfigHash(const Configuration &config) {
  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  os << config.pdbPath << '\n'
     << config.pdbAltPath << '\n'
     << config.pdbSourcePath << '\n'
     << config.pdbPageSize << '\n'
     << config.lldmapFile << '\n'
     << config.mapFile << '\n'
     << config.timestamp << '\n'
     << config.repro << '\n'
     << unsigned(config.buildIDHash) << '\n';
  for (const std::string &natvis : config.natvisFiles)
    os << natvis << '\n';
  appendSortedStringMap(os, config.namedStreams);
  return xxh3_64bits(buffer);
}

IncrementalCurrentInputs prepareCurrentIncrementalInputs(COFFLinkerContext &ctx) {
  IncrementalCurrentInputs currentInputs;
  currentInputs.hashes.reserve(ctx.objFileInstances.size());
  currentInputs.names.reserve(ctx.objFileInstances.size());
  currentInputs.parentNames.reserve(ctx.objFileInstances.size());
  currentInputs.archiveOffsets.reserve(ctx.objFileInstances.size());
  for (size_t i = 0; i < ctx.objFileInstances.size(); ++i) {
    ObjFile *file = ctx.objFileInstances[i];
    currentInputs.inputIndices[file] = i;
    currentInputs.hashes.push_back(xxh3_64bits(file->mb.getBuffer()));
    currentInputs.names.push_back(file->getName().str());
    currentInputs.parentNames.push_back(file->archiveName.str());
    currentInputs.archiveOffsets.push_back(file->archiveOffset);
  }
  return currentInputs;
}

IncrementalChunkKind classifyIncrementalChunk(const Chunk &chunk) {
  if (isa<IncrementalPaddingChunk>(&chunk))
    return IncrementalChunkKind::Padding;
  if (isa<IncrementalEntryRedirectChunkX64>(&chunk))
    return IncrementalChunkKind::EntryRedirect;
  if (isa<IncrementalLongThunkChunkX64>(&chunk))
    return IncrementalChunkKind::LongThunk;
  if (auto *section = dyn_cast<SectionChunk>(&chunk))
    if (section->file)
      return IncrementalChunkKind::ObjSection;
  return IncrementalChunkKind::Synthetic;
}

std::string getIncrementalChunkKey(const IncrementalInputIndexMap &inputIndices,
                                   const Chunk &chunk) {
  std::string key;
  raw_string_ostream os(key);
  if (auto *padding = dyn_cast<IncrementalPaddingChunk>(&chunk)) {
    os << "pad:" << padding->getSectionName() << ':' << chunk.getOutputCharacteristics()
       << ':' << chunk.getRVA() << ':' << chunk.getSize() << ':'
       << unsigned(padding->getFillByte());
    return os.str();
  }
  if (auto *redirect = dyn_cast<IncrementalEntryRedirectChunkX64>(&chunk)) {
    os << "redirect:" << redirect->getDebugName() << ':' << chunk.getSize();
    return os.str();
  }
  if (auto *thunk = dyn_cast<IncrementalLongThunkChunkX64>(&chunk)) {
    os << "longthunk:" << thunk->getDebugName();
    return os.str();
  }
  if (auto *section = dyn_cast<SectionChunk>(&chunk)) {
    auto it = inputIndices.find(section->file);
    if (it != inputIndices.end())
      os << "obj:" << it->second << ':';
    else
      os << "obj:?:";
    if (section->sym)
      os << "comdat:" << section->sym->getName();
    else
      os << "sec:" << section->getSectionNumber() << ':'
         << section->getSectionName();
    return os.str();
  }
  os << "syn:" << chunk.getDebugName() << ':' << chunk.getOutputCharacteristics()
     << ':' << chunk.getAlignment();
  return os.str();
}

uint64_t computeIncrementalSymbolHash(const SectionChunk &chunk) {
  SmallVector<std::pair<std::string, uint32_t>, 8> symbols;
  uint32_t sectionNumber = chunk.getSectionNumber();
  for (const SymbolRef &ref : chunk.file->getCOFFObj()->symbols()) {
    COFFSymbolRef symbol = chunk.file->getCOFFObj()->getCOFFSymbol(ref);
    if (!symbol.isExternal())
      continue;
    Expected<StringRef> nameOrErr = chunk.file->getCOFFObj()->getSymbolName(symbol);
    if (!nameOrErr)
      continue;
    if (symbol.getSectionNumber() != static_cast<int32_t>(sectionNumber))
      continue;
    symbols.emplace_back(nameOrErr->str(), symbol.getValue());
  }
  llvm::sort(symbols, [](const auto &lhs, const auto &rhs) {
    return lhs < rhs;
  });

  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  for (const auto &entry : symbols)
    os << entry.first << '=' << entry.second << '\n';
  return xxh3_64bits(buffer);
}

static bool validateIncrementalSymbolStates(COFFLinkerContext &ctx,
                                            const IncrementalBaselineData &baseline) {
  std::vector<IncrementalSymbolState> currentSymbols =
      buildIncrementalSymbolStates(ctx, baseline.currentInputs.inputIndices);
  if (currentSymbols.size() != baseline.state.symbols.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "resolved symbol count changed");
    return false;
  }

  for (size_t i = 0; i < currentSymbols.size(); ++i) {
    const IncrementalSymbolState &current = currentSymbols[i];
    const IncrementalSymbolState &old = baseline.state.symbols[i];
    if (current.name != old.name || current.kind != old.kind ||
        current.inputIndex != old.inputIndex || current.value != old.value ||
        current.auxiliaryKey != old.auxiliaryKey) {
      std::string detail;
      raw_string_ostream os(detail);
      os << "symbol winner changed: " << current.name;
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             os.str());
      return false;
    }
  }
  return true;
}

void prepareIncrementalLink(COFFLinkerContext &ctx) {
  installIncrementalCoordinator(ctx, IncrementalCoordinator::makeDisabled());

  if (ctx.config.incrementalRequestPolicy !=
      IncrementalRequestPolicy::AttemptIncrementalReuse)
    return;

  ensureIncrementalStatePath(ctx.config);

  if (ctx.config.machine != AMD64) {
    installIncrementalCoordinator(
        ctx, buildFullImageBuild(ctx,
                                 IncrementalFallbackReason::UnsupportedMachine,
                                 {},
                                 IncrementalBaselineEmission::make<
                                     SkipNextBaseline>()));
    return;
  }

  if (hasBitcodeInputs(ctx)) {
    installIncrementalCoordinator(
        ctx, buildFullImageBuild(ctx, IncrementalFallbackReason::LtoInput, {},
                                 IncrementalBaselineEmission::make<
                                     SkipNextBaseline>()));
    return;
  }

  if (ctx.config.tailMerge) {
    installIncrementalCoordinator(
        ctx, buildFullImageBuild(ctx,
                                 IncrementalFallbackReason::TailMergeEnabled,
                                 {},
                                 IncrementalBaselineEmission::make<
                                     SkipNextBaseline>()));
    return;
  }

  auto installFallback = [&](IncrementalFallbackReason reason,
                             const Twine &detail = {}) {
    installIncrementalCoordinator(
        ctx, buildFullImageBuild(ctx, reason, detail,
                                 IncrementalBaselineEmission::make<
                                     EmitNextBaseline>()));
  };

  if (!sys::fs::exists(ctx.config.incrementalStatePath)) {
    installFallback(IncrementalFallbackReason::MissingState);
    return;
  }

  Expected<IncrementalStateFile> stateOrErr =
      loadIncrementalState(ctx.config.incrementalStatePath);
  if (!stateOrErr) {
    installFallback(IncrementalFallbackReason::InvalidState,
                    toString(stateOrErr.takeError()));
    return;
  }

  uint64_t hardHash = computeIncrementalHardConfigHash(ctx.config);
  uint64_t softHash = computeIncrementalSoftConfigHash(ctx.config);
  if (stateOrErr->machine != ctx.config.machine ||
      stateOrErr->hardConfigHash != hardHash) {
    installFallback(IncrementalFallbackReason::ConfigChanged);
    return;
  }
  if (stateOrErr->outputPath != ctx.config.outputFile) {
    installFallback(IncrementalFallbackReason::OutputMismatch,
                    "incremental state was written for a different output");
    return;
  }
  if (stateOrErr->version < 3 ||
      stateOrErr->layoutMode != IncrementalLayoutMode::Slotted) {
    installFallback(IncrementalFallbackReason::MissingState,
                    "slotted baseline state is missing");
    return;
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> oldImage = MemoryBuffer::getFile(
      ctx.config.outputFile, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!oldImage) {
    installFallback(IncrementalFallbackReason::OutputMismatch);
    return;
  }
  if ((*oldImage)->getBufferSize() != stateOrErr->outputSize ||
      xxh3_64bits((*oldImage)->getBuffer()) != stateOrErr->outputHash) {
    installFallback(IncrementalFallbackReason::OutputMismatch);
    return;
  }

  IncrementalBaselineData baseline;
  baseline.state = std::move(*stateOrErr);
  baseline.oldImage = std::move(*oldImage);
  for (ArchiveFile *file : ctx.archiveFileInstances)
    baseline.replayableArchives.insert(file->getName());

  for (const IncrementalInputState &input : baseline.state.inputs) {
    if (input.parentName.empty())
      continue;

    // Let the current link drive archive extraction. Replaying baseline members
    // here pollutes the graph before we know reuse is valid and makes later
    // extraction validation observe the replay instead of current demand.
    baseline.expectedArchiveMembers.insert(getIncrementalArchiveMemberKey(
        input.parentName, input.archiveOffset, input.name));
  }

  IncrementalPdbReusePolicy pdbReuse =
      baseline.state.softConfigHash == softHash
          ? IncrementalPdbReusePolicy::make<ReusePdbMetadata>()
          : IncrementalPdbReusePolicy::make<RebuildPdbMetadata>();
  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeStateBackedLink(
               std::move(baseline), std::move(pdbReuse),
               IncrementalBaselineEmission::make<EmitNextBaseline>()));
}

void finalizeIncrementalLinkPlan(COFFLinkerContext &ctx) {
  auto loaded = takeStateBackedLink(ctx);
  if (!loaded)
    return;
  clearPendingIncrementalFallback(ctx);

  auto installFullImageBuild =
      [&](std::unique_ptr<FullImageBuild> fullImageBuild) {
        installIncrementalCoordinator(
            ctx, IncrementalCoordinator::makeFullImageBuild(
                     fullImageBuild->fallbackReason,
                     std::move(fullImageBuild->fallbackDetail),
                     std::move(fullImageBuild->baselineEmission)));
      };

  if (hasBitcodeInputs(ctx)) {
    installIncrementalCoordinator(
        ctx, buildFullImageBuild(ctx, IncrementalFallbackReason::LtoInput, {},
                                 IncrementalBaselineEmission::make<
                                     SkipNextBaseline>()));
    return;
  }

  uint64_t resourceHash = computeIncrementalResourceInputHash(ctx);
  if (resourceHash != loaded->baseline.state.resourceInputHash) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "resource or manifest inputs changed");
    installFullImageBuild(consumePendingIncrementalFallback(
        ctx, std::move(loaded->baselineEmission)));
    return;
  }

  loaded->baseline.currentInputs = prepareCurrentIncrementalInputs(ctx);
  loaded->baseline.changedInputs.clear();

  if (loaded->baseline.state.inputs.size() != loaded->baseline.currentInputs.hashes.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "input count changed");
    installFullImageBuild(consumePendingIncrementalFallback(
        ctx, std::move(loaded->baselineEmission)));
    return;
  }

  for (size_t i = 0; i < loaded->baseline.state.inputs.size(); ++i) {
    const IncrementalInputState &input = loaded->baseline.state.inputs[i];
    if (input.name != loaded->baseline.currentInputs.names[i] ||
        input.parentName != loaded->baseline.currentInputs.parentNames[i] ||
        input.archiveOffset != loaded->baseline.currentInputs.archiveOffsets[i]) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "input order changed");
      installFullImageBuild(consumePendingIncrementalFallback(
          ctx, std::move(loaded->baselineEmission)));
      return;
    }
    if (input.contentHash != loaded->baseline.currentInputs.hashes[i])
      loaded->baseline.changedInputs.insert(ctx.objFileInstances[i]);
  }

  if (loaded->baseline.loadedArchiveMembers.size() !=
      loaded->baseline.expectedArchiveMembers.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "archive member extraction changed");
    installFullImageBuild(consumePendingIncrementalFallback(
        ctx, std::move(loaded->baselineEmission)));
    return;
  }
  for (const auto &entry : loaded->baseline.expectedArchiveMembers) {
    if (!loaded->baseline.loadedArchiveMembers.contains(entry.getKey())) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "archive member extraction changed");
      installFullImageBuild(consumePendingIncrementalFallback(
          ctx, std::move(loaded->baselineEmission)));
      return;
    }
  }

  if (loaded->baseline.state.importTopologyHash !=
      computeIncrementalImportTopologyHash(ctx)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "import topology changed");
    installFullImageBuild(consumePendingIncrementalFallback(
        ctx, std::move(loaded->baselineEmission)));
    return;
  }
  if (loaded->baseline.state.exportTopologyHash !=
      computeIncrementalExportTopologyHash(ctx)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "export topology changed");
    installFullImageBuild(consumePendingIncrementalFallback(
        ctx, std::move(loaded->baselineEmission)));
    return;
  }

  if (!validateIncrementalSymbolStates(ctx, loaded->baseline)) {
    installFullImageBuild(consumePendingIncrementalFallback(
        ctx, std::move(loaded->baselineEmission)));
    return;
  }

  if (ctx.config.verbose) {
    Log(ctx) << "incremental: using state " << ctx.config.incrementalStatePath;
    if (!shouldReusePdbMetadata(loaded->pdbReuse))
      Log(ctx) << "incremental: soft-config metadata changed; rebuilding PDB metadata";
  }

  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeLayoutStableLink(
               std::move(loaded->baseline), std::move(loaded->pdbReuse),
               std::move(loaded->baselineEmission)));
}

void finalizeIncrementalLink(COFFLinkerContext &ctx) {
  if (errorCount() != 0 || !shouldEmitIncrementalBaseline(ctx))
    return;

  IncrementalCurrentInputs currentInputs = prepareCurrentIncrementalInputs(ctx);
  const ByteReuseLink *activeReuse = findActiveByteReuseLink(ctx);
  const IncrementalReuseData *reuseData = activeReuse ? &activeReuse->reuse : nullptr;

  IncrementalStateBuildResult buildResult =
      buildIncrementalState(ctx, currentInputs, reuseData);
  if (!shouldEmitNextBaseline(buildResult.baselineEmission))
    return;
  if (Error err =
          writeIncrementalState(ctx.config.incrementalStatePath, buildResult.state))
    Warn(ctx) << "failed to write incremental state: " << toString(std::move(err));
}

void noteIncrementalArchiveMemberLoad(COFFLinkerContext &ctx,
                                      StringRef archiveName,
                                      uint64_t archiveOffset,
                                      StringRef memberName) {
  if (archiveName.empty())
    return;

  IncrementalBaselineData *baseline = findActiveIncrementalBaseline(ctx);
  if (!baseline || !baseline->replayableArchives.contains(archiveName))
    return;
  baseline->loadedArchiveMembers.insert(
      getIncrementalArchiveMemberKey(archiveName, archiveOffset, memberName));
}

} // namespace lld::coff
