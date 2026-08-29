# Failed Paths

This file is the active structured rejection record. [OPTIMIZATION_PATH_FAILURES.md](OPTIMIZATION_PATH_FAILURES.md) retains the earlier full narrative and artifact links. Reopen an archived family only when a new reachable path or new measurement evidence changes its premise.

## MEASURE-001 - Cargo rebuild timing presented as linker timing

- Hypothesis: switching Cargo linker settings and timing cargo build would measure linker throughput.
- Scope: early Rust/MSVC benchmark setup.
- Attempted change: changed Cargo linker settings and rustflags between lanes.
- Benchmark evidence: the setting changes forced compilation, so results mixed compile and link time.
- Correctness evidence: builds completed, but the measured operation was wrong.
- Failure mode: invalid benchmark.
- Why not to retry unchanged: compiler work dominates and differs between configurations.
- Reopen only if: one linker invocation is captured once and replayed directly.
- Related commit or revert: none.

## MEASURE-002 - Windows PowerShell 5.1 benchmark host

- Hypothesis: Windows PowerShell and PowerShell Core would provide equivalent host timing.
- Scope: direct-link replay driver.
- Attempted change: ran the old harness under Windows PowerShell 5.1.
- Benchmark evidence: host-side timing distortion was large, especially for rust-lld; pwsh removed it.
- Correctness evidence: links completed.
- Failure mode: invalid timing host.
- Why not to retry unchanged: host process-launch behavior changes the measured wall time.
- Reopen only if: an independent timer proves equivalence on the current machine.
- Related commit or revert: none.

## MEASURE-003 - Whole-lane sequential A/B batches

- Hypothesis: large sequential batches would average away temporal drift.
- Scope: C:/Users/Daniel/linker-bench/bench-cargo-linker.ps1.
- Attempted change: ran 50 current links, then 50 frozen-baseline links, then 50 rust-lld links.
- Benchmark evidence: byte-identical current and frozen linkers reported 1908 ms and 2088 ms averages, with 1898 ms and 2056 ms p50 values. A rotated rerun reduced their average gap to 1.3 ms.
- Correctness evidence: all 150 links succeeded.
- Failure mode: lane order was confounded with broad machine/cache speed shifts.
- Why not to retry unchanged: duplicate binaries proved the bias was larger than likely optimization effects.
- Reopen only if: lane order is balanced or samples are paired in time.
- Related commit or revert: external harness changed to deterministic lane rotation.

## MEASURE-004 - Treating explicit manifest outputs as the complete clean-output set

- Hypothesis: deleting output paths captured explicitly from linker arguments would create a clean full-link replay.
- Scope: C:/Users/Daniel/linker-bench/bench-cargo-linker.ps1.
- Attempted change: removed manifest output_path, pdb_path, implib_path, and ilk_path before each process.
- Benchmark evidence: Rust supplied /DEBUG and /PDBALTPATH without /PDB, so pdb_path was empty and the derived 143 MB PDB remained. Removing derived PDB/LIB/EXP/ILK files changed n=100 wall averages from roughly 1.35 seconds to 1.71 seconds on the same capture.
- Correctness evidence: both policies linked successfully; this is a workload-definition distinction.
- Failure mode: the harness labeled an existing-PDB overwrite workload as clean output.
- Why not to retry unchanged: implicit linker outputs are omitted from capture metadata.
- Reopen only if: the intended benchmark explicitly measures overwrite-in-place linking and names that policy.
- Related commit or revert: external harness now derives output siblings from /OUT.

## PERF-001 - Remove dormant symbol-mutation counters

- Hypothesis: deleting always-null SymbolMutationStats parameters and guarded counter branches would reduce hot regular-object symbol initialization work.
- Scope: lld/COFF/COFFLinkerContext.cpp/.h, InputFiles.cpp/.h, and SymbolTable.cpp/.h.
- Attempted change: removed the stats types, context field, uncalled printer, forwarding overloads, nullable parameters, and counter-only branches while preserving all symbol operations.
- Benchmark evidence: the clean-output n=50 screen slightly improved wall 1904 to 1898 ms and Initialize Symbols 393.24 to 391.56 ms. The lower-noise existing-output n=100 confirmation regressed wall 1685 to 1687 ms, Initialize Symbols 357.55 to 359.49 ms, Input Parse 483.32 to 485.47 ms, and Total Linking Time 1560.99 to 1565.89 ms.
- Correctness evidence: the candidate built successfully; exhaustive search proved the facility had no non-null producer or live consumer.
- Failure mode: the source and binary became smaller, but code layout/register-allocation changes or noise outweighed predictable null-branch removal in the confirmation run.
- Why not to retry unchanged: the intended timer and total linker timer both regressed across 100 alternating pairs.
- Reopen only if: production builds gain LTO/PGO changes that materially alter these functions, or a new profile attributes retired instructions to the dormant branches.
- Related commit or revert: candidate discarded by explicit reverse patch; documented in this record-only commit.

## ARCHIVE-001 - Broad debug S caching and module-symbol buffering

