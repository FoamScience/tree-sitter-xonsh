#!/usr/bin/env python3
"""Validate tree-sitter-xonsh coverage of xonsh PLY grammar constructs.

Two checks:

1. **Internal consistency** — every ``done`` construct must reference rules
   that exist in ``grammar.js``. Exits 1 if any are missing.

2. **Upstream drift** — when ``xonsh`` is importable, the script also
   compares ``XONSH_CONSTRUCTS`` against the ``p_*`` rules in xonsh's
   parser modules. Reports:
     - PLY rules listed here but no longer present upstream (renamed/removed)
     - xonsh-flavoured rules added upstream but not yet tracked here
     - installed version vs the pinned ``TARGET_XONSH`` range
   Drift is informational only — it warns, never fails.

Run from a venv where xonsh is installed for the upstream check:

    uv run --with "xonsh==0.24.1" scripts/check_coverage.py
"""

import ast
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GRAMMAR_JS = REPO_ROOT / "grammar.js"

# Pinned upstream xonsh range. Bump alongside XONSH_COMPAT.md when the
# project tracks a new release; the script warns on mismatch but does not
# fail (downstream consumers may be on older xonsh).
TARGET_XONSH = "0.24"

# Each entry: (name, ply_rule, ts_rules, status, notes)
# status: "done", "partial", or "gap"
XONSH_CONSTRUCTS = [
    # === Expressions ===
    (
        "$VAR",
        "p_atom_envvar",
        ["env_variable"],
        "done",
        "",
    ),
    (
        "${expr}",
        "p_atom_envvar_braced",
        ["env_variable_braced"],
        "done",
        "",
    ),
    (
        "$(cmd)",
        "p_atom_subproc_captured",
        ["captured_subprocess"],
        "done",
        "",
    ),
    (
        "!(cmd)",
        "p_atom_subproc_captured_object",
        ["captured_subprocess_object"],
        "done",
        "",
    ),
    (
        "$[cmd]",
        "p_atom_subproc_uncaptured",
        ["uncaptured_subprocess"],
        "done",
        "",
    ),
    (
        "![cmd]",
        "p_atom_subproc_uncaptured_object",
        ["uncaptured_subprocess_object"],
        "done",
        "",
    ),
    (
        "@(expr)",
        "p_atom_pyeval",
        ["python_evaluation"],
        "done",
        "",
    ),
    (
        "@$(cmd)",
        "p_atom_subproc_inject",
        ["tokenized_substitution"],
        "done",
        "",
    ),
    (
        "@!(raw)",
        "p_subproc_atom_pyeval_macro",
        ["pyeval_macro"],
        "done",
        "Subprocess pyeval macro; added in xonsh 0.23.",
    ),
    (
        "@.attr",
        "p_atom_at_attribute",
        ["at_object"],
        "done",
        "",
    ),
    (
        "`pattern`",
        "p_atom_re_glob",
        ["regex_glob"],
        "done",
        "",
    ),
    (
        "rp`pattern`",
        "p_atom_re_glob_path",
        ["regex_path_glob"],
        "done",
        "",
    ),
    (
        "g`pattern`",
        "p_atom_glob",
        ["glob_pattern"],
        "done",
        "",
    ),
    (
        "gp`pattern`",
        "p_atom_glob_path",
        ["glob_path"],
        "done",
        "",
    ),
    (
        "f`pattern`",
        "p_atom_fglob",
        ["formatted_glob"],
        "done",
        "",
    ),
    (
        "@func`pattern`",
        "p_atom_custom_glob",
        ["custom_function_glob"],
        "done",
        "",
    ),
    (
        'p"..."',
        "p_atom_pathobj",
        ["path_string"],
        "done",
        "",
    ),
    (
        "func!(args)",
        "p_atom_bang_lfunc",
        ["macro_call"],
        "done",
        "",
    ),
    (
        "expr?",
        "p_help_expression",
        ["help_expression"],
        "done",
        "",
    ),
    (
        "expr??",
        "p_super_help_expression",
        ["super_help_expression"],
        "done",
        "",
    ),
    (
        "@@.name decorator",
        "p_decorator_atat_*",
        ["at_object"],
        "done",
        "at_object reused inside decorator context",
    ),
    (
        "@modifier cmd",
        "p_subproc_atom_modifier",
        ["subprocess_modifier"],
        "done",
        "In subprocess context",
    ),
    # === Statements ===
    (
        "$VAR = val",
        "p_env_assignment",
        ["env_assignment"],
        "done",
        "",
    ),
    (
        "del $VAR",
        "p_env_deletion",
        ["env_deletion"],
        "done",
        "",
    ),
    (
        "$VAR=val cmd",
        "p_env_scoped_command",
        ["env_scoped_command", "env_prefix"],
        "done",
        "",
    ),
    (
        '$VAR="val"',
        "p_env_assignment",
        ["env_prefix_statement"],
        "done",
        "No-space assignment variant",
    ),
    (
        "xontrib load name",
        "p_xontrib_statement",
        ["xontrib_statement"],
        "done",
        "",
    ),
    (
        "cmd! args",
        "p_subproc_macro",
        ["subprocess_macro"],
        "done",
        "",
    ),
    (
        "with! ctx:",
        "p_block_macro",
        ["block_macro_statement"],
        "done",
        "",
    ),
    (
        "bare subprocess",
        "p_subproc_bare",
        ["bare_subprocess"],
        "done",
        "Detected by scanner heuristics",
    ),
    # === Subprocess internals ===
    (
        "pipe |",
        "p_subproc_pipe",
        ["pipe_operator", "subprocess_pipeline"],
        "done",
        "",
    ),
    (
        "stderr pipe e|",
        "p_subproc_pipe",
        ["pipe_operator"],
        "done",
        "",
    ),
    (
        "logical && ||",
        "p_subproc_logical",
        ["logical_operator", "subprocess_logical"],
        "done",
        "",
    ),
    (
        "redirect > >> <",
        "p_subproc_redirect",
        ["redirect_operator", "subprocess_redirect"],
        "done",
        "",
    ),
    (
        "stream merge 2>&1",
        "p_subproc_redirect",
        ["stream_merge_operator"],
        "done",
        "",
    ),
    (
        "background &",
        "p_subproc_background",
        ["background_command"],
        "done",
        "",
    ),
    (
        "brace expansion {a,b}",
        "p_subproc_brace_expansion",
        ["brace_expansion"],
        "done",
        "",
    ),
    (
        "boolean && || at Python level",
        "p_or_test / p_and_test",
        ["boolean_operator"],
        "done",
        "Override of Python boolean_operator",
    ),
    # === Known gaps ===
    (
        "trailing ! in subprocess (empty)",
        "p_atom_bang_empty_fistful_of_dollars",
        [],
        "gap",
        "e.g. $(cmd !) — trailing ! before closer appends empty string arg",
    ),
    (
        "trailing ! in subprocess (raw text)",
        "p_atom_bang_fistful_of_dollars",
        [],
        "gap",
        "e.g. $(cmd ! raw text) — ! acts as raw-string boundary before closer",
    ),
    (
        "f-string format spec starting with =",
        "p_fstring_format_spec",
        [],
        "gap",
        'e.g. f"{v:=>10}" — tree-sitter-python (through 0.25) reads ":=" as walrus',
    ),
]


