# Performance Log

## Current Active Timings

| Benchmark | Baseline | Current | Delta | Command | Last Updated |
|---|---:|---:|---:|---|---|
| Hauberk clean-output direct-link wall, n=100 average | 1723.9 ms | 1710.5 ms | -13.4 ms (-0.78%) | linker-bench hauberk dev, alternating A/B | 2026-08-28 19:20 PDT |
| Hauberk clean-output Total Linking Time, n=100 average | 1600.85 ms | 1587.79 ms | -13.06 ms (-0.82%) | lld-link /time from the same replay | 2026-08-28 19:20 PDT |
| Hauberk clean-output GC, n=100 average | 152.52 ms | 148.84 ms | -3.68 ms (-2.41%) | lld-link /time from the same replay | 2026-08-28 19:20 PDT |
| Hauberk existing-output direct-link wall, n=100 average | 1356 ms | 1343 ms | -13 ms (-0.96%) | linker-bench hauberk dev, alternating A/B | 2026-08-28 19:08 PDT |

## Baseline

- Commit: 702deda99e7a9823f6bab178f68e396e21bdcc9b.
- Hardware / OS: AMD Ryzen 9 9900X, 12 cores / 24 logical processors, 48 GiB RAM, Windows 11 Pro 10.0.26200.
- Toolchain: CMake 4.3.3, Ninja 1.13.2, clang-cl 22.1.8, PowerShell 7.6.5, rustc/cargo 1.98.0.
- Build profile: out-of-tree RelWithDebInfo, assertions off, X86/AArch64/ARM targets, Ninja build at C:/Users/Daniel/llvm-build-lld-perf-main.
- Linker binary: LLD 24.0.0 at commit 702deda99e7a. Frozen duplicate: C:/Users/Daniel/linker-bench/binaries/baseline-702deda99e7a/lld-link.exe. Both SHA-256 hashes matched before measurement.
- Command: pwsh -NoProfile -File C:/Users/Daniel/linker-bench/bench-cargo-linker.ps1 -Target hauberk -Profile dev -Iterations 50 -ResultsDir C:/Users/Daniel/linker-bench/results/baseline-702deda99e7a.
- Input: Hauberk x86_64-pc-windows-msvc dev executable captured with cargo --locked; one 74,490-byte replay response file and 1,281 materialized input files.
- Capture isolation: CARGO_TARGET_DIR is under linker-bench results. Cargo.lock and Cargo.toml SHA-256 hashes were identical before and after capture.
- Active output policy: remove the explicit output plus derived PDB, LIB, EXP, ILK, LLILK, and LLPDBCACHE files before each measured process starts. The original discovery baseline retained the derived PDB because Cargo supplied /PDBALTPATH without /PDB; PERF-004 was confirmed under both policies.
- Warmup policy: a three-sample smoke run and a 50-sample sequential diagnostic batch ran before the active baseline. The active batch retained all 50 samples per lane.
- Lane ordering: deterministic rotation by iteration: current/baseline/rust-lld, baseline/rust-lld/current, rust-lld/current/baseline. This balances temporal and position effects.
- Noise control: current-llvm and upstream-baseline were byte-identical. Their n=50 averages were 1504.4 ms and 1505.7 ms, a 1.3 ms / 0.09% spread. Average and paired evidence are primary; p50 alone is secondary because machine speed shifted in broad blocks.
- Artifacts: C:/Users/Daniel/linker-bench/results/baseline-702deda99e7a/hauberk-dev.

### Baseline timer averages

| Timer | Average |
|---|---:|
| Total Linking Time | 1400.48 ms |
| Input File Reading | 521.90 ms |
| Input Parse | 445.72 ms |
| Initialize Symbols | 329.60 ms |
| GC | 136.84 ms |
| Code Layout | 104.28 ms |
| PDB Emission cumulative | 477.24 ms |
| Add Objects | 256.50 ms |
| Symbol Merging | 213.34 ms |
| Handle debug S | 138.86 ms |
| Publics Stream Layout | 37.40 ms |
| Commit to Disk | 158.88 ms |
| Rewrite Module Symbols cumulative | 297.34 ms |

### Raw active baseline samples