- Hypothesis: caching parsed debug subsections and buffering module symbols per object would avoid repeated parsing and writes.
- Scope: lld/COFF/PDB.cpp.
- Attempted change: broad debug S cache plus object-local module-symbol buffers; a narrower one-write-per-object version was also measured.
- Benchmark evidence: broad A/B moved wall 990.0 to 991.75 ms and PDB emission 389.5 to 398.0 ms. Narrow batching moved wall 950.88 to 953.71 ms.
- Correctness evidence: no retained correctness failure; changes were removed for performance.
- Failure mode: added planning, storage, and copy costs exceeded saved work.
- Why not to retry unchanged: both broad and narrow forms lost end to end.
- Reopen only if: profiling identifies a distinct write path with fewer bytes or allocations and no replay-plan overhead.
- Related commit or revert: archived patches audit1-pdb-debugs-module-replay-cache.diff and audit3-pdb-buffer-module-symbol-writes.patch.

## ARCHIVE-002 - COMDAT section-name reuse

- Hypothesis: reusing split COMDAT section names would reduce parser cost.
- Scope: lld/COFF/InputFiles.cpp and Chunks.cpp.
- Attempted change: cached or reused section-name processing.
- Benchmark evidence: Read COMDAT Sections improved about 62.0 to 60.5 ms while Total Linking Time regressed 965.5 to 983.75 ms and wall 1029.5 to 1049.75 ms.
- Correctness evidence: no reported correctness failure.
- Failure mode: microbucket gain did not survive end-to-end measurement.
- Why not to retry unchanged: measured regression outweighed the roughly 1.5 ms local gain.
- Reopen only if: a newer workload makes section-name handling materially larger and a same-capture A/B confirms it.
- Related commit or revert: discarded experiment.

## ARCHIVE-003 - Debug-section lookup and stripped-content cache

- Hypothesis: caching debug section lookups and stripped content would reduce input parse work.
- Scope: lld/COFF input/debug processing.
- Attempted change: audit2-debug-section-cache.diff family.
- Benchmark evidence: wall regressed 950.50 to 965.00 ms, Input File Reading 299 to 307 ms, Input Parse 263.5 to 270.25 ms, and Total Linking Time 895.25 to 907.75 ms.
- Correctness evidence: no reported correctness failure.
- Failure mode: cache management cost exceeded saved lookups.
- Why not to retry unchanged: every relevant front-half timer moved backward.
- Reopen only if: a distinct lookup producer/consumer pair is proven hot without this cache's storage costs.
- Related commit or revert: audit2-debug-section-cache.diff, discarded.

## ARCHIVE-004 - Parallel BulkPublic fill

- Hypothesis: parallelizing the Publics stream fill would reduce PDB time.
- Scope: PDB publics stream layout.
- Attempted change: parallel BulkPublic population.
- Benchmark evidence: Publics Stream Layout improved 29.5 to 21.75 ms while wall regressed 951.25 to 956.00 ms.
- Correctness evidence: no reported correctness failure.
- Failure mode: parallel overhead and downstream effects erased the local gain.
- Why not to retry unchanged: an approximately 8 ms bucket win produced a wall regression.
- Reopen only if: the publics bucket becomes substantially larger or work can be fused without added synchronization.
- Related commit or revert: discarded experiment.

## ARCHIVE-005 - Direct global-symbol rewrite

- Hypothesis: bypassing generic global-symbol rewrite machinery would cut Symbol Merging.
- Scope: lld/COFF/PDB.cpp.
- Attempted change: direct global-symbol rewrite path.
- Benchmark evidence: wall regressed 959.25 to 965.25 ms, Symbol Merging 167.75 to 170.25 ms, and PDB Emission 383.5 to 387.5 ms.
- Correctness evidence: no reported correctness failure.
- Failure mode: the replacement path was slower in its target bucket.
- Why not to retry unchanged: target and end-to-end measurements both regressed.
- Reopen only if: a different symbol class is proven dominant with a narrower operation cut.
- Related commit or revert: discarded experiment.

## ARCHIVE-006 - Symbol-table reserve, prehash, and guarded-store variants

- Hypothesis: reducing table growth, duplicate hashing, pendingArchiveLoad writes, and isUsedInRegularObj stores would speed regular-object symbol initialization.
- Scope: lld/COFF/InputFiles.cpp and SymbolTable.cpp.
- Attempted change: table reserve; CachedHashStringRef threading; idempotent forceLazy plus guarded used-bit store.
- Benchmark evidence: reserve regressed wall 978.25 to 984.75 ms. Prehash improved Symbol Table Insert/Lookup 72.0 to 67.8 ms but regressed wall average 1031 to 1049 ms and Total Linking Time 962.6 to 978.1 ms. Guarded stores regressed wall average 915 to 925 ms and Total Linking Time 846.5 to 853.5 ms.
- Correctness evidence: prehash and guarded-store focused COFF slices passed 13/13.
- Failure mode: added object/key plumbing or branch costs outweighed local savings.
- Why not to retry unchanged: three increasingly narrow forms lost in same-capture measurements.
- Reopen only if: a separate live symbol operation is identified by current profiles and avoids these data-flow changes.
- Related commit or revert: discarded experiments; see repo-symbol-mutation-prehash-n100 and repo-lazyguard-ab-n100 in the archive.

