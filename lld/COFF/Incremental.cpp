#include "Incremental.h"
#include "COFFLinkerContext.h"
#include "IncrementalRedirects.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
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

std::unique_ptr<IncrementalCoordinator>
IncrementalCoordinator::makePendingFullImageBuild(
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(
      new IncrementalCoordinator(State::make<PendingFullImageBuild>(
          PendingFullImageBuild{std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator>
IncrementalCoordinator::makeFullImageBuild(
    IncrementalFullBuildDecision decision,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(
      new IncrementalCoordinator(State::make<FullImageBuild>(
          FullImageBuild{std::move(decision), std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator>
IncrementalCoordinator::makeStateBackedLink(
    IncrementalBaselineData baseline, IncrementalPdbReusePolicy pdbReuse,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(
      new IncrementalCoordinator(State::make<StateBackedLink>(
          StateBackedLink{std::move(baseline), std::move(pdbReuse),
                          std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator>
IncrementalCoordinator::makeLayoutStableLink(
    IncrementalBaselineData baseline, IncrementalPdbReusePolicy pdbReuse,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(
      new IncrementalCoordinator(State::make<LayoutStableLink>(
          LayoutStableLink{std::move(baseline), std::move(pdbReuse),
                           std::move(baselineEmission)})));
}

std::unique_ptr<IncrementalCoordinator>
IncrementalCoordinator::makeByteReuseLink(
    IncrementalBaselineData baseline, IncrementalReuseData reuse,
    IncrementalPdbReusePolicy pdbReuse,
    IncrementalBaselineEmission baselineEmission) {
  return std::unique_ptr<IncrementalCoordinator>(
      new IncrementalCoordinator(State::make<ByteReuseLink>(
          ByteReuseLink{std::move(baseline), std::move(reuse),
                        std::move(pdbReuse), std::move(baselineEmission)})));
}

namespace {

struct IncrementalStateBuildResult {
  IncrementalBaselineEmission baselineEmission =
      IncrementalBaselineEmission::make<EmitNextBaseline>();
  IncrementalBaselineSnapshot snapshot;
};

static bool
shouldEmitNextBaseline(const IncrementalBaselineEmission &baselineEmission) {
  return baselineEmission.match([](const EmitNextBaseline &) { return true; },
                                [](const SkipNextBaseline &) { return false; });
}

static StringRef
getFullImageBuildCauseLabel(const IncrementalFullBuildCause &cause) {
  return cause.match(
      [](const RebuildForMissingBaseline &) -> StringRef {
        return "MissingState";
      },
      [](const RebuildForRejectedBaseline &) -> StringRef {
        return "InvalidState";
      },
      [](const RebuildForUnsupportedMachine &) -> StringRef {
        return "UnsupportedMachine";
      },
      [](const RebuildForBitcodeInputs &) -> StringRef { return "LtoInput"; },
      [](const RebuildForTailMerging &) -> StringRef {
        return "TailMergeEnabled";
      },
      [](const RebuildForConfigDrift &) -> StringRef {
        return "ConfigChanged";
      },
      [](const RebuildForOutputDrift &) -> StringRef {
        return "OutputMismatch";
      },
      [](const RebuildForLayoutRewrite &) -> StringRef {
        return "LayoutChanged";
      },
      [](const RebuildForSlotCapacity &) -> StringRef {
        return "SlotOverflow";
      },
      [](const RebuildForMergeParticipantDrift &) -> StringRef {
        return "MergeChunkParticipantChanged";
      },
      [](const RebuildForPackedSectionGrowth &) -> StringRef {
        return "PackedSectionOverflow";
      },
      [](const RebuildForRel32RangeOverflow &) -> StringRef {
        return "Amd64Rel32OutOfRange";
      });
}

static void logFullImageBuild(COFFLinkerContext &ctx,
                              const IncrementalFullBuildDecision &decision) {
  if (!ctx.config.verbose)
    return;
  StringRef causeLabel = getFullImageBuildCauseLabel(decision.cause);
  Log(ctx) << "incremental: fallback: " << causeLabel;
  if (StringRef(decision.message) != causeLabel)
    Log(ctx) << "incremental: detail: " << decision.message;
}

static void installIncrementalCoordinatorImpl(
    COFFLinkerContext &ctx,
    std::unique_ptr<IncrementalCoordinator> coordinator) {
  ctx.incremental = std::move(coordinator);
}

static void
preparePendingFullImageBuild(COFFLinkerContext &ctx,
                             IncrementalBaselineEmission baselineEmission) {
  ctx.incremental = IncrementalCoordinator::makePendingFullImageBuild(
      std::move(baselineEmission));
}

static PendingFullImageBuild takePendingFullImageBuild(COFFLinkerContext &ctx) {
  std::unique_ptr<PendingFullImageBuild> pending;
  ctx.incremental->match(
      [&](IncrementalDisabled &) {
        llvm_unreachable("incremental fallback requested without active state");
      },
      [&](PendingFullImageBuild &fullBuild) {
        pending = std::make_unique<PendingFullImageBuild>(
            PendingFullImageBuild{std::move(fullBuild.baselineEmission)});
      },
      [&](FullImageBuild &) {
        llvm_unreachable("incremental fallback requested after final fallback");
      },
      [&](StateBackedLink &) {
        llvm_unreachable(
            "incremental fallback requested before transition setup");
      },
      [&](LayoutStableLink &) {
        llvm_unreachable("incremental fallback requested before layout setup");
      },
      [&](ByteReuseLink &) {
        llvm_unreachable("incremental fallback requested after byte reuse");
      });
  ctx.incremental = IncrementalCoordinator::makeDisabled();
  return std::move(*pending);
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
  std::unique_ptr<IncrementalBaselineEmission> pendingEmission;
  ctx.incremental->match(
      [&](IncrementalDisabled &) {}, [&](PendingFullImageBuild &) {},
      [&](FullImageBuild &) {},
      [&](StateBackedLink &loaded) {
        pendingEmission = std::make_unique<IncrementalBaselineEmission>(
            std::move(loaded.baselineEmission));
        result = std::make_unique<StateBackedLink>(std::move(loaded));
      },
      [&](LayoutStableLink &) {}, [&](ByteReuseLink &) {});
  if (pendingEmission)
    preparePendingFullImageBuild(ctx, std::move(*pendingEmission));
  return result;
}

static std::unique_ptr<LayoutStableLink>
takeLayoutStableLink(COFFLinkerContext &ctx) {
  std::unique_ptr<LayoutStableLink> result;
  std::unique_ptr<IncrementalBaselineEmission> pendingEmission;
  ctx.incremental->match(
      [&](IncrementalDisabled &) {}, [&](PendingFullImageBuild &) {},
      [&](FullImageBuild &) {}, [&](StateBackedLink &) {},
      [&](LayoutStableLink &validated) {
        pendingEmission = std::make_unique<IncrementalBaselineEmission>(
            std::move(validated.baselineEmission));
        result = std::make_unique<LayoutStableLink>(std::move(validated));
      },
      [&](ByteReuseLink &) {});
  if (pendingEmission)
    preparePendingFullImageBuild(ctx, std::move(*pendingEmission));
  return result;
}

static ByteReuseLink *findActiveByteReuseLinkImpl(COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](IncrementalDisabled &) -> ByteReuseLink * { return nullptr; },
      [&](PendingFullImageBuild &) -> ByteReuseLink * { return nullptr; },
      [&](FullImageBuild &) -> ByteReuseLink * { return nullptr; },
      [&](StateBackedLink &) -> ByteReuseLink * { return nullptr; },
      [&](LayoutStableLink &) -> ByteReuseLink * { return nullptr; },
      [&](ByteReuseLink &reuse) -> ByteReuseLink * { return &reuse; });
}

static IncrementalBaselineData *
findActiveIncrementalBaselineImpl(COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](IncrementalDisabled &) -> IncrementalBaselineData * {
        return nullptr;
      },
      [&](PendingFullImageBuild &) -> IncrementalBaselineData * {
        return nullptr;
      },
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

static bool shouldEmitIncrementalBaselineImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) { return false; },
      [&](const PendingFullImageBuild &) { return false; },
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
                             const IncrementalReuseData *reuseData,
                             llvm::ArrayRef<IncrementalSectionState> sectionStates,
                             IncrementalBaselineSnapshot &snapshot) {
  enum class BaselineEmissionChoice : uint8_t {
    Emit = 1,
    Skip = 2,
  };

  BaselineEmissionChoice baselineEmission = BaselineEmissionChoice::Emit;
  SmallVector<OutputSection *, 16> activeSections;
  for (OutputSection *section : ctx.outputSections)
    if (section->getVirtualSize() != 0)
      activeSections.push_back(section);

  DenseMap<const OutputSection *, uint64_t> maxSectionEndRVABySection;
  for (size_t i = 0; i < activeSections.size(); ++i)
    maxSectionEndRVABySection[activeSections[i]] =
        i + 1 < activeSections.size() ? activeSections[i + 1]->getRVA()
                                      : snapshot.sizeOfImage;

  snapshot.sections.reserve(sectionStates.size());
  for (size_t sectionIndex = 0; sectionIndex < ctx.outputSections.size();
       ++sectionIndex) {
    OutputSection *section = ctx.outputSections[sectionIndex];
    const IncrementalSectionState &sectionState = sectionStates[sectionIndex];
    IncrementalSectionLayoutKind layoutKind = classifyIncrementalSection(
        section->name, section->header.Characteristics);

    if (section->getVirtualSize() == 0 ||
        layoutKind == IncrementalSectionLayoutKind::ExactSectionLayout) {
      snapshot.sections.push_back(
          IncrementalSectionSnapshot::make<ExactSectionSnapshot>(
              ExactSectionSnapshot{sectionState}));
      continue;
    }

    if (isIncrementalFreeSlotLayout(layoutKind)) {
      auto suppressStateWrite = [&](const Twine &detail) {
        baselineEmission = BaselineEmissionChoice::Skip;
        if (ctx.config.verbose)
          Log(ctx) << "incremental: not writing state: " << detail;
      };
      auto isPersistedSlotChunk = [&](Chunk *chunk) {
        return isIncrementalPersistedSlotChunk(layoutKind, *chunk);
      };

      IncrementalSlotSectionSnapshot slotSection;
      slotSection.section = sectionState;
      slotSection.maxSectionEndRVA = maxSectionEndRVABySection.lookup(section);
      slotSection.activeEndRVA = section->getRVA() + section->getVirtualSize();
      if (layoutKind == IncrementalSectionLayoutKind::TextFreeSlots) {
        slotSection.activeEndRVA = section->getRVA();
        for (Chunk *chunk : section->chunks) {
          if (isa<IncrementalLongThunkChunkX64>(chunk))
            continue;
          slotSection.activeEndRVA = std::max(slotSection.activeEndRVA,
                                              chunk->getRVA() + chunk->getSize());
        }
      }

      for (size_t chunkIndex = 0; chunkIndex < section->chunks.size();
           ++chunkIndex) {
        Chunk *chunk = section->chunks[chunkIndex];
        if (!isPersistedSlotChunk(chunk))
          continue;
        std::string key = getIncrementalChunkKey(inputIndices, *chunk);
        bool isPadding = isa<IncrementalPaddingChunk>(chunk);

        uint64_t slotEnd = slotSection.activeEndRVA;
        for (size_t nextIndex = chunkIndex + 1;
             nextIndex < section->chunks.size(); ++nextIndex) {
          Chunk *nextChunk = section->chunks[nextIndex];
          if (!isPersistedSlotChunk(nextChunk))
            continue;
          slotEnd = nextChunk->getRVA();
          break;
        }
        if (slotEnd < chunk->getRVA() ||
            slotEnd - chunk->getRVA() < chunk->getSize()) {
          suppressStateWrite(
              "slot table builder found an invalid persisted range");
          return IncrementalBaselineEmission::make<SkipNextBaseline>();
        }

        IncrementalPreservedSlotState slotState;
        slotState.startRVA = chunk->getRVA();
        slotState.capacity = slotEnd - chunk->getRVA();
        slotState.committedSize = chunk->getSize();
        slotState.minAlignment = chunk->getAlignment();
        slotState.fillByte =
            isPadding ? cast<IncrementalPaddingChunk>(chunk)->getFillByte()
                      : getIncrementalFillByte(layoutKind);
        if (isPadding) {
          slotSection.slots.push_back(
              IncrementalPreservedSlot::make<FreeSlotRecord>(
                  FreeSlotRecord{slotState}));
          continue;
        }

        slotSection.slots.push_back(
            IncrementalPreservedSlot::make<OccupiedSlotRecord>(
                OccupiedSlotRecord{slotState, key}));
        slotSection.preservedChunks.push_back(
            ExistingSlotChunkPlacement{key, chunk->getRVA(), chunk->getSize(),
                                       chunk->getAlignment()});
      }

      if (layoutKind == IncrementalSectionLayoutKind::TextFreeSlots) {
        TextSlotSectionSnapshot text;
        text.slotSection = std::move(slotSection);
        if (reuseData) {
          text.redirects = reuseData->currentTextRedirects;
          text.thunkPool = reuseData->currentTextThunkPool;
        }
        snapshot.sections.push_back(
            IncrementalSectionSnapshot::make<TextSlotSectionSnapshot>(
                std::move(text)));
      } else if (layoutKind ==
                 IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots) {
        snapshot.sections.push_back(
            IncrementalSectionSnapshot::make<ReadOnlySlotSectionSnapshot>(
                ReadOnlySlotSectionSnapshot{std::move(slotSection)}));
      } else {
        snapshot.sections.push_back(
            IncrementalSectionSnapshot::make<WritableSlotSectionSnapshot>(
                WritableSlotSectionSnapshot{std::move(slotSection)}));
      }
      continue;
    }

    if (!isIncrementalPackedLayout(layoutKind)) {
      snapshot.sections.push_back(
          IncrementalSectionSnapshot::make<ExactSectionSnapshot>(
              ExactSectionSnapshot{sectionState}));
      continue;
    }

    IncrementalPackedPrefixSectionSnapshot packedSection;
    packedSection.section = sectionState;
    packedSection.activePrefixSize =
        section->getRVA() + section->getVirtualSize() - section->getRVA();
    packedSection.reserveSize =
        maxSectionEndRVABySection.lookup(section) -
        (section->getRVA() + section->getVirtualSize());
    for (Chunk *chunk : section->chunks) {
      if (chunk->getSize() == 0)
        continue;
      std::string key = getIncrementalChunkKey(inputIndices, *chunk);
      packedSection.members.push_back(
          PackedPrefixChunkPlacement{key, chunk->getRVA(), chunk->getSize(),
                                     chunk->getAlignment()});
    }

    if (layoutKind == IncrementalSectionLayoutKind::PackedPDataPrefix)
      snapshot.sections.push_back(
          IncrementalSectionSnapshot::make<PDataPackedPrefixSectionSnapshot>(
              PDataPackedPrefixSectionSnapshot{std::move(packedSection)}));
    else
      snapshot.sections.push_back(
          IncrementalSectionSnapshot::make<XDataPackedPrefixSectionSnapshot>(
              XDataPackedPrefixSectionSnapshot{std::move(packedSection)}));
  }

  if (baselineEmission == BaselineEmissionChoice::Skip)
    return IncrementalBaselineEmission::make<SkipNextBaseline>();
  return IncrementalBaselineEmission::make<EmitNextBaseline>();
}

static std::string
describeIncrementalResolvedSymbol(const IncrementalResolvedSymbolSnapshot &state) {
  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  state.match(
      [&](const RegularResolvedSymbol &regular) {
        os << regular.name << '\n' << "regular\n";
        regular.owner.match(
            [&](const PersistedInputOwner &owner) {
              os << "input:" << owner.inputIndex << '\n';
            },
            [&](const NoPersistedInputOwner &) { os << "input:none\n"; });
        os << regular.value << '\n';
        regular.chunk.match(
            [&](const PersistedChunkReference &chunk) {
              os << "chunk:" << chunk.chunkKey << '\n';
            },
            [&](const NoPersistedChunkReference &) { os << "chunk:none\n"; });
      },
      [&](const CommonResolvedSymbol &common) {
        os << common.name << '\n' << "common\n";
        common.owner.match(
            [&](const PersistedInputOwner &owner) {
              os << "input:" << owner.inputIndex << '\n';
            },
            [&](const NoPersistedInputOwner &) { os << "input:none\n"; });
        os << common.size << '\n' << common.alignment << '\n';
      },
      [&](const ImportDataResolvedSymbol &importData) {
        os << importData.name << '\n' << "importdata\n" << importData.ordinal
           << '\n' << importData.dllName << '\n' << importData.externalName
           << '\n' << importData.typeInfo << '\n';
      },
      [&](const ImportThunkResolvedSymbol &importThunk) {
        os << importThunk.name << '\n' << "importthunk\n"
           << importThunk.wrappedSymbolName << '\n';
      },
      [&](const LocalImportResolvedSymbol &localImport) {
        os << localImport.name << '\n' << "localimport\n";
        localImport.chunk.match(
            [&](const PersistedChunkReference &chunk) {
              os << "chunk:" << chunk.chunkKey << '\n';
            },
            [&](const NoPersistedChunkReference &) { os << "chunk:none\n"; });
      },
      [&](const AbsoluteResolvedSymbol &absolute) {
        os << absolute.name << '\n' << "absolute\n" << absolute.value << '\n';
      },
      [&](const SyntheticResolvedSymbol &synthetic) {
        os << synthetic.name << '\n' << "synthetic\n";
        synthetic.chunk.match(
            [&](const PersistedChunkReference &chunk) {
              os << "chunk:" << chunk.chunkKey << '\n';
            },
            [&](const NoPersistedChunkReference &) { os << "chunk:none\n"; });
      });
  return std::string(buffer);
}

static std::vector<IncrementalResolvedSymbolSnapshot>
buildIncrementalSymbolStates(COFFLinkerContext &ctx,
                             const IncrementalInputIndexMap &inputIndices) {
  std::vector<IncrementalResolvedSymbolSnapshot> states;
  std::vector<std::pair<std::string, size_t>> order;
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    symtab.forEachSymbol([&](Symbol *sym) {
      auto *def = dyn_cast<Defined>(sym);
      if (!def)
        return;
      if (auto *coff = dyn_cast<DefinedCOFF>(sym))
        if (!coff->getCOFFSymbol().isExternal())
          return;

      if (auto *reg = dyn_cast<DefinedRegular>(sym)) {
        IncrementalPersistedInputOwner owner = [&]() {
          if (auto *file = dyn_cast<ObjFile>(reg->getFile()))
            if (auto it = inputIndices.find(file); it != inputIndices.end())
              return IncrementalPersistedInputOwner::make<PersistedInputOwner>(
                  PersistedInputOwner{it->second});
          return IncrementalPersistedInputOwner::make<NoPersistedInputOwner>();
        }();
        IncrementalPersistedChunkReference chunkRef = [&]() {
          if (SectionChunk *chunk = reg->getChunk())
            return IncrementalPersistedChunkReference::make<PersistedChunkReference>(
                PersistedChunkReference{
                    getIncrementalChunkKey(inputIndices, *chunk)});
          return IncrementalPersistedChunkReference::make<
              NoPersistedChunkReference>();
        }();
        RegularResolvedSymbol state{sym->getName().str(), std::move(owner),
                                    reg->getValue(), std::move(chunkRef)};
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<RegularResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *common = dyn_cast<DefinedCommon>(sym)) {
        IncrementalPersistedInputOwner owner = [&]() {
          if (auto *file = dyn_cast<ObjFile>(common->getFile()))
            if (auto it = inputIndices.find(file); it != inputIndices.end())
              return IncrementalPersistedInputOwner::make<PersistedInputOwner>(
                  PersistedInputOwner{it->second});
          return IncrementalPersistedInputOwner::make<NoPersistedInputOwner>();
        }();
        CommonResolvedSymbol state{sym->getName().str(), std::move(owner),
                                   common->getChunk()->getSize(),
                                   common->getChunk()->getAlignment()};
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<CommonResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *imp = dyn_cast<DefinedImportData>(sym)) {
        ImportDataResolvedSymbol state;
        state.name = sym->getName().str();
        state.ordinal = imp->getOrdinal();
        state.dllName = imp->getDLLName().str();
        state.externalName = imp->getExternalName().str();
        state.typeInfo = imp->file->hdr->TypeInfo;
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<ImportDataResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *thunk = dyn_cast<DefinedImportThunk>(sym)) {
        ImportThunkResolvedSymbol state;
        state.name = sym->getName().str();
        state.wrappedSymbolName = thunk->wrappedSym->getName().str();
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<ImportThunkResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *localImport = dyn_cast<DefinedLocalImport>(sym)) {
        IncrementalPersistedChunkReference chunkRef = [&]() {
          if (Chunk *chunk = localImport->getChunk())
            return IncrementalPersistedChunkReference::make<
                PersistedChunkReference>(
                PersistedChunkReference{chunk->getDebugName().str()});
          return IncrementalPersistedChunkReference::make<
              NoPersistedChunkReference>();
        }();
        LocalImportResolvedSymbol state{sym->getName().str(),
                                        std::move(chunkRef)};
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<LocalImportResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *absolute = dyn_cast<DefinedAbsolute>(sym)) {
        AbsoluteResolvedSymbol state;
        state.name = sym->getName().str();
        state.value = absolute->getVA();
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<AbsoluteResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *synthetic = dyn_cast<DefinedSynthetic>(sym)) {
        IncrementalPersistedChunkReference chunkRef = [&]() {
          if (Chunk *chunk = synthetic->getChunk())
            return IncrementalPersistedChunkReference::make<
                PersistedChunkReference>(
                PersistedChunkReference{chunk->getDebugName().str()});
          return IncrementalPersistedChunkReference::make<
              NoPersistedChunkReference>();
        }();
        SyntheticResolvedSymbol state{sym->getName().str(),
                                      std::move(chunkRef)};
        states.push_back(
            IncrementalResolvedSymbolSnapshot::make<SyntheticResolvedSymbol>(
                std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else {
        return;
      }
    });
  });

  llvm::sort(order, llvm::less_first());
  std::vector<IncrementalResolvedSymbolSnapshot> sortedStates;
  sortedStates.reserve(states.size());
  for (const auto &[description, index] : order) {
    (void)description;
    sortedStates.push_back(std::move(states[index]));
  }
  return sortedStates;
}

static IncrementalStateBuildResult
buildIncrementalState(COFFLinkerContext &ctx,
                      const IncrementalCurrentInputs &currentInputs,
                      const IncrementalReuseData *reuseData) {
  IncrementalBaselineSnapshot snapshot;
  snapshot.machine = ctx.config.machine;
  snapshot.outputPath = ctx.config.outputFile;
  snapshot.hardConfigHash = computeIncrementalHardConfigHash(ctx.config);
  snapshot.softConfigHash = computeIncrementalSoftConfigHash(ctx.config);
  snapshot.importTopologyHash = computeIncrementalImportTopologyHash(ctx);
  snapshot.exportTopologyHash = computeIncrementalExportTopologyHash(ctx);
  snapshot.resourceInputHash = computeIncrementalResourceInputHash(ctx);

  ErrorOr<std::unique_ptr<MemoryBuffer>> output =
      MemoryBuffer::getFile(ctx.config.outputFile, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (!output) {
    report_fatal_error(createFileError(ctx.config.outputFile,
                                       errorCodeToError(output.getError())));
  }
  StringRef outputData = (*output)->getBuffer();
  snapshot.outputSize = outputData.size();
  snapshot.outputHash = xxh3_64bits(outputData);

  if (outputData.size() >= sizeof(dos_header)) {
    const uint8_t *buf = reinterpret_cast<const uint8_t *>(outputData.data());
    const dos_header *dos = reinterpret_cast<const dos_header *>(buf);
    uint32_t peOff = dos->AddressOfNewExeHeader;
    uint32_t headerOff =
        peOff + sizeof(llvm::COFF::PEMagic) + sizeof(coff_file_header);
    if (ctx.config.is64() &&
        outputData.size() >= headerOff + sizeof(pe32plus_header)) {
      const pe32plus_header *pe =
          reinterpret_cast<const pe32plus_header *>(buf + headerOff);
      snapshot.sizeOfHeaders = pe->SizeOfHeaders;
      snapshot.sizeOfImage = pe->SizeOfImage;
    } else if (outputData.size() >= headerOff + sizeof(pe32_header)) {
      const pe32_header *pe =
          reinterpret_cast<const pe32_header *>(buf + headerOff);
      snapshot.sizeOfHeaders = pe->SizeOfHeaders;
      snapshot.sizeOfImage = pe->SizeOfImage;
    }
  }

  snapshot.inputs.reserve(ctx.objFileInstances.size());
  for (size_t i = 0; i < ctx.objFileInstances.size(); ++i) {
    IncrementalInputState input;
    input.name = currentInputs.names[i];
    input.parentName = currentInputs.parentNames[i];
    input.archiveOffset = currentInputs.archiveOffsets[i];
    input.contentHash = currentInputs.hashes[i];
    input.size = ctx.objFileInstances[i]->mb.getBufferSize();
    snapshot.inputs.push_back(std::move(input));
  }

  SmallVector<IncrementalSectionState, 16> sectionStates;
  for (OutputSection *section : ctx.outputSections) {
    IncrementalSectionState sectionState;
    sectionState.name = section->name.str();
    sectionState.characteristics = section->header.Characteristics;
    sectionState.rva = section->getRVA();
    sectionState.fileOffset = section->getFileOff();
    sectionState.virtualSize = section->getVirtualSize();
    sectionState.rawSize = section->getRawSize();
    sectionState.firstChunk = snapshot.chunks.size();
    sectionState.chunkCount = section->chunks.size();
    sectionStates.push_back(sectionState);

    uint64_t sectionEnd = section->getRVA() + section->getVirtualSize();
    for (size_t i = 0; i < section->chunks.size(); ++i) {
      Chunk *chunk = section->chunks[i];
      IncrementalChunkState chunkState;
      chunkState.key =
          getIncrementalChunkKey(currentInputs.inputIndices, *chunk);
      chunkState.sectionIndex = sectionStates.size() - 1;
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
        ObjSectionChunkSnapshot objChunk;
        objChunk.chunk = std::move(chunkState);
        auto it = currentInputs.inputIndices.find(sec->file);
        if (it != currentInputs.inputIndices.end())
          objChunk.inputIndex = it->second;
        objChunk.sectionNumber = sec->getSectionNumber();
        objChunk.contentHash = xxh3_64bits(sec->getContents());
        objChunk.symbolHash = computeIncrementalSymbolHash(*sec);
        snapshot.chunks.push_back(
            IncrementalChunkSnapshot::make<ObjSectionChunkSnapshot>(
                std::move(objChunk)));
      } else if (isa<IncrementalPaddingChunk>(chunk)) {
        snapshot.chunks.push_back(
            IncrementalChunkSnapshot::make<PaddingChunkSnapshot>(
                PaddingChunkSnapshot{std::move(chunkState)}));
      } else if (isa<IncrementalEntryRedirectChunkX64>(chunk)) {
        snapshot.chunks.push_back(
            IncrementalChunkSnapshot::make<EntryRedirectChunkSnapshot>(
                EntryRedirectChunkSnapshot{std::move(chunkState)}));
      } else if (isa<IncrementalLongThunkChunkX64>(chunk)) {
        snapshot.chunks.push_back(
            IncrementalChunkSnapshot::make<LongThunkChunkSnapshot>(
                LongThunkChunkSnapshot{std::move(chunkState)}));
      } else {
        snapshot.chunks.push_back(
            IncrementalChunkSnapshot::make<SyntheticChunkSnapshot>(
                SyntheticChunkSnapshot{std::move(chunkState)}));
      }
    }
  }

  IncrementalBaselineEmission baselineEmission =
      buildIncrementalLayoutTables(ctx, currentInputs.inputIndices, reuseData,
                                   sectionStates, snapshot);
  snapshot.symbols =
      buildIncrementalSymbolStates(ctx, currentInputs.inputIndices);
  return IncrementalStateBuildResult{std::move(baselineEmission),
                                     std::move(snapshot)};
}

} // namespace

void installIncrementalCoordinator(
    COFFLinkerContext &ctx,
    std::unique_ptr<IncrementalCoordinator> coordinator) {
  installIncrementalCoordinatorImpl(ctx, std::move(coordinator));
}

void installIncrementalFullImageBuild(
    COFFLinkerContext &ctx, IncrementalFullBuildDecision decision,
    IncrementalBaselineEmission baselineEmission) {
  logFullImageBuild(ctx, decision);
  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeFullImageBuild(
               std::move(decision), std::move(baselineEmission)));
}

void installPendingIncrementalFullImageBuild(
    COFFLinkerContext &ctx, IncrementalFullBuildDecision decision) {
  PendingFullImageBuild pending = takePendingFullImageBuild(ctx);
  installIncrementalFullImageBuild(ctx, std::move(decision),
                                   std::move(pending.baselineEmission));
}

void installPendingIncrementalFullImageBuild(
    COFFLinkerContext &ctx, IncrementalFullBuildDecision decision,
    IncrementalBaselineEmission baselineEmission) {
  takePendingFullImageBuild(ctx);
  installIncrementalFullImageBuild(ctx, std::move(decision),
                                   std::move(baselineEmission));
}

IncrementalSectionLayoutKind
classifyIncrementalSection(StringRef name, uint32_t characteristics) {
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

IncrementalFreeSlotSelection
findBestFitIncrementalFreeSlot(ArrayRef<IncrementalPreservedSlot> slots,
                               uint64_t size, uint32_t alignment) {
  size_t bestIndex = 0;
  bool found = false;
  for (size_t i = 0; i < slots.size(); ++i) {
    const IncrementalPreservedSlotState &slot =
        getIncrementalPreservedSlotState(slots[i]);
    if (slot.capacity < size || slot.startRVA % alignment != 0)
      continue;
    const IncrementalPreservedSlotState &bestSlot =
        getIncrementalPreservedSlotState(slots[bestIndex]);
    if (!found || slot.capacity < bestSlot.capacity ||
        (slot.capacity == bestSlot.capacity &&
         slot.startRVA < bestSlot.startRVA)) {
      bestIndex = i;
      found = true;
    }
  }
  if (!found)
    return IncrementalFreeSlotSelection::make<NoFreeSlotFit>();
  return IncrementalFreeSlotSelection::make<SelectedFreeSlot>(
      SelectedFreeSlot{bestIndex});
}

IncrementalTailReserveSelection
allocateIncrementalTailReserve(uint64_t tailCursor, uint64_t maxSectionEndRVA,
                               uint64_t size, uint32_t alignment) {
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

IncrementalCurrentInputs
prepareCurrentIncrementalInputs(COFFLinkerContext &ctx) {
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
    os << "pad:" << padding->getSectionName() << ':'
       << chunk.getOutputCharacteristics() << ':' << chunk.getRVA() << ':'
       << chunk.getSize() << ':' << unsigned(padding->getFillByte());
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
  os << "syn:" << chunk.getDebugName() << ':'
     << chunk.getOutputCharacteristics() << ':' << chunk.getAlignment();
  return os.str();
}

uint64_t computeIncrementalSymbolHash(const SectionChunk &chunk) {
  SmallVector<std::pair<std::string, uint32_t>, 8> symbols;
  uint32_t sectionNumber = chunk.getSectionNumber();
  for (const SymbolRef &ref : chunk.file->getCOFFObj()->symbols()) {
    COFFSymbolRef symbol = chunk.file->getCOFFObj()->getCOFFSymbol(ref);
    if (!symbol.isExternal())
      continue;
    Expected<StringRef> nameOrErr =
        chunk.file->getCOFFObj()->getSymbolName(symbol);
    if (!nameOrErr)
      continue;
    if (symbol.getSectionNumber() != static_cast<int32_t>(sectionNumber))
      continue;
    symbols.emplace_back(nameOrErr->str(), symbol.getValue());
  }
  llvm::sort(symbols,
             [](const auto &lhs, const auto &rhs) { return lhs < rhs; });

  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  for (const auto &entry : symbols)
    os << entry.first << '=' << entry.second << '\n';
  return xxh3_64bits(buffer);
}

static bool
validateIncrementalSymbolStates(COFFLinkerContext &ctx,
                                const IncrementalBaselineData &baseline) {
  std::vector<IncrementalResolvedSymbolSnapshot> currentSymbols =
      buildIncrementalSymbolStates(ctx, baseline.currentInputs.inputIndices);
  if (currentSymbols.size() != baseline.snapshot.symbols.size()) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForLayoutRewrite("resolved symbol count changed"));
    return false;
  }

  for (size_t i = 0; i < currentSymbols.size(); ++i) {
    const IncrementalResolvedSymbolSnapshot &current = currentSymbols[i];
    const IncrementalResolvedSymbolSnapshot &old = baseline.snapshot.symbols[i];
    if (describeIncrementalResolvedSymbol(current) !=
        describeIncrementalResolvedSymbol(old)) {
      std::string detail;
      raw_string_ostream os(detail);
      std::string symbolName = current.match(
          [](const RegularResolvedSymbol &symbol) { return symbol.name; },
          [](const CommonResolvedSymbol &symbol) { return symbol.name; },
          [](const ImportDataResolvedSymbol &symbol) { return symbol.name; },
          [](const ImportThunkResolvedSymbol &symbol) { return symbol.name; },
          [](const LocalImportResolvedSymbol &symbol) { return symbol.name; },
          [](const AbsoluteResolvedSymbol &symbol) { return symbol.name; },
          [](const SyntheticResolvedSymbol &symbol) { return symbol.name; });
      os << "symbol winner changed: " << symbolName;
      installPendingIncrementalFullImageBuild(
          ctx, rebuildForLayoutRewrite(os.str()));
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
    installIncrementalFullImageBuild(
        ctx, rebuildForUnsupportedMachine(),
        IncrementalBaselineEmission::make<SkipNextBaseline>());
    return;
  }

  if (hasBitcodeInputs(ctx)) {
    installIncrementalFullImageBuild(
        ctx, rebuildForBitcodeInputs(),
        IncrementalBaselineEmission::make<SkipNextBaseline>());
    return;
  }

  if (ctx.config.tailMerge) {
    installIncrementalFullImageBuild(
        ctx, rebuildForTailMerging(),
        IncrementalBaselineEmission::make<SkipNextBaseline>());
    return;
  }

  auto installFallback = [&](IncrementalFullBuildDecision decision) {
    installIncrementalFullImageBuild(
        ctx, std::move(decision),
        IncrementalBaselineEmission::make<EmitNextBaseline>());
  };

  if (!sys::fs::exists(ctx.config.incrementalStatePath)) {
    installFallback(rebuildForMissingBaseline());
    return;
  }

  Expected<IncrementalBaselineSnapshot> stateOrErr =
      loadIncrementalState(ctx.config.incrementalStatePath);
  if (!stateOrErr) {
    installFallback(
        rebuildForRejectedBaseline(toString(stateOrErr.takeError())));
    return;
  }

  uint64_t hardHash = computeIncrementalHardConfigHash(ctx.config);
  uint64_t softHash = computeIncrementalSoftConfigHash(ctx.config);
  if (stateOrErr->machine != ctx.config.machine ||
      stateOrErr->hardConfigHash != hardHash) {
    installFallback(rebuildForConfigDrift());
    return;
  }
  if (stateOrErr->outputPath != ctx.config.outputFile) {
    installFallback(rebuildForOutputDrift(
        "incremental state was written for a different output"));
    return;
  }
  ErrorOr<std::unique_ptr<MemoryBuffer>> oldImage =
      MemoryBuffer::getFile(ctx.config.outputFile, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (!oldImage) {
    installFallback(rebuildForOutputDrift());
    return;
  }
  if ((*oldImage)->getBufferSize() != stateOrErr->outputSize ||
      xxh3_64bits((*oldImage)->getBuffer()) != stateOrErr->outputHash) {
    installFallback(rebuildForOutputDrift());
    return;
  }

  IncrementalBaselineData baseline;
  baseline.snapshot = std::move(*stateOrErr);
  baseline.oldImage = std::move(*oldImage);
  for (ArchiveFile *file : ctx.archiveFileInstances)
    baseline.replayableArchives.insert(file->getName());

  for (const IncrementalInputState &input : baseline.snapshot.inputs) {
    if (input.parentName.empty())
      continue;

    // Let the current link drive archive extraction. Replaying baseline members
    // here pollutes the graph before we know reuse is valid and makes later
    // extraction validation observe the replay instead of current demand.
    baseline.expectedArchiveMembers.insert(getIncrementalArchiveMemberKey(
        input.parentName, input.archiveOffset, input.name));
  }

  IncrementalPdbReusePolicy pdbReuse =
      baseline.snapshot.softConfigHash == softHash
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

  if (hasBitcodeInputs(ctx)) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForBitcodeInputs(),
        IncrementalBaselineEmission::make<SkipNextBaseline>());
    return;
  }

  uint64_t resourceHash = computeIncrementalResourceInputHash(ctx);
  if (resourceHash != loaded->baseline.snapshot.resourceInputHash) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForLayoutRewrite("resource or manifest inputs changed"));
    return;
  }

  loaded->baseline.currentInputs = prepareCurrentIncrementalInputs(ctx);
  loaded->baseline.changedInputs.clear();

  if (loaded->baseline.snapshot.inputs.size() !=
      loaded->baseline.currentInputs.hashes.size()) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForLayoutRewrite("input count changed"));
    return;
  }

  for (size_t i = 0; i < loaded->baseline.snapshot.inputs.size(); ++i) {
    const IncrementalInputState &input = loaded->baseline.snapshot.inputs[i];
    if (input.name != loaded->baseline.currentInputs.names[i] ||
        input.parentName != loaded->baseline.currentInputs.parentNames[i] ||
        input.archiveOffset !=
            loaded->baseline.currentInputs.archiveOffsets[i]) {
      installPendingIncrementalFullImageBuild(
          ctx, rebuildForLayoutRewrite("input order changed"));
      return;
    }
    if (input.contentHash != loaded->baseline.currentInputs.hashes[i])
      loaded->baseline.changedInputs.insert(ctx.objFileInstances[i]);
  }

  if (loaded->baseline.loadedArchiveMembers.size() !=
      loaded->baseline.expectedArchiveMembers.size()) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForLayoutRewrite("archive member extraction changed"));
    return;
  }
  for (const auto &entry : loaded->baseline.expectedArchiveMembers) {
    if (!loaded->baseline.loadedArchiveMembers.contains(entry.getKey())) {
      installPendingIncrementalFullImageBuild(
          ctx, rebuildForLayoutRewrite("archive member extraction changed"));
      return;
    }
  }

  if (loaded->baseline.snapshot.importTopologyHash !=
      computeIncrementalImportTopologyHash(ctx)) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForLayoutRewrite("import topology changed"));
    return;
  }
  if (loaded->baseline.snapshot.exportTopologyHash !=
      computeIncrementalExportTopologyHash(ctx)) {
    installPendingIncrementalFullImageBuild(
        ctx, rebuildForLayoutRewrite("export topology changed"));
    return;
  }

  if (!validateIncrementalSymbolStates(ctx, loaded->baseline)) {
    return;
  }

  if (ctx.config.verbose) {
    Log(ctx) << "incremental: using state " << ctx.config.incrementalStatePath;
    loaded->pdbReuse.match([&](const ReusePdbMetadata &) {},
                           [&](const RebuildPdbMetadata &) {
                             Log(ctx) << "incremental: soft-config metadata "
                                         "changed; rebuilding PDB metadata";
                           });
  }

  PendingFullImageBuild pending = takePendingFullImageBuild(ctx);
  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeLayoutStableLink(
               std::move(loaded->baseline), std::move(loaded->pdbReuse),
               std::move(pending.baselineEmission)));
}

void finalizeIncrementalLink(COFFLinkerContext &ctx) {
  if (errorCount() != 0 || !shouldEmitIncrementalBaselineImpl(ctx))
    return;

  IncrementalCurrentInputs currentInputs = prepareCurrentIncrementalInputs(ctx);
  const ByteReuseLink *activeReuse = findActiveByteReuseLinkImpl(ctx);
  const IncrementalReuseData *reuseData =
      activeReuse ? &activeReuse->reuse : nullptr;

  IncrementalStateBuildResult buildResult =
      buildIncrementalState(ctx, currentInputs, reuseData);
  if (!shouldEmitNextBaseline(buildResult.baselineEmission))
    return;
  if (Error err = writeIncrementalState(ctx.config.incrementalStatePath,
                                        buildResult.snapshot))
    Warn(ctx) << "failed to write incremental state: "
              << toString(std::move(err));
}

void noteIncrementalArchiveMemberLoad(COFFLinkerContext &ctx,
                                      StringRef archiveName,
                                      uint64_t archiveOffset,
                                      StringRef memberName) {
  if (archiveName.empty())
    return;

  IncrementalBaselineData *baseline = findActiveIncrementalBaselineImpl(ctx);
  if (!baseline || !baseline->replayableArchives.contains(archiveName))
    return;
  baseline->loadedArchiveMembers.insert(
      getIncrementalArchiveMemberKey(archiveName, archiveOffset, memberName));
}

} // namespace lld::coff