def extract_grammar_rules(grammar_text: str) -> set[str]:
    """Extract rule names from grammar.js.

    Matches patterns like:
      ruleName: $ =>
      ruleName: _ =>
      ruleName: ($, original) =>
    """
    pattern = re.compile(r"^\s+(\w+)\s*:\s*(?:[\$_]|\(\$(?:,\s*original)?\))\s*=>", re.MULTILINE)
    return set(pattern.findall(grammar_text))


# ---------------------------------------------------------------------------
# Upstream drift detection
# ---------------------------------------------------------------------------
#
# The ply_rule column in XONSH_CONSTRUCTS uses *conceptual* names, not
# upstream's actual ``p_*`` method names — so we can't diff it directly. Instead
# the script snapshots the set of xonsh-flavoured rules at the pinned target
# release (BASELINE_RULES below) and reports anything added/removed in the
# installed version. Refresh the snapshot when bumping TARGET_XONSH.

# Substrings that mark a PLY rule as xonsh-specific (not pure Python grammar).
# Conservative: false negatives are acceptable; false positives create noise.
_XONSH_RULE_HINTS = (
    "envvar", "subproc", "xontrib", "pyeval", "pathobj",
    "fistful", "bang", "atat", "xonsh",
    "env_assignment", "env_deletion", "env_scoped",
    "help_expression", "super_help",
    "block_macro", "atom_re_glob", "atom_glob", "atom_fglob",
    "atom_custom_glob", "atom_glob_path", "atom_re_glob_path",
    "atom_at_attribute", "with_bang",
)

