#include "Incremental.h"
#include "COFFLinkerContext.h"
#include "IncrementalRedirects.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
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

struct IncrementalSnapshotCounters {
  uint64_t slotCount = 0;
  uint64_t placementCount = 0;
};

static IncrementalSnapshotCounters
summarizeIncrementalSnapshot(const IncrementalBaselineSnapshot &snapshot) {
  IncrementalSnapshotCounters counters;
  for (const IncrementalSectionSnapshot &section : snapshot.sections) {
    matchIncrementalSlotSectionSnapshot(
        section,
        [&](const IncrementalSlotSectionSnapshot &slotSection) {
          counters.slotCount += slotSection.slots.size();
          counters.placementCount += slotSection.preservedChunks.size();
        },
        [&]() {});
    matchIncrementalPackedPrefixSectionSnapshot(
        section,
        [&](const IncrementalPackedPrefixSectionSnapshot &packedSection) {
          counters.placementCount += packedSection.members.size();
        },
        [&]() {});
  }
  return counters;
}

static void logIncrementalSnapshotCounters(
    COFFLinkerContext &ctx, const IncrementalBaselineSnapshot &snapshot) {
  if (!ctx.config.verbose)
    return;
  IncrementalSnapshotCounters counters = summarizeIncrementalSnapshot(snapshot);
  Log(ctx) << "incremental: state counters: inputs=" << snapshot.inputs.size()
           << " chunks=" << snapshot.chunks.size()
           << " slots=" << counters.slotCount
           << " placements=" << counters.placementCount
           << " symbols=" << snapshot.symbols.size()
           << " bytes=" << snapshot.stateFileSize;
}

static std::optional<uint64_t>
translateIncrementalRvaToFileOffset(ArrayRef<uint8_t> bytes,
                                    ArrayRef<coff_section> sections,
                                    uint64_t rva, uint64_t size) {
  for (const coff_section &section : sections) {
    uint64_t sectionRva = section.VirtualAddress;
    uint64_t sectionSpan =
        std::max<uint64_t>(section.VirtualSize, section.SizeOfRawData);
    if (rva < sectionRva || rva > UINT64_MAX - size ||
        rva + size > sectionRva + sectionSpan)
      continue;

    uint64_t fileOffset = section.PointerToRawData + (rva - sectionRva);
    if (fileOffset > bytes.size() || bytes.size() - fileOffset < size)
      return std::nullopt;
    return fileOffset;
  }
  return std::nullopt;
}

