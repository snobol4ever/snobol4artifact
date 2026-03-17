# snobol4artifact
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL_v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)

CPython C extension: SNOBOL4 pattern match engine.

Python builds the pattern tree using [SNOBOL4python](https://github.com/snobol4ever/snobol4python)
objects. C runs the Byrd Box engine. Python gets `(start, end)` or `None`.

```python
from SNOBOL4python.SNOBOL4patterns import POS, RPOS, σ, Π
import snobol4c

BEAD = POS(0) + (σ("B") | σ("R")) + (σ("E") | σ("EA")) + (σ("D") | σ("DS")) + RPOS(0)

snobol4c.match(BEAD, "BEAD")    # → (0, 4)
snobol4c.match(BEAD, "READS")   # → (0, 5)
snobol4c.match(BEAD, "XYZ")     # → None

snobol4c.search(σ("world"), "hello world!")   # → (6, 11)
snobol4c.fullmatch(σ("hello"), "hello")       # → (0, 5)
```

## API

| Function | Description |
|----------|-------------|
| `match(pattern, subject)` | Match at position 0 → `(start, end)` or `None` |
| `search(pattern, subject)` | Scan for first match anywhere → `(start, end)` or `None` |
| `fullmatch(pattern, subject)` | Match entire subject → `(start, end)` or `None` |

## Build

```bash
pip install -e .
# or
python3 setup.py build_ext --inplace
```

Requires Python 3.8+. No dependencies beyond SNOBOL4python for the test suite.

## Architecture

Python builds a `SNOBOL4python` pattern tree using operator overloading
(`+` → Σ sequence, `|` → Π alternation, `~` → π optional).  
The C extension walks that tree, allocates a mirror C struct tree, and runs
the full **Psi/Omega Byrd Box engine** — the same engine described in:

> Kshemkalyani, A. and Shenoy, R., "A Generalized Pattern Matching Algorithm",
> *ACM Transactions on Programming Languages and Systems*, 1995.

The engine handles: `LITERAL`, `ANY`, `NOTANY`, `SPAN`, `BREAK`, `POS`, `RPOS`,
`LEN`, `TAB`, `RTAB`, `REM`, `ARB`, `ARBNO`, `BAL`, `FENCE`, `FAIL`, `ABORT`,
`SUCCEED`, `ε`, `α`, `ω`, `Σ`, `Π`, `ρ`, `π`.

## History

| Version | Memory model | Lines | Notes |
|---------|-------------|-------|-------|
| v1 | Arena bump allocator — slab of Pattern nodes, freed all at once | 788 | First working proof-of-concept |
| v2 | Per-node `malloc` + `PatternList` tracker | 721 | Cleaner; no relocation issues; this is the current version |

`engine.c` in [snobol4x](https://github.com/snobol4ever/snobol4x)
was extracted from v2 of this file.

## Relation to SPIPAT

[SNOBOL4python](https://github.com/snobol4ever/snobol4python) currently uses
SPIPAT as its C backend. snobol4artifact is an alternative backend — same
pattern tree, different engine. Long-term it is a candidate to replace SPIPAT
with a snobol4ever-owned implementation.