# Snapshot of xonsh-flavoured ``p_*`` rules in the pinned target release.
# Generated by running this script against xonsh 0.24.1. To refresh:
#   uv run --with "xonsh==<new>" scripts/check_coverage.py --dump-baseline
BASELINE_XONSH = "0.24.1"
BASELINE_RULES: frozenset[str] = frozenset({
    "p_atom_bang_empty_fistful_of_dollars",
    "p_atom_bang_fistful_of_dollars",
    "p_atom_fistful_of_dollars",
    "p_decorator_atat_call_chain",
    "p_decorator_atat_call_simple",
    "p_decorator_atat_nocall_chain",
    "p_decorator_atat_nocall_simple",
    "p_envvar_assign",
    "p_envvar_assign_left",
    "p_envvar_assign_subproc_atoms",
    "p_subproc_amper",
    "p_subproc_arg_many",
    "p_subproc_arg_part",
    "p_subproc_arg_part_brackets",
    "p_subproc_arg_part_brackets_empty",
    "p_subproc_arg_single",
    "p_subproc_atom_arg",
    "p_subproc_atom_captured_stdout",
    "p_subproc_atom_captured_stdout_bang",
    "p_subproc_atom_captured_stdout_bang_empty",
    "p_subproc_atom_pyenv_lookup",
    "p_subproc_atom_pyeval",
    "p_subproc_atom_pyeval_macro",
    "p_subproc_atom_re",
    "p_subproc_atom_redirect",
    "p_subproc_atom_str",
    "p_subproc_atom_subproc_inject",
    "p_subproc_atom_subproc_inject_bang",
    "p_subproc_atom_subproc_inject_bang_empty",
    "p_subproc_atom_uncaptured",
    "p_subproc_atom_uncaptured_bang",
    "p_subproc_atom_uncaptured_bang_empty",
    "p_subproc_atoms_many",
    "p_subproc_atoms_single",
    "p_subproc_atoms_subshell",
    "p_subproc_pipe",
    "p_subproc_s1",
    "p_subproc_s2",
    "p_trailer_bang_lparen",
    "p_with_bang_stmt_many_suite",
    "p_with_bang_stmt_single_suite",
})


def _collect_upstream_xonsh_rules() -> tuple[set[str], str | None]:
    """Return (xonsh-flavoured p_* rules, installed xonsh version) or
    (set(), None) if xonsh isn't importable."""
    try:
        import xonsh  # type: ignore[import-not-found]
    except ImportError:
        return set(), None

    parsers_dir = Path(xonsh.__file__).resolve().parent / "parsers"
    rules: set[str] = set()
    for py in parsers_dir.glob("*.py"):
        try:
            tree = ast.parse(py.read_text(), filename=str(py))
        except SyntaxError:
            continue
        for node in ast.walk(tree):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name.startswith("p_") and any(h in node.name for h in _XONSH_RULE_HINTS):
                    rules.add(node.name)
    return rules, getattr(xonsh, "__version__", None)