- current-llvm wall ms: 1627, 1585, 1604, 1589, 1622, 1631, 1541, 1458, 1431, 1422, 1462, 1387, 1388, 1403, 1512, 1594, 1618, 1611, 1582, 1636, 1616, 1609, 1634, 1631, 1608, 1641, 1604, 1627, 1615, 1565, 1564, 1561, 1496, 1538, 1422, 1420, 1385, 1432, 1423, 1408, 1376, 1370, 1345, 1383, 1350, 1419, 1464, 1369, 1299, 1342.
- frozen duplicate wall ms: 1591, 1589, 1596, 1586, 1605, 1537, 1577, 1528, 1428, 1429, 1440, 1387, 1373, 1388, 1605, 1607, 1740, 1604, 1608, 1619, 1636, 1617, 1620, 1612, 1620, 1609, 1630, 1586, 1647, 1549, 1548, 1587, 1518, 1548, 1456, 1418, 1396, 1422, 1407, 1383, 1376, 1380, 1369, 1348, 1393, 1391, 1412, 1314, 1314, 1342.
- rust-lld 22.1.8 wall ms: 1618, 1595, 1630, 1657, 1603, 1690, 1575, 1521, 1455, 1445, 1455, 1403, 1467, 1427, 1380, 1690, 1705, 1637, 1653, 1677, 1672, 1626, 1652, 1658, 1616, 1651, 1667, 1646, 1668, 1599, 1576, 1646, 1574, 1583, 1461, 1485, 1461, 1395, 1430, 1411, 1372, 1385, 1388, 1388, 1379, 1419, 1504, 1433, 1336, 1416.
- current-llvm Total Linking Time ms: 1488, 1487, 1508, 1493, 1523, 1525, 1444, 1362, 1338, 1329, 1359, 1296, 1295, 1312, 1407, 1489, 1478, 1495, 1468, 1532, 1511, 1505, 1528, 1506, 1500, 1534, 1498, 1500, 1454, 1430, 1459, 1459, 1397, 1438, 1325, 1326, 1289, 1340, 1329, 1314, 1282, 1275, 1255, 1284, 1262, 1331, 1318, 1279, 1213, 1255.

## Deltas

### 2026-08-28 18:45 PDT - PERF-000 reproducible Hauberk replay baseline

- Change: added Hauberk --locked target support to the external linker-bench harness, froze the source-built linker, and rotated lane order per iteration.
- Benchmark evidence: current source build averaged 1504.4 ms wall and 1400.48 ms internal over 50 links. Its byte-identical frozen lane averaged 1505.7 ms wall.
- Correctness evidence: all 150 active direct links exited successfully; Cargo manifests were unchanged; capture used one shared materialized response set.
- Decision: accepted as the active baseline and measurement protocol.
- Commit: same commit as PERF-004.

### 2026-08-28 18:40 PDT - MEASURE-001 sequential lane batches

- Change: measured 50 links per lane in three whole sequential batches.
- Benchmark evidence: byte-identical current and frozen lanes reported 1504-200 ms apart at p50 due broad temporal drift; current/frozen/rust-lld averages were 1908, 2088, and 1542 ms.
- Correctness evidence: all links succeeded.
- Decision: rejected for A/B decisions. Raw samples remain in this log's Git history once committed; the active artifact directory now contains the rotated rerun.
- Commit: same commit as PERF-004.

### 2026-08-28 19:20 PDT - PERF-004 direct GC symbol dispatch

- Change: replaced the type-erased recursive std::function in markLive with a statically dispatched generic self-lambda. Traversal order and all mutations are unchanged.
- Production reachability: Driver calls markLive when doGC is enabled. Every GC root, relocation symbol from each live section, and EC entry thunk reaches addSym; import data and import thunks can recurse through impchk exit thunks.
- Mechanism evidence: the RelWithDebInfo MarkLive object shrank from 145,007 to 124,591 bytes. llvm-nm showed the baseline std::_Func_impl_no_alloc vtable and virtual _Do_call; those symbols are absent from the candidate object.
- Existing-output n=50 A/B: wall 1459 versus 1458 ms, Total Linking Time 1356.48 versus 1358.66 ms, GC 128.74 versus 132.88 ms. The GC target improved 4.14 ms and won 43/50 pairs; wall was neutral.
- Existing-output n=100 confirmation: wall 1343 versus 1356 ms, Total Linking Time 1252.22 versus 1263.73 ms, GC 120.28 versus 124.31 ms. Candidate won 63/100 wall pairs and 75/100 GC pairs.
- Clean-output n=100 confirmation: wall min/p50/avg/p95/max 1328/1746/1710.5/2077/2246 ms versus 1337/1748/1723.9/2120/2170 ms. Total Linking Time averaged 1587.79 versus 1600.85 ms; GC averaged 148.84 versus 152.52 ms. Candidate won 62/100 wall pairs and 66/100 GC pairs.
- Correctness evidence: focused associative COMDAT, PDB import GC, symbol-table GC, TLS GC, ARM64EC import, delay-import, entry-thunk, and entry-mangle tests passed 8/8. LLDCOFFTests passed 32/32. Full COFF lit discovered 615 tests: 589 passed, 14 unsupported, 8 failed, and 4 unresolved; all 12 non-passes are incremental-link tests outside markLive, including five Python 3.14 internal-shell failures on the nul device. The failing incremental commands use opt:noref or fail in incremental-state/PDB behavior, so this doGC-only path is unreachable in them.
- Output equivalence: candidate and baseline import libraries were byte-identical. PE headers, section layout, entry point, and data directories matched. Extracted text, data, pdata, tls, and reloc sections were byte-identical. Whole EXE/PDB hashes differ between repeated links because the capture omits /Brepro, producing timestamps/PDB GUIDs and nondeterministic parallel PDB block allocation.
- Decision: accepted. Two independent n=100 batches improved end-to-end wall and internal linker time, while three batches consistently reduced the target GC phase by about 4 ms.
- Commit: this optimization commit.

