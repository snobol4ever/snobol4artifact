#!/usr/bin/env python3
"""
BEAD end-to-end test: Python builds PATTERN tree → C engine matches → Python gets result.

One small step for man, one giant leap for mankind.
"""
import sys, os
# Run from repo root: python -m pytest tests/ or python tests/test_bead.py
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from SNOBOL4python.SNOBOL4patterns import POS, RPOS, LEN, TAB, RTAB, REM
from SNOBOL4python.SNOBOL4patterns import ANY, NOTANY, SPAN, BREAK
from SNOBOL4python.SNOBOL4patterns import ARB, ARBNO, BAL, FENCE
from SNOBOL4python.SNOBOL4patterns import FAIL, ABORT, SUCCEED
from SNOBOL4python.SNOBOL4patterns import Σ, Π, σ, ε, π, α, ω
import snobol4c

passed = 0
failed = 0

def test(name, pattern, subject, expected):
    global passed, failed
    result = snobol4c.match(pattern, subject)
    ok = (result == expected)
    status = "✅" if ok else "❌"
    print(f"  {status} {name:40s}  match({subject!r:20s}) → {str(result):16s}  expected {str(expected):16s}")
    if ok:
        passed += 1
    else:
        failed += 1

# ======================================================================================
print("═" * 78)
print("  SNOBOL4c CPython Extension — End-to-End Tests")
print("═" * 78)

# --- BEAD: the first test ---
print("\n── BEAD Pattern ──")
BEAD = POS(0) + (σ("B") | σ("R")) + (σ("E") | σ("EA")) + (σ("D") | σ("DS")) + RPOS(0)

test("BEAD/BEAD",   BEAD, "BEAD",   (0, 4))
test("BEAD/BEADS",  BEAD, "BEADS",  (0, 5))
test("BEAD/READS",  BEAD, "READS",  (0, 5))
test("BEAD/READ",   BEAD, "READ",   (0, 4))
test("BEAD/RED",    BEAD, "RED",    (0, 3))
test("BEAD/BED",    BEAD, "BED",    (0, 3))
test("BEAD/BEDS",   BEAD, "BEDS",   (0, 4))
test("BEAD/XYZ",    BEAD, "XYZ",    None)
test("BEAD/empty",  BEAD, "",       None)
test("BEAD/B",      BEAD, "B",      None)

# --- BEARDS: more complex alternation ---
print("\n── BEARDS Pattern ──")
BEARDS = ( POS(0)
         + ( (σ("BE") | σ("BEA") | σ("BEAR")) + (σ("DS") | σ("D"))
           | (σ("RO") | σ("ROO") | σ("ROOS")) + (σ("TS") | σ("T"))
           )
         + RPOS(0)
         )

test("BEARDS/BEARDS",  BEARDS, "BEARDS",  (0, 6))
test("BEARDS/BEARD",   BEARDS, "BEARD",   (0, 5))
test("BEARDS/BED",     BEARDS, "BED",     (0, 3))
test("BEARDS/ROOSTS",  BEARDS, "ROOSTS",  (0, 6))
test("BEARDS/ROOT",    BEARDS, "ROOT",    (0, 4))
test("BEARDS/ROT",     BEARDS, "ROT",     (0, 3))
test("BEARDS/XYZ",     BEARDS, "XYZ",     None)

# --- Simple literal ---
print("\n── Literal σ ──")
HELLO = POS(0) + σ("hello") + RPOS(0)
test("σ/hello",     HELLO, "hello",   (0, 5))
test("σ/world",     HELLO, "world",   None)
test("σ/hell",      HELLO, "hell",    None)
test("σ/helloo",    HELLO, "helloo",  None)

# --- ANY / SPAN / BREAK ---
print("\n── ANY / SPAN / BREAK ──")
DIGITS_PAT = POS(0) + SPAN("0123456789") + RPOS(0)
test("SPAN digits/123",    DIGITS_PAT, "123",    (0, 3))
test("SPAN digits/abc",    DIGITS_PAT, "abc",    None)
test("SPAN digits/12x",    DIGITS_PAT, "12x",    None)