def report_upstream_drift(dump_baseline: bool = False) -> None:
    upstream, installed_version = _collect_upstream_xonsh_rules()
    if not upstream:
        print(
            "\nUpstream check skipped: xonsh is not installed in this venv.\n"
            "  Run with `uv run --with xonsh scripts/check_coverage.py` to enable."
        )
        return

    if dump_baseline:
        print("\n# Paste into BASELINE_RULES; bump BASELINE_XONSH to:", installed_version)
        print("BASELINE_RULES = frozenset({")
        for r in sorted(upstream):
            print(f"    {r!r},")
        print("})")
        return

    print(
        f"\nUpstream xonsh check (installed: {installed_version}, "
        f"target ~={TARGET_XONSH}, baseline snapshot: {BASELINE_XONSH}):"
    )

    if installed_version and not installed_version.startswith(TARGET_XONSH + "."):
        print(
            f"  WARNING: installed xonsh {installed_version} does not match the\n"
            f"           pinned target {TARGET_XONSH}.x — bump TARGET_XONSH and\n"
            f"           XONSH_COMPAT.md if upstream has moved."
        )

    added = sorted(upstream - BASELINE_RULES)
    removed = sorted(BASELINE_RULES - upstream)

    if added:
        print(f"  NEW xonsh-flavoured rules upstream (since {BASELINE_XONSH}):")
        for r in added:
            print(f"    + {r}")
        print("  → review whether tree-sitter-xonsh covers these constructs.")

    if removed:
        print(f"  REMOVED xonsh-flavoured rules upstream (since {BASELINE_XONSH}):")
        for r in removed:
            print(f"    - {r}")
        print("  → references to these rules in XONSH_COMPAT.md may be stale.")

    if not added and not removed:
        print(f"  No drift since baseline snapshot {BASELINE_XONSH}.")


def main() -> int:
    if not GRAMMAR_JS.exists():
        print(f"ERROR: {GRAMMAR_JS} not found", file=sys.stderr)
        return 1

    grammar_text = GRAMMAR_JS.read_text()
    grammar_rules = extract_grammar_rules(grammar_text)

    done = []
    partial = []
    gaps = []
    missing_rules: list[tuple[str, str]] = []

    for name, ply_rule, ts_rules, status, notes in XONSH_CONSTRUCTS:
        if status == "done":
            done.append(name)
            for rule in ts_rules:
                if rule not in grammar_rules:
                    missing_rules.append((name, rule))
        elif status == "partial":
            partial.append(name)
        elif status == "gap":
            gaps.append(name)

    total = len(XONSH_CONSTRUCTS)
    done_count = len(done)
    partial_count = len(partial)
    gap_count = len(gaps)
    pct = done_count / total * 100 if total else 0

    print(f"Total constructs checked: {total}")
    print(f"Done:    {done_count:3d}  ({pct:.0f}%)")
    print(f"Partial: {partial_count:3d}")
    print(f"Gap:     {gap_count:3d}")

    if gaps:
        print("\nKnown gaps:")
        for g in gaps:
            # Find the notes for this gap
            for name, _, _, status, notes in XONSH_CONSTRUCTS:
                if name == g and status == "gap":
                    print(f"  - {g}: {notes}" if notes else f"  - {g}")
                    break

    if missing_rules:
        print("\nERROR: 'done' constructs reference missing grammar rules:")
        for name, rule in missing_rules:
            print(f"  - {name!r} references {rule!r} not found in grammar.js")
        return 1

    if not missing_rules:
        print("\nAll 'done' rules validated against grammar.js.")

    report_upstream_drift(dump_baseline="--dump-baseline" in sys.argv)

    return 0


if __name__ == "__main__":
    sys.exit(main())