#### PERF-004 clean-output raw wall samples

- Candidate ms: 1548, 1415, 1413, 1399, 1405, 1371, 1371, 1903, 1971, 1937, 2065, 1794, 2011, 1998, 1934, 1966, 1844, 2067, 1977, 1933, 1953, 2032, 1912, 1835, 2025, 2033, 1986, 2246, 2022, 1930, 1852, 1889, 2108, 1874, 2135, 2077, 1961, 1927, 1915, 2214, 1948, 2060, 2105, 1973, 1891, 1917, 1864, 1845, 1803, 1705, 1746, 1670, 1689, 1654, 1650, 1787, 1843, 1799, 1922, 1878, 1854, 1825, 1782, 1624, 1645, 1568, 1572, 1561, 1511, 1513, 1515, 1455, 1422, 1463, 1443, 1388, 1448, 1388, 1453, 1470, 1465, 1437, 1457, 1453, 1473, 1450, 1447, 1457, 1436, 1472, 1465, 1379, 1354, 1361, 1364, 1344, 1345, 1328, 1334, 1336.
- Frozen baseline ms: 1485, 1477, 1413, 1418, 1407, 1365, 1392, 1772, 1823, 1873, 2013, 1989, 2090, 1943, 2096, 1932, 1743, 1974, 1914, 2120, 1985, 2053, 1972, 1834, 2143, 2170, 2077, 1956, 2100, 2001, 2141, 1956, 2161, 2039, 2118, 2015, 1949, 1993, 2145, 1904, 1946, 2006, 2042, 1943, 1949, 1914, 1893, 1869, 1763, 1748, 1822, 1757, 1677, 1677, 1675, 1811, 1820, 1812, 1885, 1822, 1897, 1864, 1736, 1692, 1665, 1610, 1583, 1575, 1584, 1541, 1491, 1498, 1483, 1468, 1423, 1416, 1361, 1355, 1459, 1493, 1467, 1438, 1465, 1460, 1483, 1456, 1426, 1476, 1433, 1467, 1462, 1432, 1375, 1393, 1395, 1361, 1337, 1372, 1363, 1354.

## Candidate Inventory

| ID | Hypothesis | Scope | Status | Evidence |
|---|---|---|---|---|
| PERF-001 | Reduce repeated work in the live regular-object symbol initialization path | lld/COFF/InputFiles.cpp and SymbolTable.cpp | Survey | Initialize Symbols averages 329.60 ms; avoid archived prehash/reserve/store-guard variants |
| PERF-002 | Cut a narrow currently repeated operation in debug S symbol merging | lld/COFF/PDB.cpp | Survey | Handle debug S averages 138.86 ms; broad buffering and replay plans are archived failures |
| PERF-003 | Reduce allocation or lookup overhead in module-symbol rewrite without changing storage family | lld/COFF/PDB.cpp | Survey | Rewrite Module Symbols averages 297.34 ms cumulative; SmallVector and broad batching lost |
| PERF-004 | Remove type-erased dispatch from reachable GC symbol traversal | lld/COFF/MarkLive.cpp | Accepted | Three A/B batches reduced GC about 4 ms; two n=100 batches improved wall time |
| PERF-005 | Reduce output section/layout work on the Hauberk graph | lld/COFF/Writer.cpp | Survey | Code Layout averages 104.28 ms |
| PERF-006 | Improve benchmark diagnostics with paired lane deltas and timer summaries | external linker-bench harness | Rotation and clean-output policy active | Rotated duplicate lanes reduced average drift to 0.09% |

## Commit History

| Commit | Candidate | Result |
|---|---|---|
| This commit | PERF-000, PERF-004 | Baseline records plus validated GC dispatch optimization |
