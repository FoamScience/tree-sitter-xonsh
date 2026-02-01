# Xonsh grammar for TreeSitter

A [tree-sitter](https://tree-sitter.github.io/) grammar for [xonsh](https://xon.sh/), the Python-powered shell.

## Overview

Xonsh extends Python 3 with shell-like syntax for subprocess execution. This grammar extends `tree-sitter-python` with xonsh-specific constructs.

> [!IMPORTANT]
> - This should be treated as experimental beta-stage software. The output tree layout would change.
> - Some limitations are forced by the fact that tree-sitter is context-free while some xonsh constructs are resolvable only at runtime.

## Installation

### Building from source
```bash
git clone https://github.com/FoamScience/tree-sitter-xonsh
cd tree-sitter-xonsh
npm install
tree-sitter generate
tree-sitter parse <your_file>.xsh
```

## Known Limitations

1. **Unknown commands parsed as Python** instead of a bare subprocess command.
   - Workaround: Use explicit subprocess syntax: `$[mycommand]` instead of just `mycommand`
   - This is an effect of scanner-based approaches, for context-bound xonsh subprocesses.

## Architecture

This grammar extends [tree-sitter-python](https://github.com/tree-sitter/tree-sitter-python). Key components:

- **grammar.js**: Defines xonsh-specific rules and overrides Python rules where needed
- **scanner.c**: External scanner for:
  - Bare subprocess detection (heuristic-based)
  - `@` symbol usage disambiguation (decorator vs `@(...)` vs `@.attr` vs `@modifier`)
  - Subprocess modifier handling (`@json`, `@unthread`, etc.)
  - `&&`/`||` vs `&` disambiguation
  - Brace expansion vs literal detection
  - Python's indent/dedent handling (inherited)
  - String delimiter handling (inherited)
- **queries/highlights.scm** provides syntax highlighting queries for Neovim.
  - The TreeSitter CLI can read those, but will render the highlighting differently.

> Currently the scanner may look-ahead a whole line, which can affect performance.

## License

MIT
