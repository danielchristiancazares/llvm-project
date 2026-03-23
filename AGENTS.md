# Repository Guidelines

Fork of LLD's COFF linker focused on Rust.

## Project Structure & Module Organization
This checkout is a focused LLVM tree where COFF linker work lives under `lld/COFF`. Keep edits scoped there unless you are touching shared LLD infrastructure.

Source is in `lld/COFF/*.cpp` and `lld/COFF/*.h`, with shared interfaces in `lld/include` and tool wiring in `lld/tools/lld/CMakeLists.txt`. Tests are in `lld/test/COFF` with fixtures under `lld/test/COFF/Inputs`. Primary build/test integration is in `lld/CMakeLists.txt`, `lld/test/CMakeLists.txt`, and `lld/test/lit.cfg.py`. COFF docs are under `lld/docs`.

## Build, Test, and Development Commands
Use a dedicated build directory outside source:

`cmake -S llvm -B build -G Ninja -DLLVM_ENABLE_PROJECTS=lld -DLLVM_TARGETS_TO_BUILD="X86;AArch64;ARM" -DCMAKE_BUILD_TYPE=RelWithDebInfo`

`cmake --build build -j`

`cmake --build build --target lld`

`cmake --build build --target check-lld`

For COFF-only manual runs, prefer direct lit execution:

`cd build`  
`bin/llvm-lit -sv ../lld/test/COFF`

`bin/llvm-lit -sv ../lld/test/COFF/align.s`

Built binaries are in `build/bin` (for example `lld`, `lld-link`, and `ld.lld`).

## Coding Style & Naming Conventions
Style follows LLVM defaults (`.clang-format` is `BasedOnStyle: LLVM`). Use existing patterns: `UpperCamelCase` for types/namespaces and `camelCase` for variables/functions. Match file-local style before introducing new abstractions.

Keep code narrowly scoped, prefer existing helper utilities in LLVM/LLD, and use `Options.td` for new user-facing flags. Include copyright/SPDX headers exactly as in neighboring files.

## Testing Guidelines
LLD tests are lit-based. `lld/test/lit.cfg.py` treats `.s`, `.ll`, `.test`, `.yaml`, and `.objtxt` as test files. Add regression coverage for each COFF behavior change using `RUN:` and `CHECK:` idioms.

For broader verification run `check-lld`. For targeted fixes, use a focused `llvm-lit` command on `lld/test/COFF` directories or specific files. Unit test coverage is limited; if you modify shared COFF-independent logic, update or add `lld/unittests` coverage where appropriate.

## Commit & Pull Request Guidelines
Recent history commonly uses subsystem prefixes in commit subjects (for example `[lld][COFF]`, `[clang]`, `[MLIR]`). Use clear imperative subjects and keep commits narrowly focused.

PRs should include: linked issue or regression context, summary of impacted files/flags, and exact commands run. Prefer attaching test results instead of promises (e.g., `cmake --build build --target check-lld` and focused `llvm-lit` runs). Include platform caveats when COFF behavior changes are toolchain-specific.

## Security & Configuration Tips
LLD COFF assumes well-formed, trusted object files, so avoid running untrusted binaries during development. Keep PDB/DIA/debug toolchain dependencies version-consistent in CI-like environments, and record environment-specific flags when tests depend on them.
