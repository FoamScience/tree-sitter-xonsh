# Xonsh Compatibility Reference

This document tracks which xonsh-specific syntax constructs are supported by `tree-sitter-xonsh`, mapped against the PLY grammar rules in [xonsh's parser](https://github.com/xonsh/xonsh/blob/main/xonsh/parsers/v310.py).

> The main reason this grammar exists is the fail-tolerance nature of Tree-Sitter. If not for this,
> parsing with xonsh itself would have been better

## Target Versions

- **Xonsh**: 0.22.x
- **Python**: 3.10+ (`tree-sitter-python` base grammar)

Parsing compatibility level can be checked by running `uv run scripts/check_coverage.py`

## Expressions

| Xonsh Syntax | PLY Rule(s) | tree-sitter Rule(s) | Status |
|---|---|---|---|
| `$VAR` | `p_atom_envvar` | `env_variable` | Done |
| `${expr}` | `p_atom_envvar_braced` | `env_variable_braced` | Done |
| `$(cmd)` | `p_atom_subproc_captured` | `captured_subprocess` | Done |
| `!(cmd)` | `p_atom_subproc_captured_object` | `captured_subprocess_object` | Done |
| `$[cmd]` | `p_atom_subproc_uncaptured` | `uncaptured_subprocess` | Done |
| `![cmd]` | `p_atom_subproc_uncaptured_object` | `uncaptured_subprocess_object` | Done |
| `@(expr)` | `p_atom_pyeval` | `python_evaluation` | Done |
| `@$(cmd)` | `p_atom_subproc_inject` | `tokenized_substitution` | Done |
| `@.attr` | `p_atom_at_attribute` | `at_object` | Done |
| `` `pattern` `` | `p_atom_re_glob` | `regex_glob` | Done |
| `` r`pattern` `` | `p_atom_re_glob` | `regex_glob` | Done |
| `` rp`pattern` `` | `p_atom_re_glob_path` | `regex_path_glob` | Done |
| `` g`pattern` `` | `p_atom_glob` | `glob_pattern` | Done |
| `` gp`pattern` `` | `p_atom_glob_path` | `glob_path` | Done |
| `` f`pattern` `` | `p_atom_fglob` | `formatted_glob` | Done |
| `` @func`pattern` `` | `p_atom_custom_glob` | `custom_function_glob` | Done |
| `p"..."` / `pf"..."` / `pr"..."` | `p_atom_pathobj` | `path_string` | Done |
| `func!(args)` | `p_atom_bang_lfunc` | `macro_call` | Done |
| `expr?` | `p_help_expression` | `help_expression` | Done |
| `expr??` | `p_super_help_expression` | `super_help_expression` | Done |
| `@@.name` decorator | `p_decorator_atat_*` | `at_object` (inside `decorator`) | Done |
| `@modifier cmd` (in subprocess) | `p_subproc_atom_modifier` | `subprocess_modifier` | Done |

## Statements

| Xonsh Syntax | PLY Rule(s) | tree-sitter Rule(s) | Status |
|---|---|---|---|
| `$VAR = val` | `p_env_assignment` | `env_assignment` | Done |
| `del $VAR` | `p_env_deletion` | `env_deletion` | Done |
| `$VAR=val cmd` | `p_env_scoped_command` | `env_scoped_command` | Done |
| `$VAR="val"` (no-space) | `p_env_assignment` | `env_prefix_statement` | Done |
| `xontrib load name` | `p_xontrib_statement` | `xontrib_statement` | Done |
| `cmd! args` | `p_subproc_macro` | `subprocess_macro` | Done |
| `with! ctx:` | `p_block_macro` | `block_macro_statement` | Done |
| Bare subprocess | `p_subproc_bare` | `bare_subprocess` | Done |

## Subprocess Internals

| Feature | PLY Rule(s) | tree-sitter Rule(s) | Status |
|---|---|---|---|
| Pipe `\|` | `p_subproc_pipe` | `pipe_operator`, `subprocess_pipeline` | Done |
| Stderr pipe `e\|` / `err\|` | `p_subproc_pipe` | `pipe_operator` | Done |
| All pipe `a\|` / `all\|` | `p_subproc_pipe` | `pipe_operator` | Done |
| Logical `&&` / `\|\|` | `p_subproc_logical` | `logical_operator`, `subprocess_logical` | Done |
| Keyword `and` / `or` | `p_subproc_logical` | `logical_operator` (via scanner) | Done |
| Redirect `>` `>>` `<` | `p_subproc_redirect` | `redirect_operator`, `subprocess_redirect` | Done |
| Numbered fd `1>` `2>` etc. | `p_subproc_redirect` | `redirect_operator` | Done |
| Named redirect `o>` `e>` `out>` `err>` `all>` `a>` | `p_subproc_redirect` | `redirect_operator` | Done |
| Stream merge `2>&1` `err>out` etc. | `p_subproc_redirect` | `stream_merge_operator` | Done |
| Background `&` | `p_subproc_background` | `background_command`, `bare_subprocess` | Done |
| Brace expansion `{a,b}` `{1..5}` | `p_subproc_brace_expansion` | `brace_expansion` | Done |
| Boolean `&&`/`\|\|` at Python level | `p_or_test` / `p_and_test` | `boolean_operator` (override) | Done |

## Known Limitations

| Xonsh Syntax | PLY Rule(s) | Notes |
|---|---|---|
| `$(cmd !)` | `p_atom_bang_empty_fistful_of_dollars` | Trailing `!` before closer appends empty string arg |
| `$(cmd ! raw text)` | `p_atom_bang_fistful_of_dollars` | `!` acts as raw-string boundary; text between `!` and closer is passed verbatim |

These rules handle `!` appearing **inside** subprocess operators (`$(...)`, `!(...)`, `$[...]`, `![...]`) as a raw-string boundary marker. The "empty" variant (`$(cmd !)`) appends `""` to the argument list. The "nocloser" variant (`$(cmd ! arbitrary {raw} text)`) captures everything between `!` and the closing delimiter as a literal string, bypassing normal tokenization. Both are extremely rare in practice.

## Architectural Differences

### Context-Free vs Context-Sensitive Parsing

Xonsh's CPython-based parser uses a **context-sensitive** approach:

1. **PLY parser** produces an initial AST
2. **`CtxAwareTransformer`** walks the AST, using execution context (registered aliases, `$PATH` commands, callable detection) to decide whether bare identifiers are subprocess calls or Python expressions

Tree-sitter is a **context-free** parser. It cannot know at parse time whether `myapp` is a registered alias or a Python variable. Instead:

- **Known commands** (those with flags like `--version`, path-like arguments, pipes, etc.) are detected by the external scanner heuristics and parsed as `bare_subprocess`
- **Unknown commands** without shell-like arguments are parsed as Python `identifier` / `expression_statement`
- **Workaround**: Use explicit subprocess operators (`$[cmd]`, `$(cmd)`) for commands the scanner cannot detect

> The `xonsh-lsp` mitigates some of these issues, so these limitations are eventually transparent to the user.

### Subprocess Detection

The external scanner (`scanner.c`) uses heuristic pattern matching to detect bare subprocesses:

- Flags (`-f`, `--flag`)
- Path-like arguments (`./script`, `/usr/bin/cmd`)
- Pipe operators (`|`, `e|`, `a|`)
- Redirect operators (`>`, `>>`, `2>`)
- `@modifier` prefix (e.g., `@json curl`)
- Known common commands (hardcoded list in scanner)
