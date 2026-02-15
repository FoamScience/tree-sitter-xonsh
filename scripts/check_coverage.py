#!/usr/bin/env python3
"""Validate tree-sitter-xonsh coverage of xonsh PLY grammar constructs.

Reads grammar.js rule names and checks that every "done" construct
references rules that actually exist. Prints a summary report.

Exit code 1 if any "done" construct references a missing rule.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GRAMMAR_JS = REPO_ROOT / "grammar.js"

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

    return 0


if __name__ == "__main__":
    sys.exit(main())