WORD = POS(0) + ANY("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + RPOS(0)
test("ANY upper/A",    WORD, "A",   (0, 1))
test("ANY upper/a",    WORD, "a",   None)

UPTO = POS(0) + BREAK("+") + σ("+") + RPOS(0)
test("BREAK +/abc+",   UPTO, "abc+",   (0, 4))
test("BREAK +/abc",    UPTO, "abc",    None)

# --- LEN / TAB / RTAB / REM ---
print("\n── LEN / TAB / RTAB / REM ──")
LEN3 = POS(0) + LEN(3) + RPOS(0)
test("LEN 3/abc",    LEN3, "abc",    (0, 3))
test("LEN 3/ab",     LEN3, "ab",     None)
test("LEN 3/abcd",   LEN3, "abcd",   None)

TAB3 = POS(0) + TAB(3) + RPOS(0)
test("TAB 3/abc",    TAB3, "abc",    (0, 3))
test("TAB 3/ab",     TAB3, "ab",     None)

REM_PAT = POS(0) + σ("AB") + REM() + RPOS(0)
test("REM/ABCDE",    REM_PAT, "ABCDE",  (0, 5))
test("REM/AB",       REM_PAT, "AB",     (0, 2))
test("REM/X",        REM_PAT, "X",      None)

# --- ARB ---
print("\n── ARB ──")
ARB_PAT = POS(0) + ARB() + RPOS(0)
test("ARB/empty",    ARB_PAT, "",      (0, 0))
test("ARB/x",        ARB_PAT, "x",     (0, 1))
test("ARB/xyz",      ARB_PAT, "xyz",   (0, 3))

# --- ARBNO ---
print("\n── ARBNO ──")
ARBNO_PAT = POS(0) + ARBNO(σ("ab")) + RPOS(0)
test("ARBNO ab/empty",    ARBNO_PAT, "",       (0, 0))
test("ARBNO ab/ab",       ARBNO_PAT, "ab",     (0, 2))
test("ARBNO ab/abab",     ARBNO_PAT, "abab",   (0, 4))
test("ARBNO ab/ababab",   ARBNO_PAT, "ababab", (0, 6))
test("ARBNO ab/abc",      ARBNO_PAT, "abc",    None)

# --- ε (epsilon) ---
print("\n── ε (epsilon) ──")
EPS = POS(0) + ε() + RPOS(0)
test("ε/empty",     EPS, "",     (0, 0))
test("ε/x",         EPS, "x",   None)

# --- π (optional) ---
print("\n── π (optional) ──")
OPT = POS(0) + ~σ("X") + RPOS(0)
test("~σ(X)/empty",  OPT, "",    (0, 0))
test("~σ(X)/X",      OPT, "X",   (0, 1))
test("~σ(X)/Y",      OPT, "Y",   None)

# --- FENCE ---
print("\n── FENCE ──")
FENCED = POS(0) + FENCE() + σ("AB") + RPOS(0)
test("FENCE/AB",     FENCED, "AB",    (0, 2))

# --- BAL ---
print("\n── BAL ──")
BAL_PAT = POS(0) + BAL() + RPOS(0)
test("BAL/(A+B)",    BAL_PAT, "(A+B)",   (0, 5))
test("BAL/A+B",      BAL_PAT, "A+B",     (0, 3))
test("BAL/((A))",    BAL_PAT, "((A))",   (0, 5))

# --- FAIL / SUCCEED ---
print("\n── FAIL / SUCCEED ──")
FAIL_PAT = POS(0) + FAIL() + RPOS(0)
test("FAIL/x",      FAIL_PAT, "x",      None)

# --- NOTANY ---
print("\n── NOTANY ──")
NA = POS(0) + NOTANY("0123456789") + RPOS(0)
test("NOTANY digit/a",  NA, "a",   (0, 1))
test("NOTANY digit/5",  NA, "5",   None)

# --- search() (unanchored) ---
print("\n── search() (unanchored) ──")
PAT = σ("world")
result = snobol4c.search(PAT, "hello world!")
test_name = "search/hello world!"
ok = (result == (6, 11))
status = "✅" if ok else "❌"
print(f"  {status} {test_name:40s}  search('hello world!') → {result}  expected (6, 11)")
if ok: passed += 1
else: failed += 1

# ======================================================================================
print("\n" + "═" * 78)
print(f"  Results: {passed} passed, {failed} failed, {passed + failed} total")
print("═" * 78)

if failed == 0:
    print("\n  🚀 One small step for man, one giant leap for mankind.\n")
else:
    print(f"\n  ⚠️  {failed} test(s) need attention.\n")

sys.exit(0 if failed == 0 else 1)