## ARCHIVE-007 - Metadata-only module replay plan

- Hypothesis: recording first-pass metadata would avoid costly second-pass module-symbol parsing.
- Scope: lld/COFF/PDB.cpp.
- Attempted change: metadata-only debug S module replay plan.
- Benchmark evidence: wall regressed 950.00 to 1005.75 ms, PDB Emission 384.25 to 424.75 ms, Add Objects 197.5 to 237.0 ms, and Symbol Merging 166.75 to 204.5 ms; Commit to Disk was flat.
- Correctness evidence: a corrupt-subsection rollback fix existed, but performance rejected the plan.
- Failure mode: plan construction and replay overhead exceeded second-pass parsing cost.
- Why not to retry unchanged: decisive regression in every producer-side bucket.
- Reopen only if: parsing semantics or workload composition changes enough to make second-pass parsing independently dominant.
- Related commit or revert: audit2-pdb-module-symbol-plan.diff family, discarded.

## ARCHIVE-008 - Fixed-offset generic symbol type remap

- Hypothesis: selected symbol kinds could bypass remapTypesInSymbolRecord through fixed offsets.
- Scope: lld/COFF/PDB.cpp.
- Attempted change: FixedSymbolTypeRef and getFixedSymbolTypeRef fast path.
- Benchmark evidence: baseline 940/951/958/989/997 ms min/p50/avg/p95/max versus patched 935/953/965/1015/1039 ms; average regressed 7 ms and p95 26 ms.
- Correctness evidence: no reported correctness failure.
- Failure mode: dispatch and specialized handling cost more than the generic routine.
- Why not to retry unchanged: clean repo-vs-repo A/B rejected it.
- Reopen only if: a single newly dominant symbol kind has a simpler proven-safe representation.
- Related commit or revert: reverted experiment.

## ARCHIVE-009 - Eager PDB relocation memoization

- Hypothesis: a PDB-local table of resolved debug S relocation targets would accelerate module-symbol writes.
- Scope: lld/COFF/PDB.cpp.
- Attempted change: eagerly built SectionChunk-keyed resolved-relocation side table consumed only by module-symbol writers.
- Benchmark evidence: wall p50/avg/p95 regressed 900/906/940 to 936/940/972 ms. Relocate Module Symbols stayed 43.9 versus 43.6 ms while PDB Emission rose 320.9 to 348.6 ms.
- Correctness evidence: six focused relocation tests passed.
- Failure mode: eager cache build moved cost into Add Objects without reducing the composite relocation bucket.
- Why not to retry unchanged: the target timer was flat and total cost rose roughly 34 ms.
- Reopen only if: lazy memoization is tied to a separately measured repeated relocation lookup and preserves all silent-skip cases.
- Related commit or revert: discarded pdb-fastpath-p3n experiment.

## ARCHIVE-010 - SmallVector overwrite storage for PDB rewrite

- Hypothesis: resize_for_overwrite would avoid zero-fill cost in rewritten symbol buffers.
- Scope: lld/COFF/PDB.cpp.
- Attempted change: std::vector byte buffers replaced with SmallVector byte buffers and resize_for_overwrite.
- Benchmark evidence: 28 ms average and 43 ms minimum wall regression across 50 runs. Reverting measured 897/919/930/971/989 ms versus 940/951/958/989/997 ms.
- Correctness evidence: no reported correctness failure.
- Failure mode: SmallVector growth/reallocation on MSVC cost more than zero filling.
- Why not to retry unchanged: the large minimum delta rules out noise.
- Reopen only if: storage can retain std::vector allocation behavior while safely avoiding initialization.
- Related commit or revert: 0f2896aed7b3, reverted; current tree uses std::vector.

## ARCHIVE-011 - ICF and eager input prefetch as warm-link throughput flags

- Hypothesis: ICF or /prefetch-inputs would improve warm link throughput.
- Scope: linker configuration.
- Attempted change: benchmarked ICF and prefetch lanes.
- Benchmark evidence: ICF was slower; /prefetch-inputs was effectively neutral on warm replay.
- Correctness evidence: links completed.
- Failure mode: extra work had no warm-cache payoff.
- Why not to retry unchanged: the active benchmark is explicitly warm and directly replayed.
- Reopen only if: evaluating cold-link behavior with a controlled cache protocol.
- Related commit or revert: none.

## Verified cautions

- Relocate Module Symbols is composite: subsection copy, relocation-range scan, application, and alignment repair. Treat it as applyRelocation evidence only after narrowing the timed scope.
- Generic relocation caches touch normal image emission and several PDB consumers. CodeView silent-skip and compatibility cases must remain exact.
- Global-symbol rewriting was near noise in intrusive historical per-record timing. Current clean profiles must precede any renewed work there.