static IncrementalOutputMetadata
extractIncrementalOutputMetadata(StringRef outputData) {
  IncrementalOutputMetadata metadata;
  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(outputData.data()),
                          outputData.size());
  if (bytes.size() < sizeof(dos_header))
    return metadata;

  const auto *dos = reinterpret_cast<const dos_header *>(bytes.data());
  uint64_t peOff = dos->AddressOfNewExeHeader;
  if (peOff > bytes.size() ||
      bytes.size() - peOff < sizeof(llvm::COFF::PEMagic) +
                                sizeof(coff_file_header))
    return metadata;

  const auto *coff = reinterpret_cast<const coff_file_header *>(
      bytes.data() + peOff + sizeof(llvm::COFF::PEMagic));
  metadata.timestamp = coff->TimeDateStamp;

  uint64_t optionalHeaderOff =
      peOff + sizeof(llvm::COFF::PEMagic) + sizeof(coff_file_header);
  if (optionalHeaderOff > bytes.size() ||
      bytes.size() - optionalHeaderOff < coff->SizeOfOptionalHeader)
    return metadata;

  const data_directory *dataDirs = nullptr;
  uint32_t numberOfDataDirs = 0;
  if (coff->SizeOfOptionalHeader >= sizeof(pe32plus_header) &&
      bytes.size() - optionalHeaderOff >= sizeof(pe32plus_header) &&
      reinterpret_cast<const pe32plus_header *>(bytes.data() + optionalHeaderOff)
              ->Magic == llvm::COFF::PE32Header::PE32_PLUS) {
    const auto *pe =
        reinterpret_cast<const pe32plus_header *>(bytes.data() + optionalHeaderOff);
    numberOfDataDirs = pe->NumberOfRvaAndSize;
    if (coff->SizeOfOptionalHeader <
        sizeof(pe32plus_header) + numberOfDataDirs * sizeof(data_directory))
      return metadata;
    dataDirs = reinterpret_cast<const data_directory *>(
        bytes.data() + optionalHeaderOff + sizeof(pe32plus_header));
  } else if (coff->SizeOfOptionalHeader >= sizeof(pe32_header) &&
             bytes.size() - optionalHeaderOff >= sizeof(pe32_header) &&
             reinterpret_cast<const pe32_header *>(bytes.data() + optionalHeaderOff)
                     ->Magic == llvm::COFF::PE32Header::PE32) {
    const auto *pe =
        reinterpret_cast<const pe32_header *>(bytes.data() + optionalHeaderOff);
    numberOfDataDirs = pe->NumberOfRvaAndSize;
    if (coff->SizeOfOptionalHeader <
        sizeof(pe32_header) + numberOfDataDirs * sizeof(data_directory))
      return metadata;
    dataDirs = reinterpret_cast<const data_directory *>(
        bytes.data() + optionalHeaderOff + sizeof(pe32_header));
  } else {
    return metadata;
  }

  uint64_t sectionTableOff = optionalHeaderOff + coff->SizeOfOptionalHeader;
  uint64_t sectionTableSize =
      uint64_t(coff->NumberOfSections) * sizeof(coff_section);
  if (sectionTableOff > bytes.size() ||
      bytes.size() - sectionTableOff < sectionTableSize)
    return metadata;
  ArrayRef<coff_section> sections(
      reinterpret_cast<const coff_section *>(bytes.data() + sectionTableOff),
      coff->NumberOfSections);

  if (numberOfDataDirs <= llvm::COFF::DEBUG_DIRECTORY)
    return metadata;
  const data_directory &debugDir = dataDirs[llvm::COFF::DEBUG_DIRECTORY];
  if (debugDir.RelativeVirtualAddress == 0 ||
      debugDir.Size < sizeof(debug_directory))
    return metadata;

  std::optional<uint64_t> debugDirFileOffset = translateIncrementalRvaToFileOffset(
      bytes, sections, debugDir.RelativeVirtualAddress, debugDir.Size);
  if (!debugDirFileOffset)
    return metadata;
  ArrayRef<debug_directory> debugEntries(
      reinterpret_cast<const debug_directory *>(bytes.data() + *debugDirFileOffset),
      debugDir.Size / sizeof(debug_directory));
  for (const debug_directory &entry : debugEntries) {
    if (entry.Type != llvm::COFF::IMAGE_DEBUG_TYPE_CODEVIEW ||
        entry.SizeOfData < sizeof(codeview::DebugInfo))
      continue;

    uint64_t debugInfoFileOffset = entry.PointerToRawData;
    if (debugInfoFileOffset == 0) {
      std::optional<uint64_t> translated = translateIncrementalRvaToFileOffset(
          bytes, sections, entry.AddressOfRawData, entry.SizeOfData);
      if (!translated)
        continue;
      debugInfoFileOffset = *translated;
    }
    if (debugInfoFileOffset > bytes.size() ||
        bytes.size() - debugInfoFileOffset < entry.SizeOfData)
      continue;

    const auto *info = reinterpret_cast<const codeview::DebugInfo *>(
        bytes.data() + debugInfoFileOffset);
    if (info->Signature.CVSignature != OMF::Signature::PDB70)
      continue;

    llvm::codeview::GUID guid = {};
    memcpy(guid.Guid, info->PDB70.Signature, sizeof(guid.Guid));
    metadata.pdbGuid = guid;
    metadata.pdbAge = info->PDB70.Age;
    break;
  }

  return metadata;
}

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

static const IncrementalBaselineData *
findActiveIncrementalBaselineImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) -> const IncrementalBaselineData * {
        return nullptr;
      },
      [&](const PendingFullImageBuild &) -> const IncrementalBaselineData * {
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

static const ByteReuseLink *
findActiveByteReuseLinkImpl(const COFFLinkerContext &ctx) {
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) -> const ByteReuseLink * {
        return nullptr;
      },
      [&](const PendingFullImageBuild &) -> const ByteReuseLink * {
        return nullptr;
      },
      [&](const FullImageBuild &) -> const ByteReuseLink * { return nullptr; },
      [&](const StateBackedLink &) -> const ByteReuseLink * { return nullptr; },
      [&](const LayoutStableLink &) -> const ByteReuseLink * { return nullptr; },
      [&](const ByteReuseLink &reuse) -> const ByteReuseLink * {
        return &reuse;
      });
}

