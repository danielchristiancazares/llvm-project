=====================================
Windows x64 Speed Audit for lld/COFF
=====================================

This audit narrows the optimization discussion to ``lld/COFF`` itself on
Windows x64 targets. It separates three different kinds of work:

* ``lld``-local code changes,
* toolchain-wide changes that also require object producers or compilers, and
* deployment or custom-toolchain tuning such as PGO or LTO for ``lld`` itself.

The primary metric is measured ``lld-link`` wall time on Windows x64.
Representative baselines should include both PDB-heavy and non-PDB links.
Use existing ``/time`` timers and ``--time-trace`` output as ground truth
before proposing new concurrency work.

Current State
=============

``lld/COFF`` already has meaningful parallelism and Windows-specific input
pipelining, so the audit should not start from the assumption that the backend
is broadly single-threaded.

* ``lld/COFF/MarkLive.cpp`` still implements GC as a single-threaded worklist
  traversal.
* ``lld/COFF/DebugTypes.cpp`` already uses parallel work for GHash loading,
  insertion, remapping, and sorting in the PDB type-merging path.
* ``lld/COFF/ICF.cpp`` already uses parallel work for class initialization and
  iterative equivalence refinement in ICF.
* Windows input handling already uses async file-open futures in
  ``lld/COFF/Driver.cpp``. The ``/prefetch-inputs`` option is narrower: it only
  requests early mmap prefetch and does not add a separate overlapped-I/O
  subsystem.

Measure First
=============

Use measured ``lld-link`` time, not intuition, to rank work.

Collect baselines for at least these workloads:

* a PDB-heavy C++ link,
* a Rust MSVC link, and
* a non-PDB optimized release link.

For each baseline, capture both:

* ``lld-link /time ...`` for the existing COFF timers, and
* ``lld-link /time --time-trace=<file> ...`` for finer-grained scopes.

The existing ``/time`` output is already useful for:

* ``Input File Reading``,
* ``GC``,
* ``ICF``,
* ``PDB Emission (Cumulative)``,
* ``Global Type Hashing``,
* ``GHash Type Merging``, and
* other top-level buckets such as ``Code Layout`` and ``Commit Output File``.

Use ``--time-trace`` to break down coarse writer buckets further, especially:

* ``Assign addresses``, and
* ``Write sections``.

High-Confidence lld-Local Opportunities
=======================================

PDB Type Merging
----------------

The highest-confidence hotspot remains the PDB type-merging path when debug
info is enabled.

The key correction is in ``lld/COFF/DebugTypes.cpp``. The TODO near
``TpiSource::fillIsItemIndexFromDebugT()`` is not about recomputing GHash
values. It says that ``.debug$H`` could store the ``isItemIndex`` information so
that ``lld`` does not have to rescan ``.debug$T`` to rebuild it.

That yields two separate tracks:

* A richer ``.debug$H`` format is a toolchain-wide producer change, not an
  ``lld``-only change.
* The ``lld``-local version of the work is to optimize or cache
  ``fillIsItemIndexFromDebugT()`` and benchmark that change directly.

Because the current code already parallelizes GHash loading and remapping, this
path should be described as a measured, source-backed hotspot rather than a
missing-parallelism guess.

Garbage Collection
------------------

``markLive()`` is still single-threaded, so GC remains a plausible secondary
candidate. It should stay measurement-gated.

Only pursue GC parallelization if ``/time`` shows that ``GC`` is a meaningful
fraction of end-to-end link time on the target workloads. If GC is not a large
bucket, concurrency added here is likely to increase complexity without moving
the wall-clock result enough to matter.

Input Resolution and Deferred Tasks
-----------------------------------

``LinkerDriver::taskQueue`` is real and sequential, but it is a deferred-input
queue, not a standalone "archive scanning" phase.

That distinction matters because archive loading is lazy and symbol-driven:

* archives first populate lazy symbols,
* archive members are loaded only when symbol resolution forces them, and
* the current queue preserves that demand-driven flow.

Parallelizing this area therefore requires preserving determinism and current
archive-resolution semantics. Keep it as a redesign candidate only if
``Input File Reading`` or adjacent input-resolution work dominates measured
link time.

Measurement-Gated, Higher-Risk Candidates
=========================================

Writer Address Assignment
-------------------------

``Writer::writeSections()`` already performs per-chunk parallel work.

The part that remains sequential is ``Writer::assignAddresses()``, but it is
not an embarrassingly parallel pass. It computes prefix-dependent layout while
respecting:

* chunk alignment,
* hotpatch padding,
* EC entry-thunk space,
* ARM64EC range boundaries, and
* section-level raw and virtual size evolution.

Treat this as a measurement-gated candidate. If the writer is proven to matter,
prefer evaluating a two-pass prototype over ad hoc threading.

Symbol Table Concurrency
------------------------

The current symbol table uses a ``DenseMap``, but the surrounding symbol
resolution flow is not concurrent.

That means swapping in a concurrent hash map by itself is not a meaningful
optimization plan. Any serious concurrency work here would need to redesign the
resolution path around lazy archives, conflict resolution, and input ordering.

This item should therefore be removed as a near-term recommendation or
downgraded to a high-risk redesign note.

Deployment and Custom-Toolchain Tuning
======================================

These items may be valid for teams shipping a custom linker, but they are not
evidence of an algorithmic gap in ``lld/COFF`` itself.

``/threads:N``
--------------

``/threads:N`` sets LLVM's global parallel strategy and also supplies the
default for ``/opt:lldltojobs=``. It is not a per-phase tuning knob.

Keep it in a deployment appendix, not in the primary list of source-level
optimization opportunities.

PGO and LTO for lld
-------------------

Building ``lld`` with PGO and LTO can be worthwhile for downstream toolchains
that ship a custom linker. That belongs in a "shipping a custom linker" section.

It should not be presented as proof that ``lld/COFF`` has an obvious missing
optimization in its current algorithms.

``/prefetch-inputs``
--------------------

If ``/prefetch-inputs`` is ever made default on Windows x64, document that as a
user-visible default change. It should not be described as a new parallel I/O
subsystem because the implementation only requests earlier OS prefetch for
mapped inputs.

Validation Requirements
=======================

Any optimization candidate should clear all of the following:

* measurable link-time improvement across the PDB-heavy C++, Rust MSVC, and
  non-PDB release workloads,
* no PE or PDB correctness regressions,
* no determinism regressions,
* no unacceptable memory growth on large links.

If a change touches debug types or archive resolution, add focused synthetic or
microbenchmark coverage instead of relying only on end-to-end links.

Scope and Defaults
==================

The assumptions for this audit are:

* scope is ``lld/COFF`` on Windows x64 only,
* the goal is wall-clock link speed rather than output size or runtime
  performance,
* source-backed ``lld``-local opportunities should be prioritized first,
* PDB/type-merging work is the strongest near-term candidate, and
* GC, writer parallelization, archive-resolution redesign, and symbol-table
  concurrency should remain measurement-gated.