static bool shouldReusePdbMetadata(const IncrementalPdbReusePolicy &policy) {
  return policy.match([](const ReusePdbMetadata &) { return true; },
                      [](const RebuildPdbMetadata &) { return false; });
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

static IncrementalBaselineEmission buildIncrementalLayoutTables(
    COFFLinkerContext &ctx, const IncrementalInputIndexMap &inputIndices,
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
          slotSection.activeEndRVA = std::max(
              slotSection.activeEndRVA, chunk->getRVA() + chunk->getSize());
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
        slotSection.preservedChunks.push_back(ExistingSlotChunkPlacement{
            key, chunk->getRVA(), chunk->getSize(), chunk->getAlignment()});
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
    packedSection.reserveSize = maxSectionEndRVABySection.lookup(section) -
                                (section->getRVA() + section->getVirtualSize());
    for (Chunk *chunk : section->chunks) {
      if (chunk->getSize() == 0)
        continue;
      std::string key = getIncrementalChunkKey(inputIndices, *chunk);
      packedSection.members.push_back(PackedPrefixChunkPlacement{
          key, chunk->getRVA(), chunk->getSize(), chunk->getAlignment()});
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

static std::string describeIncrementalResolvedSymbol(
    const IncrementalResolvedSymbolSnapshot &state) {
  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  state.match(
      [&](const ObjFileRegularResolvedSymbol &regular) {
        os << regular.name << '\n'
           << "regular\n"
           << "input:" << regular.inputIndex << '\n'
           << regular.value << '\n'
           << "chunk:" << regular.chunkKey << '\n';
      },
      [&](const BitcodeRegularResolvedSymbol &regular) {
        os << regular.name << '\n'
           << "regular\n"
           << "input:none\n"
           << regular.value << '\n'
           << "chunk:none\n";
      },
      [&](const ObjFileCommonResolvedSymbol &common) {
        os << common.name << '\n'
           << "common\n"
           << "input:" << common.inputIndex << '\n'
           << common.size << '\n'
           << common.alignment << '\n';
      },
      [&](const BitcodeCommonResolvedSymbol &common) {
        os << common.name << '\n'
           << "common\n"
           << "input:none\n"
           << common.size << '\n'
           << common.alignment << '\n';
      },
      [&](const ImportDataResolvedSymbol &importData) {
        os << importData.name << '\n'
           << "importdata\n"
           << importData.ordinal << '\n'
           << importData.dllName << '\n'
           << importData.externalName << '\n'
           << importData.typeInfo << '\n';
      },
      [&](const ImportThunkResolvedSymbol &importThunk) {
        os << importThunk.name << '\n'
           << "importthunk\n"
           << importThunk.wrappedSymbolName << '\n';
      },
      [&](const LocalImportResolvedSymbol &localImport) {
        os << localImport.name << '\n' << "localimport\n";
        os << "chunk:" << localImport.chunkKey << '\n';
      },
      [&](const AbsoluteResolvedSymbol &absolute) {
        os << absolute.name << '\n' << "absolute\n" << absolute.value << '\n';
      },
      [&](const ChunkBackedSyntheticResolvedSymbol &synthetic) {
        os << synthetic.name << '\n' << "synthetic\n";
        os << "chunk:" << synthetic.chunkKey << '\n';
      },
      [&](const ImageBaseSyntheticResolvedSymbol &synthetic) {
        os << synthetic.name << '\n' << "synthetic\n" << "chunk:none\n";
      });
  return std::string(buffer);
}

static bool isLateBoundWriterSymbol(StringRef name) {
  return StringSwitch<bool>(name)
      .Case("__buildid", true)
      .Case("__safe_se_handler_table", true)
      .Case("__safe_se_handler_count", true)
      .Case("__guard_fids_table", true)
      .Case("__guard_fids_count", true)
      .Case("__guard_flags", true)
      .Case("__guard_iat_table", true)
      .Case("__guard_iat_count", true)
      .Case("__guard_longjmp_table", true)
      .Case("__guard_longjmp_count", true)
      .Case("__guard_eh_cont_table", true)
      .Case("__guard_eh_cont_count", true)
      .Case("__hybrid_code_map", true)
      .Case("__hybrid_code_map_count", true)
      .Case("__x64_code_ranges_to_entry_points", true)
      .Case("__x64_code_ranges_to_entry_points_count", true)
      .Case("__arm64x_redirection_metadata", true)
      .Case("__arm64x_redirection_metadata_count", true)
      .Case("__RUNTIME_PSEUDO_RELOC_LIST__", true)
      .Case("__RUNTIME_PSEUDO_RELOC_LIST_END__", true)
      .Case("__CTOR_LIST__", true)
      .Case("__DTOR_LIST__", true)
      .Case("__data_start__", true)
      .Case("__data_end__", true)
      .Case("__bss_start__", true)
      .Case("__bss_end__", true)
      .Case("__arm64x_extra_rfe_table", true)
      .Case("__arm64x_extra_rfe_table_size", true)
      .Case("__hybrid_auxiliary_iat", true)
      .Case("__hybrid_auxiliary_iat_copy", true)
      .Case("__hybrid_auxiliary_delayload_iat", true)
      .Case("__hybrid_auxiliary_delayload_iat_copy", true)
      .Case("__arm64x_native_entrypoint", true)
      .Default(false);
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
      // These linker-created placeholders are materialized during Writer setup,
      // which happens after incremental plan validation. Comparing their
      // pre-writer and post-writer forms would produce false drift.
      if (isLateBoundWriterSymbol(sym->getName()))
        return;
      if (auto *coff = dyn_cast<DefinedCOFF>(sym))
        if (!coff->getCOFFSymbol().isExternal())
          return;

      if (auto *reg = dyn_cast<DefinedRegular>(sym)) {
        if (auto *file = dyn_cast<ObjFile>(reg->getFile()))
          if (auto it = inputIndices.find(file); it != inputIndices.end())
            if (SectionChunk *chunk = reg->getChunk()) {
              ObjFileRegularResolvedSymbol state{
                  sym->getName().str(), it->second, reg->getValue(),
                  getIncrementalChunkKey(inputIndices, *chunk)};
              states.push_back(IncrementalResolvedSymbolSnapshot::make<
                               ObjFileRegularResolvedSymbol>(std::move(state)));
              order.emplace_back(
                  describeIncrementalResolvedSymbol(states.back()),
                  states.size() - 1);
              return;
            }
        BitcodeRegularResolvedSymbol state{sym->getName().str(),
                                           reg->getValue()};
        states.push_back(IncrementalResolvedSymbolSnapshot::make<
                         BitcodeRegularResolvedSymbol>(std::move(state)));
        order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                           states.size() - 1);
      } else if (auto *common = dyn_cast<DefinedCommon>(sym)) {
        if (auto *file = dyn_cast<ObjFile>(common->getFile()))
          if (auto it = inputIndices.find(file); it != inputIndices.end()) {
            ObjFileCommonResolvedSymbol state{
                sym->getName().str(), it->second, common->getChunk()->getSize(),
                common->getChunk()->getAlignment()};
            states.push_back(IncrementalResolvedSymbolSnapshot::make<
                             ObjFileCommonResolvedSymbol>(std::move(state)));
            order.emplace_back(describeIncrementalResolvedSymbol(states.back()),
                               states.size() - 1);
            return;
          }
        BitcodeCommonResolvedSymbol state{sym->getName().str(),
                                          common->getChunk()->getSize(),
                                          common->getChunk()->getAlignment()};
        states.push_back(IncrementalResolvedSymbolSnapshot::make<
                         BitcodeCommonResolvedSymbol>(std::move(state)));
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
        LocalImportResolvedSymbol state{
            sym->getName().str(),
            localImport->getChunk()->getDebugName().str()};
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
        if (Chunk *chunk = synthetic->getChunk()) {
          ChunkBackedSyntheticResolvedSymbol state{sym->getName().str(),
                                                   chunk->getDebugName().str()};
          states.push_back(
              IncrementalResolvedSymbolSnapshot::make<
                  ChunkBackedSyntheticResolvedSymbol>(std::move(state)));
        } else {
          ImageBaseSyntheticResolvedSymbol state{sym->getName().str()};
          states.push_back(IncrementalResolvedSymbolSnapshot::make<
                           ImageBaseSyntheticResolvedSymbol>(std::move(state)));
        }
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
  llvm::TimeTraceScope timeScope("Incremental snapshot rebuild");
  ScopedTimer t(ctx.incrementalStateBuildTimer);
  IncrementalBaselineSnapshot snapshot;
  snapshot.machine = ctx.config.machine;
  snapshot.outputPath = ctx.config.outputFile;
  snapshot.hardConfigHash = computeIncrementalHardConfigHash(ctx.config);
  uint32_t effectiveTimestamp = ctx.config.timestamp;
  if (shouldPreserveIncrementalBuildMetadata(ctx))
    if (const IncrementalOutputMetadata *oldMetadata =
            findActiveIncrementalOutputMetadata(ctx))
      effectiveTimestamp = oldMetadata->timestamp;
  snapshot.softConfigHash =
      computeIncrementalSoftConfigHash(ctx.config, effectiveTimestamp);
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

  IncrementalBaselineEmission baselineEmission = buildIncrementalLayoutTables(
      ctx, currentInputs.inputIndices, reuseData, sectionStates, snapshot);
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

    matchIncrementalTextThunkRVA(
        plan.poolThunkRVA, tailCursor, poolCursor, poolEndRVA, claimedThunkRVAs,
        [&](uint64_t selectedPoolThunkRVA) {
          if (poolStart == 0 || selectedPoolThunkRVA < poolStart)
            poolStart = selectedPoolThunkRVA;
          poolCursor = std::min(poolCursor, selectedPoolThunkRVA);
          claimedThunkRVAs.push_back(selectedPoolThunkRVA);
          plan.poolThunkRVA = selectedPoolThunkRVA;
          plan.targeting = IncrementalRedirectTargeting::PoolThunkTarget;
        },
        [&]() {
          plan.engagement = IncrementalRedirectEngagement::Deferred;
          releasePoolThunkRVA(plan);
        },
        freedThunkRVAs);
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

uint64_t computeIncrementalSoftConfigHash(
    const Configuration &config, std::optional<uint32_t> timestampOverride) {
  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  os << config.pdbPath << '\n'
     << config.pdbAltPath << '\n'
     << config.pdbSourcePath << '\n'
     << config.pdbPageSize << '\n'
     << config.lldmapFile << '\n'
     << config.mapFile << '\n'
     << timestampOverride.value_or(config.timestamp) << '\n'
     << config.repro << '\n'
     << unsigned(config.buildIDHash) << '\n';
  for (const std::string &natvis : config.natvisFiles)
    os << natvis << '\n';
  appendSortedStringMap(os, config.namedStreams);
  return xxh3_64bits(buffer);
}

IncrementalCurrentInputs
prepareCurrentIncrementalInputs(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Incremental input hashing");
  ScopedTimer t(ctx.incrementalInputHashTimer);
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

const IncrementalCurrentInputs *
findActiveIncrementalCurrentInputs(const COFFLinkerContext &ctx) {
  const IncrementalBaselineData *baseline = findActiveIncrementalBaselineImpl(ctx);
  if (!baseline || baseline->currentInputs.hashes.empty())
    return nullptr;
  return &baseline->currentInputs;
}

const IncrementalBaselineData *
findActiveIncrementalBaseline(const COFFLinkerContext &ctx) {
  return findActiveIncrementalBaselineImpl(ctx);
}

const IncrementalOutputMetadata *
findActiveIncrementalOutputMetadata(const COFFLinkerContext &ctx) {
  const IncrementalBaselineData *baseline = findActiveIncrementalBaselineImpl(ctx);
  if (!baseline)
    return nullptr;
  return &baseline->previousOutputMetadata;
}

bool shouldPreserveIncrementalBuildMetadata(const COFFLinkerContext &ctx) {
  return !ctx.config.timestampSpecified && !ctx.config.repro &&
         ctx.config.buildIDHash != BuildIDHash::Binary;
}

bool shouldReuseIncrementalPdbMetadata(const COFFLinkerContext &ctx) {
  if (!shouldPreserveIncrementalBuildMetadata(ctx))
    return false;
  return ctx.incremental->match(
      [&](const IncrementalDisabled &) { return false; },
      [&](const PendingFullImageBuild &) { return false; },
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

bool shouldSkipIncrementalPdbEmission(const COFFLinkerContext &ctx) {
  if (!shouldReuseIncrementalPdbMetadata(ctx))
    return false;
  const ByteReuseLink *reuse = findActiveByteReuseLinkImpl(ctx);
  return reuse && reuse->reuse.exactLayoutOnly &&
         reuse->baseline.changedInputs.empty();
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
  llvm::TimeTraceScope timeScope("Incremental symbol-state validation");
  ScopedTimer t(ctx.incrementalSymbolValidationTimer);
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
          [](const ObjFileRegularResolvedSymbol &symbol) {
            return symbol.name;
          },
          [](const BitcodeRegularResolvedSymbol &symbol) {
            return symbol.name;
          },
          [](const ObjFileCommonResolvedSymbol &symbol) { return symbol.name; },
          [](const BitcodeCommonResolvedSymbol &symbol) { return symbol.name; },
          [](const ImportDataResolvedSymbol &symbol) { return symbol.name; },
          [](const ImportThunkResolvedSymbol &symbol) { return symbol.name; },
          [](const LocalImportResolvedSymbol &symbol) { return symbol.name; },
          [](const AbsoluteResolvedSymbol &symbol) { return symbol.name; },
          [](const ChunkBackedSyntheticResolvedSymbol &symbol) {
            return symbol.name;
          },
          [](const ImageBaseSyntheticResolvedSymbol &symbol) {
            return symbol.name;
          });
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

  if (ctx.config.tailMergeMode == TailMergeMode::TailMergeStringLiterals) {
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

  Expected<IncrementalBaselineSnapshot> stateOrErr = [&]() {
    llvm::TimeTraceScope timeScope("Incremental state read/decode");
    ScopedTimer t(ctx.incrementalStateReadTimer);
    return loadIncrementalState(ctx.config.incrementalStatePath);
  }();
  if (!stateOrErr) {
    installFallback(
        rebuildForRejectedBaseline(toString(stateOrErr.takeError())));
    return;
  }

  if (stateOrErr->machine != ctx.config.machine) {
    installFallback(rebuildForConfigDrift());
    return;
  }
  if (stateOrErr->outputPath != ctx.config.outputFile) {
    installFallback(rebuildForOutputDrift(
        "incremental state was written for a different output"));
    return;
  }
  ErrorOr<std::unique_ptr<MemoryBuffer>> oldImage = [&]() {
    llvm::TimeTraceScope timeScope("Incremental output verification");
    ScopedTimer t(ctx.incrementalOutputVerifyTimer);
    return MemoryBuffer::getFile(ctx.config.outputFile, /*IsText=*/false,
                                 /*RequiresNullTerminator=*/false);
  }();
  if (!oldImage) {
    installFallback(rebuildForOutputDrift());
    return;
  }
  {
    llvm::TimeTraceScope timeScope("Incremental output verification");
    ScopedTimer t(ctx.incrementalOutputVerifyTimer);
    if ((*oldImage)->getBufferSize() != stateOrErr->outputSize ||
        xxh3_64bits((*oldImage)->getBuffer()) != stateOrErr->outputHash) {
      installFallback(rebuildForOutputDrift());
      return;
    }
  }

  IncrementalBaselineData baseline;
  baseline.snapshot = std::move(*stateOrErr);
  baseline.oldImage = std::move(*oldImage);
  baseline.previousOutputMetadata =
      extractIncrementalOutputMetadata(baseline.oldImage->getBuffer());
  logIncrementalSnapshotCounters(ctx, baseline.snapshot);

  for (const IncrementalInputState &input : baseline.snapshot.inputs) {
    if (input.parentName.empty())
      continue;

    // Let the current link drive archive extraction. Replaying baseline members
    // here pollutes the graph before we know reuse is valid and makes later
    // extraction validation observe the replay instead of current demand.
    baseline.expectedArchiveMembers.insert(getIncrementalArchiveMemberKey(
        input.parentName, input.archiveOffset, input.name));
  }

  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeStateBackedLink(
               std::move(baseline),
               IncrementalPdbReusePolicy::make<RebuildPdbMetadata>(),
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

  uint64_t hardHash = computeIncrementalHardConfigHash(ctx.config);
  if (loaded->baseline.snapshot.machine != ctx.config.machine ||
      loaded->baseline.snapshot.hardConfigHash != hardHash) {
    installPendingIncrementalFullImageBuild(ctx, rebuildForConfigDrift());
    return;
  }

  uint64_t softHash = computeIncrementalSoftConfigHash(ctx.config);
  bool reusePdbMetadata = loaded->baseline.snapshot.softConfigHash == softHash;
  if (!reusePdbMetadata && shouldPreserveIncrementalBuildMetadata(ctx)) {
    uint64_t preservedTimestampHash = computeIncrementalSoftConfigHash(
        ctx.config, loaded->baseline.previousOutputMetadata.timestamp);
    reusePdbMetadata =
        loaded->baseline.snapshot.softConfigHash == preservedTimestampHash;
  }
  IncrementalPdbReusePolicy pdbReuse = reusePdbMetadata
                                           ? IncrementalPdbReusePolicy::make<
                                                 ReusePdbMetadata>()
                                           : IncrementalPdbReusePolicy::make<
                                                 RebuildPdbMetadata>();

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

  loaded->baseline.loadedArchiveMembers.clear();
  for (const auto &entry : ctx.loadedArchiveMemberKeys) {
    if (loaded->baseline.expectedArchiveMembers.contains(entry.getKey()))
      loaded->baseline.loadedArchiveMembers.insert(entry.getKey());
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

  if (!loaded->baseline.changedInputs.empty() &&
      !validateIncrementalSymbolStates(ctx, loaded->baseline))
    return;

  if (ctx.config.verbose) {
    Log(ctx) << "incremental: using state " << ctx.config.incrementalStatePath;
    pdbReuse.match([&](const ReusePdbMetadata &) {},
                   [&](const RebuildPdbMetadata &) {
                     Log(ctx) << "incremental: soft-config metadata "
                                 "changed; rebuilding PDB metadata";
                   });
  }

  PendingFullImageBuild pending = takePendingFullImageBuild(ctx);
  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeLayoutStableLink(
               std::move(loaded->baseline), std::move(pdbReuse),
               std::move(pending.baselineEmission)));
}

void finalizeIncrementalLink(COFFLinkerContext &ctx) {
  if (errorCount() != 0 || !shouldEmitIncrementalBaselineImpl(ctx))
    return;

  const IncrementalCurrentInputs *activeInputs =
      findActiveIncrementalCurrentInputs(ctx);
  IncrementalCurrentInputs fallbackInputs;
  if (!activeInputs) {
    fallbackInputs = prepareCurrentIncrementalInputs(ctx);
    activeInputs = &fallbackInputs;
  }
  const ByteReuseLink *activeReuse = findActiveByteReuseLinkImpl(ctx);
  const IncrementalReuseData *reuseData =
      activeReuse ? &activeReuse->reuse : nullptr;

  IncrementalStateBuildResult buildResult =
      buildIncrementalState(ctx, *activeInputs, reuseData);
  if (!shouldEmitNextBaseline(buildResult.baselineEmission))
    return;
  Error err = [&]() -> Error {
    llvm::TimeTraceScope timeScope("Incremental state write");
    ScopedTimer t(ctx.incrementalStateWriteTimer);
    return writeIncrementalState(ctx.config.incrementalStatePath,
                                 buildResult.snapshot);
  }();
  if (err)
    Warn(ctx) << "failed to write incremental state: "
              << toString(std::move(err));
}

void noteIncrementalArchiveMemberLoad(COFFLinkerContext &ctx,
                                      StringRef archiveName,
                                      uint64_t archiveOffset,
                                      StringRef memberName) {
  if (archiveName.empty())
    return;

  ctx.loadedArchiveMemberKeys.insert(
      getIncrementalArchiveMemberKey(archiveName, archiveOffset, memberName));
}

} // namespace lld::coff
