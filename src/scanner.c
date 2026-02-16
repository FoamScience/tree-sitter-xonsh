/**
 * External scanner for tree-sitter-xonsh
 * Based on tree-sitter-python's scanner with xonsh extensions
 */

#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static bool is_alnum(int c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

enum TokenType {
    NEWLINE,
    INDENT,
    DEDENT,
    STRING_START,
    STRING_CONTENT,
    ESCAPE_INTERPOLATION,
    STRING_END,
    COMMENT,
    CLOSE_PAREN,
    CLOSE_BRACKET,
    CLOSE_BRACE,
    EXCEPT,
    // bare subprocess detection
    SUBPROCESS_START,
    // operator disambiguation between xonsh <-> python
    LOGICAL_AND,      // &&
    LOGICAL_OR,       // ||
    BACKGROUND_AMP,   // Single &
    // and/or keywords in subprocess context
    KEYWORD_AND,      // 'and' keyword
    KEYWORD_OR,       // 'or' keyword
    // subprocess macro: identifier! args
    SUBPROCESS_MACRO_START,
    // block macro: with!
    BLOCK_MACRO_START,
    // path string prefix: p, pf, pr, P, PF, PR (only when followed by quote)
    PATH_PREFIX,
};

typedef enum {
    SingleQuote = 1 << 0,
    DoubleQuote = 1 << 1,
    BackQuote = 1 << 2,
    Raw = 1 << 3,
    Format = 1 << 4,
    Triple = 1 << 5,
    Bytes = 1 << 6,
} Flags;

typedef struct {
    char flags;
} Delimiter;

static inline Delimiter new_delimiter() { return (Delimiter){0}; }

static inline bool is_format(Delimiter *delimiter) { return delimiter->flags & Format; }

static inline bool is_raw(Delimiter *delimiter) { return delimiter->flags & Raw; }

static inline bool is_triple(Delimiter *delimiter) { return delimiter->flags & Triple; }

static inline bool is_bytes(Delimiter *delimiter) { return delimiter->flags & Bytes; }

static inline int32_t end_character(Delimiter *delimiter) {
    if (delimiter->flags & SingleQuote) {
        return '\'';
    }
    if (delimiter->flags & DoubleQuote) {
        return '"';
    }
    if (delimiter->flags & BackQuote) {
        return '`';
    }
    return 0;
}

static inline void set_format(Delimiter *delimiter) { delimiter->flags |= Format; }

static inline void set_raw(Delimiter *delimiter) { delimiter->flags |= Raw; }

static inline void set_triple(Delimiter *delimiter) { delimiter->flags |= Triple; }

static inline void set_bytes(Delimiter *delimiter) { delimiter->flags |= Bytes; }

static inline void set_end_character(Delimiter *delimiter, int32_t character) {
    switch (character) {
        case '\'':
            delimiter->flags |= SingleQuote;
            break;
        case '"':
            delimiter->flags |= DoubleQuote;
            break;
        case '`':
            delimiter->flags |= BackQuote;
            break;
        default:
            assert(false);
    }
}

typedef struct {
    Array(uint16_t) indents;
    Array(Delimiter) delimiters;
    bool inside_f_string;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// Bare Subprocess Detection Heuristics

/**
 * Check if character is valid for an identifier start
 */
static inline bool is_identifier_start(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/**
 * Check if character is valid within an identifier
 */
static inline bool is_identifier_char(int32_t c) {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

/**
 * Check if character is a digit
 */
static inline bool is_digit(int32_t c) {
    return c >= '0' && c <= '9';
}

/**
 * Check if character is whitespace (excluding newline)
 */
static inline bool is_whitespace(int32_t c) {
    return c == ' ' || c == '\t';
}

/**
 * Python keywords that should never start a bare subprocess
 */
static const char *python_keywords[] = {
    "def", "class", "if", "elif", "else", "for", "while", "try", "except",
    "finally", "with", "import", "from", "return", "yield", "raise", "pass",
    "break", "continue", "del", "global", "nonlocal", "assert", "lambda",
    "async", "await", "match", "case", "type",
    // Python keywords that start valid `keyword word` statements
    "print", "not", "exec",
    // Xonsh reserved words (prevent subprocess detection)
    "xontrib",
    NULL
};

/**
 * Check if the identifier matches a Python keyword
 */
static bool is_python_keyword(const char *ident, size_t len) {
    for (int i = 0; python_keywords[i] != NULL; i++) {
        size_t kw_len = strlen(python_keywords[i]);
        if (kw_len == len && strncmp(ident, python_keywords[i], len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Python operator keywords that appear between identifiers in valid Python.
 * e.g., `x and y`, `x or y`, `x is y`, `x in y`, `x not in y`, `x as y`
 * When these appear after whitespace mid-line, they are NOT bare word arguments.
 */
static const char *python_operator_keywords[] = {
    "and", "or", "is", "in", "not", "as",
    NULL
};

/**
 * Check if the identifier matches a Python operator keyword
 */
static bool is_python_operator_keyword(const char *ident, size_t len) {
    for (int i = 0; python_operator_keywords[i] != NULL; i++) {
        size_t kw_len = strlen(python_operator_keywords[i]);
        if (kw_len == len && strncmp(ident, python_operator_keywords[i], len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Common shell commands that should be recognized as bare subprocess
 * even without flags or other shell signals
 */
static const char *shell_commands[] = {
    // Core utilities
    "cd", "ls", "pwd", "echo", "cat", "cp", "mv", "rm", "mkdir", "rmdir",
    "touch", "chmod", "chown", "ln", "head", "tail", "less", "more",
    // Search and text processing
    "grep", "find", "sed", "awk", "sort", "uniq", "wc", "cut", "tr", "xargs",
    // Build tools
    "make", "cmake", "ninja", "gradle", "mvn", "ant", "meson",
    // Package managers
    "npm", "yarn", "pnpm", "pip", "pip3", "cargo", "go", "gem", "composer",
    // Version control
    "git", "svn", "hg", "bzr",
    // Containers
    "docker", "podman", "kubectl", "helm", "docker-compose",
    // Network
    "curl", "wget", "ssh", "scp", "rsync", "ping", "nc", "netstat",
    // Archive
    "tar", "zip", "unzip", "gzip", "gunzip", "xz", "bzip2",
    // System
    "sudo", "su", "ps", "top", "htop", "kill", "killall", "df", "du", "mount",
    // Compilers
    "gcc", "g++", "clang", "clang++", "rustc", "javac", "python", "python3",
    // Editors
    "vi", "vim", "nvim", "nano", "emacs", "code",
    // Xonsh specific
    "xpip", "completer", "history", "replay", "trace", "timeit",
    NULL
};

/**
 * Check if the identifier matches a known shell command
 */
static bool is_shell_command(const char *ident, size_t len) {
    for (int i = 0; shell_commands[i] != NULL; i++) {
        size_t cmd_len = strlen(shell_commands[i]);
        if (cmd_len == len && strncmp(ident, shell_commands[i], len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Result type for subprocess detection
 */
typedef enum {
    DETECT_NONE,              // Not a subprocess
    DETECT_SUBPROCESS,        // Bare subprocess (ls -la, cd /tmp, etc.)
    DETECT_SUBPROCESS_MACRO,  // Subprocess macro (echo! "Hello!")
    DETECT_STRING,            // String literal (f"...", b"...", etc.) - already consumed prefix
    DETECT_BLOCK_MACRO,       // Block macro (with! Context():)
    DETECT_PATH_PREFIX,       // Path string prefix (p"...", pf"...", etc.) - already consumed prefix
} DetectResult;

/**
 * Detect if the current line appears to be a bare subprocess command
 * OR a subprocess macro.
 *
 * Uses heuristics based on common shell patterns:
 *
 * SUBPROCESS MACRO:
 * - identifier! followed by space (not identifier!( which is function macro)
 * - "with!" is excluded (it's a block macro)
 *
 * POSITIVE SIGNALS (likely subprocess):
 * 1. Line starts with path: /, ./, ~/
 * 2. Contains flag-like tokens: -x, --flag
 * 3. Contains pipe: |
 * 4. Contains redirect: >, >>, <, 2>, &>
 * 5. Contains & at end (background)
 *
 * NEGATIVE SIGNALS (likely Python):
 * 1. First token is a Python keyword
 * 2. Contains = (assignment, but not ==, !=, <=, >=)
 * 3. Contains ( immediately after identifier (function call)
 * 4. Contains [ immediately after identifier (subscript)
 * 5. Contains . after identifier (attribute access)
 * 6. Contains Python comparison operators: ==, !=, <=, >=, :=
 *
 * This function scans ahead from the current position to analyze the line.
 * It does NOT consume tokens - it just peeks; which can get inefficient.
 *
 * The out parameter subprocess_macro_end is set if a subprocess macro is detected,
 * indicating how many characters were consumed up to and including "identifier! ".
 */
static DetectResult detect_subprocess_line(TSLexer *lexer, size_t *subprocess_macro_end, Delimiter *string_delimiter) {
    *subprocess_macro_end = 0;
    // Save original position marker
    lexer->mark_end(lexer);

    // Skip leading whitespace
    while (is_whitespace(lexer->lookahead)) {
        advance(lexer);
    }

    // Track position for subprocess macro detection
    size_t pos = 0;

    // Check for path-like start: /, ./, ~/
    if (lexer->lookahead == '/') {
        return DETECT_SUBPROCESS;  // Absolute path command
    }
    if (lexer->lookahead == '.') {
        advance(lexer);
        pos++;
        if (lexer->lookahead == '/') {
            return DETECT_SUBPROCESS;  // Relative path ./cmd
        }
        // Could be float literal like .5, reset and continue
    }
    if (lexer->lookahead == '~') {
        advance(lexer);
        pos++;
        if (lexer->lookahead == '/') {
            return DETECT_SUBPROCESS;  // Home path ~/cmd
        }
    }

    // If starting with $, check what follows
    if (lexer->lookahead == '$') {
        advance(lexer);
        pos++;
        if (lexer->lookahead == '(' || lexer->lookahead == '[') {
            // This is explicit subprocess syntax $(, $[, not bare
            return DETECT_NONE;
        }
        // $VAR at start - could be env var usage, scan rest of line
    }

    // If starting with !, check what follows
    if (lexer->lookahead == '!') {
        advance(lexer);
        pos++;
        if (lexer->lookahead == '(' || lexer->lookahead == '[') {
            // This is explicit subprocess syntax !(, ![, not bare
            return DETECT_NONE;
        }
    }

    // If starting with [, this is Python list syntax, not subprocess
    if (lexer->lookahead == '[') {
        return DETECT_NONE;
    }

    // Check for @identifier at line start (subprocess modifier or Python decorator)
    // @identifier followed by . or ( is a Python decorator - don't treat as subprocess
    // @identifier followed by whitespace + path/command is a modified subprocess
    if (lexer->lookahead == '@') {
        advance(lexer);
        pos++;
        // Check if followed by identifier
        if (is_identifier_start(lexer->lookahead)) {
            // Skip the identifier
            while (is_identifier_char(lexer->lookahead)) {
                advance(lexer);
                pos++;
            }
            // Check what follows the identifier
            if (lexer->lookahead == '.' || lexer->lookahead == '(') {
                // This is a Python decorator like @app.route() or @decorator()
                return DETECT_NONE;
            }
            if (is_whitespace(lexer->lookahead)) {
                // Skip whitespace
                while (is_whitespace(lexer->lookahead)) {
                    advance(lexer);
                    pos++;
                }
                // Check if what follows looks like a subprocess command
                // (path, flag, or known command)
                if (lexer->lookahead == '/' || lexer->lookahead == '.' ||
                    lexer->lookahead == '~' || lexer->lookahead == '-') {
                    return DETECT_SUBPROCESS;  // Modified subprocess like @unthread ./tool.sh
                }
                // Check for known shell command after @modifier
                char cmd[64];
                size_t cmd_len = 0;
                if (is_identifier_start(lexer->lookahead)) {
                    while (is_identifier_char(lexer->lookahead) && cmd_len < 63) {
                        cmd[cmd_len++] = (char)lexer->lookahead;
                        advance(lexer);
                    }
                    cmd[cmd_len] = '\0';
                    if (is_shell_command(cmd, cmd_len)) {
                        return DETECT_SUBPROCESS;  // @modifier known_command
                    }
                }
            }
        }
        // Not a modified subprocess - could be other @ patterns
        return DETECT_NONE;
    }

    // Read the first identifier (if present)
    char first_ident[64];
    size_t ident_len = 0;

    // Skip any $ that might be at the start (for $VAR)
    if (lexer->lookahead == '$') {
        advance(lexer);
        pos++;
    }

    if (is_identifier_start(lexer->lookahead)) {
        while (is_identifier_char(lexer->lookahead) && ident_len < 63) {
            first_ident[ident_len++] = (char)lexer->lookahead;
            advance(lexer);
            pos++;
        }
        first_ident[ident_len] = '\0';

        // Check if first identifier is a string prefix followed by a quote
        // String prefixes are 1-3 chars composed of: f, r, b, u (case insensitive)
        // Examples: f"...", rf"...", br"...", u"..."
        // If detected, return DETECT_STRING so caller can handle with prefix info
        if (ident_len >= 1 && ident_len <= 3 &&
            (lexer->lookahead == '"' || lexer->lookahead == '\'')) {
            bool is_string_prefix = true;
            for (size_t i = 0; i < ident_len && is_string_prefix; i++) {
                char c = first_ident[i];
                if (c != 'f' && c != 'F' && c != 'r' && c != 'R' &&
                    c != 'b' && c != 'B' && c != 'u' && c != 'U') {
                    is_string_prefix = false;
                }
            }
            if (is_string_prefix && string_delimiter != NULL) {
                // Fill in the delimiter info based on prefix chars
                *string_delimiter = new_delimiter();
                for (size_t i = 0; i < ident_len; i++) {
                    char c = first_ident[i];
                    if (c == 'f' || c == 'F') {
                        set_format(string_delimiter);
                    } else if (c == 'r' || c == 'R') {
                        set_raw(string_delimiter);
                    } else if (c == 'b' || c == 'B') {
                        set_bytes(string_delimiter);
                    }
                    // 'u' doesn't set any flag
                }
                return DETECT_STRING;  // String with prefix already consumed
            }

            // Check if it's a path prefix (p, pf, pr — case insensitive)
            if ((ident_len == 1 && (first_ident[0] == 'p' || first_ident[0] == 'P')) ||
                (ident_len == 2 && (first_ident[0] == 'p' || first_ident[0] == 'P') &&
                 (first_ident[1] == 'f' || first_ident[1] == 'F' ||
                  first_ident[1] == 'r' || first_ident[1] == 'R'))) {
                return DETECT_PATH_PREFIX;
            }
        }

        // Check for help expression: identifier? or identifier??
        // These should NOT be treated as subprocess, let grammar handle them
        if (lexer->lookahead == '?') {
            advance(lexer);
            if (lexer->lookahead == '?') {
                advance(lexer);  // Skip second ?
            }
            // Check if rest of line is empty (just whitespace/newline)
            while (is_whitespace(lexer->lookahead)) {
                advance(lexer);
            }
            if (lexer->lookahead == '\n' || lexer->lookahead == '\0' || lexer->eof(lexer)) {
                return DETECT_NONE;  // Help expression, not subprocess
            }
        }

        // Check if first identifier is a Python keyword (except "with" which might be with!)
        if (is_python_keyword(first_ident, ident_len)) {
            // "with" followed by ! is a block macro, not Python
            if (!(ident_len == 4 && strncmp(first_ident, "with", 4) == 0 && lexer->lookahead == '!')) {
                return DETECT_NONE;  // Python control flow
            }
        }

        // Check for subprocess macro: identifier! followed by space
        if (lexer->lookahead == '!') {
            advance(lexer);
            pos++;
            if (is_whitespace(lexer->lookahead)) {
                // "with!" is a block macro
                if (ident_len == 4 && strncmp(first_ident, "with", 4) == 0) {
                    return DETECT_BLOCK_MACRO;
                }
                // Skip the whitespace
                while (is_whitespace(lexer->lookahead)) {
                    advance(lexer);
                    pos++;
                }
                // This is a subprocess macro
                *subprocess_macro_end = pos;
                return DETECT_SUBPROCESS_MACRO;
            }
        }
    }

    // Special case: comma-only lines (aliases registered with commas)
    // e.g., aliases.register(",") then calling just ","
    if (ident_len == 0 && lexer->lookahead == ',') {
        while (lexer->lookahead == ',') {
            advance(lexer);
        }
        // Check rest of line is just whitespace
        while (is_whitespace(lexer->lookahead)) {
            advance(lexer);
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\0' || lexer->eof(lexer)) {
            return DETECT_SUBPROCESS;  // Comma-only command
        }
    }

    // Check if first identifier is a known shell command
    // If so, treat subsequent file extensions (.txt) as shell args, not Python attributes
    bool is_known_command = (ident_len > 0 && is_shell_command(first_ident, ident_len));

    // Now scan the rest of the line looking for patterns
    bool has_flag = false;          // -x, --flag
    bool has_pipe = false;          // |
    bool has_redirect = false;      // >, >>, <
    bool has_assignment = false;    // = (but not ==)
    bool has_call_parens = false;   // identifier followed immediately by (
    bool has_subscript = false;     // identifier followed immediately by [
    bool has_attribute = false;     // identifier followed immediately by .
    bool has_comparison = false;    // ==, !=, <=, >=, :=
    bool has_env_arg = false;       // identifier followed by $VAR (e.g., cd $HOME)
    bool has_macro_call = false;    // identifier!( (xonsh function macro call)
    bool has_subprocess_macro = false;  // identifier! (xonsh subprocess macro)
    bool has_bare_word_arg = false;      // identifier after whitespace (not keyword)
    bool has_python_keyword_op = false;  // 'and', 'or', 'is', etc. between idents
    bool has_python_arith_op = false;    // +, %, ^, ~, , (clearly Python-only operators)
    int brace_depth = 0;                 // track {} depth for comma handling

    bool in_string = false;
    char string_char = 0;
    bool prev_was_ident_no_space = (ident_len > 0);  // For detecting immediate follow
    bool prev_was_space = false;     // Track if we just saw whitespace
    bool seen_shell_signal = is_known_command;  // Known commands disable Python-like detection
    bool prev_was_flag = false;      // Track if we just saw -x or --flag (for --key=value)
    int python_eval_depth = 0;       // Track nesting inside @(...) to ignore Python signals

    while (lexer->lookahead && lexer->lookahead != '\n') {
        int32_t c = lexer->lookahead;

        // Handle strings (don't scan inside them)
        if (!in_string && (c == '"' || c == '\'')) {
            in_string = true;
            string_char = (char)c;
            advance(lexer);
            prev_was_ident_no_space = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                advance(lexer);  // Skip escape
                if (lexer->lookahead) advance(lexer);
                continue;
            }
            if (c == string_char) {
                in_string = false;
            }
            advance(lexer);
            continue;
        }

        // Check for flags: -x or --flag
        if (c == '-') {
            advance(lexer);
            if (lexer->lookahead == '-') {
                // -- could be --flag or Python decrement (rare)
                advance(lexer);
                if (is_identifier_start(lexer->lookahead)) {
                    has_flag = true;  // --flag pattern
                    seen_shell_signal = true;
                    prev_was_flag = true;  // Track for --key=value
                }
            } else if (is_identifier_start(lexer->lookahead)) {
                // Could be -x flag or Python subtraction
                has_flag = true;  // -x pattern
                seen_shell_signal = true;
                prev_was_flag = true;  // Track for -k=value
            }
            prev_was_ident_no_space = false;
            continue;
        }

        // Check for pipe: | and logical or: ||
        if (c == '|') {
            advance(lexer);
            if (lexer->lookahead == '|') {
                // || is logical OR - shell signal
                has_pipe = true;
                seen_shell_signal = true;
                advance(lexer);
            } else if (lexer->lookahead != '=') {
                has_pipe = true;  // Single | is shell pipe
                seen_shell_signal = true;
            }
            prev_was_ident_no_space = false;
            continue;
        }

        // Check for & (background) and && (logical and)
        if (c == '&') {
            advance(lexer);
            if (lexer->lookahead == '&') {
                // && is logical AND - shell signal
                has_pipe = true;  // Reuse flag - it's a shell signal
                seen_shell_signal = true;
                advance(lexer);
            } else {
                // Single & - could be background operator
                // Skip any trailing whitespace to check if at end of line
                while (is_whitespace(lexer->lookahead)) {
                    advance(lexer);
                }
                if (lexer->lookahead == '\n' || lexer->lookahead == '\0' || lexer->eof(lexer)) {
                    // & at end of line is background execution - shell signal
                    has_pipe = true;
                    seen_shell_signal = true;
                }
            }
            prev_was_ident_no_space = false;
            continue;
        }

        // Check for redirect: >, >>, <
        if (c == '>') {
            advance(lexer);
            if (lexer->lookahead == '=') {
                has_comparison = true;  // >=
            } else {
                has_redirect = true;  // > or >>
                seen_shell_signal = true;
            }
            prev_was_ident_no_space = false;
            continue;
        }
        if (c == '<') {
            advance(lexer);
            if (lexer->lookahead == '=') {
                has_comparison = true;  // <=
            } else if (lexer->lookahead != '<') {
                has_redirect = true;  // < (not <<)
                seen_shell_signal = true;
            }
            prev_was_ident_no_space = false;
            continue;
        }

        // Check for assignment vs comparison
        if (c == '=') {
            advance(lexer);
            if (lexer->lookahead == '=' && python_eval_depth == 0) {
                has_comparison = true;  // == (only if not inside @(...))
                advance(lexer);
                prev_was_flag = false;  // Reset after ==
            } else if (prev_was_flag) {
                // --key=value or -k=value is shell syntax, not Python assignment
                // Keep prev_was_flag true for patterns like --env=FOO=bar
                // Don't set has_assignment, continue scanning
            } else if (python_eval_depth == 0) {
                has_assignment = true;  // Single = is Python assignment (only if not inside @(...))
                prev_was_flag = false;  // Reset after assignment
            }
            prev_was_ident_no_space = false;
            continue;
        }

        // Check for != and :=, and macro calls (identifier!)
        if (c == '!') {
            advance(lexer);
            if (lexer->lookahead == '=' && python_eval_depth == 0) {
                has_comparison = true;  // != (only if not inside @(...))
            } else if (prev_was_ident_no_space && lexer->lookahead == '(') {
                // This is a function macro call: identifier!(args)
                has_macro_call = true;
            } else if (prev_was_ident_no_space && is_whitespace(lexer->lookahead)) {
                // This is a subprocess macro: identifier! args
                // e.g., echo! "Hello!", bash -c! echo {123}
                has_subprocess_macro = true;
            }
            prev_was_ident_no_space = false;
            continue;
        }
        if (c == ':') {
            advance(lexer);
            if (lexer->lookahead == '=' && python_eval_depth == 0) {
                has_comparison = true;  // := (only if not inside @(...))
            }
            prev_was_ident_no_space = false;
            continue;
        }

        // Track parentheses depth when inside @(...) python evaluation
        if (c == '(' && python_eval_depth > 0) {
            python_eval_depth++;
            advance(lexer);
            prev_was_ident_no_space = false;
            continue;
        }
        if (c == ')' && python_eval_depth > 0) {
            python_eval_depth--;
            advance(lexer);
            prev_was_ident_no_space = false;
            continue;
        }

        // Check for function call: identifier( (only before shell signals)
        if (c == '(' && prev_was_ident_no_space && !seen_shell_signal) {
            has_call_parens = true;
            prev_was_ident_no_space = false;
            advance(lexer);
            continue;
        }

        // Check for subscript: identifier[ (only before shell signals)
        if (c == '[' && prev_was_ident_no_space && !seen_shell_signal) {
            has_subscript = true;
            prev_was_ident_no_space = false;
            advance(lexer);
            continue;
        }

        // Check for attribute access: identifier. (only before shell signals)
        // This prevents file extensions like output.txt from being detected
        if (c == '.' && prev_was_ident_no_space && !seen_shell_signal) {
            has_attribute = true;
            prev_was_ident_no_space = false;
            advance(lexer);
            continue;
        }

        // Track if we just saw an identifier (with no space before next char)
        if (is_identifier_start(c)) {
            char word[64];
            size_t word_len = 0;
            word[word_len++] = (char)c;
            advance(lexer);  // advance past first char (c == lexer->lookahead at loop top)
            while (is_identifier_char(lexer->lookahead) && word_len < 63) {
                word[word_len++] = (char)lexer->lookahead;
                advance(lexer);
            }
            word[word_len] = '\0';

            if (prev_was_space && brace_depth == 0) {
                if (is_python_operator_keyword(word, word_len)) {
                    has_python_keyword_op = true;
                } else {
                    has_bare_word_arg = true;
                    seen_shell_signal = true;
                }
            }

            prev_was_ident_no_space = true;
            continue;
        }

        // Numeric arguments after whitespace (e.g., cmd 1 2 3)
        // In Python, `identifier number` without an operator is a SyntaxError;
        // in subprocess context, numbers are valid arguments.
        if (is_digit(c) && prev_was_space && brace_depth == 0) {
            advance(lexer);
            while (is_digit(lexer->lookahead) || lexer->lookahead == '.') {
                advance(lexer);
            }
            has_bare_word_arg = true;
            seen_shell_signal = true;
            prev_was_ident_no_space = false;
            prev_was_space = false;
            continue;
        }

        // Check for $ patterns - both env vars and subprocess operators
        // $VAR, $(cmd), $[cmd] are all shell signals when after whitespace
        if (c == '$' && prev_was_space) {
            advance(lexer);
            if (is_identifier_start(lexer->lookahead)) {
                // $VAR - environment variable argument
                has_env_arg = true;
                seen_shell_signal = true;
            } else if (lexer->lookahead == '(' || lexer->lookahead == '[') {
                // $(cmd) or $[cmd] - captured subprocess as argument
                has_env_arg = true;  // Reuse flag - it's a shell signal
                seen_shell_signal = true;
            }
            prev_was_ident_no_space = false;
            prev_was_space = false;
            continue;
        }

        // Check for @$( - tokenized substitution and @( - python evaluation as subprocess argument
        if (c == '@' && prev_was_space) {
            advance(lexer);
            if (lexer->lookahead == '$') {
                advance(lexer);
                if (lexer->lookahead == '(') {
                    // @$(cmd) - tokenized substitution
                    has_env_arg = true;  // Reuse flag - it's a shell signal
                    seen_shell_signal = true;
                }
            } else if (lexer->lookahead == '(') {
                // @(...) - python evaluation - start tracking paren depth
                advance(lexer);  // consume (
                python_eval_depth = 1;
                has_env_arg = true;  // This is a shell signal
                seen_shell_signal = true;
            }
            prev_was_ident_no_space = false;
            prev_was_space = false;
            continue;
        }

        // Skip whitespace - this breaks the "immediate follow" pattern
        if (is_whitespace(c)) {
            advance(lexer);
            prev_was_ident_no_space = false;  // Reset - next char isn't immediately after ident
            prev_was_space = true;
            prev_was_flag = false;  // Reset flag context on whitespace
            continue;
        }

        // Semicolons are Python statement separators — stop scanning here
        // so we only analyze the first statement on the line.
        // Comments (#) also end the scannable portion of the line.
        if (c == ';' || c == '#') {
            break;
        }

        // Track brace depth for comma handling
        if (c == '{') brace_depth++;
        if (c == '}' && brace_depth > 0) brace_depth--;

        // Track clearly-Python arithmetic operators
        // Commas only count at top level (not inside braces, which are brace expansion)
        if (c == '+' || c == '%' || c == '^' || c == '~' ||
            (c == ',' && brace_depth == 0)) {
            has_python_arith_op = true;
        }

        // Any other character (operators, punctuation, etc.)
        prev_was_ident_no_space = false;
        prev_was_space = false;
        advance(lexer);
    }

    // Decision logic: Strong Python signals override all shell signals
    if (has_assignment || has_comparison || has_call_parens ||
        has_subscript || has_attribute || has_macro_call) {
        return DETECT_NONE;  // Strong Python signals
    }

    // Python operator keywords and arithmetic ops cancel bare word detection
    // but do NOT override strong shell signals (flags, pipes, redirects, etc.)
    bool effective_bare_word = has_bare_word_arg &&
        !has_python_keyword_op && !has_python_arith_op;

    // Shell signals indicate subprocess
    if (has_flag || has_pipe || has_redirect || has_env_arg ||
        has_subprocess_macro || effective_bare_word) {
        return DETECT_SUBPROCESS;  // Shell signals (including mid-line macro, bare word args)
    }

    // Known shell command without other signals (e.g., "make", "cd /tmp")
    if (ident_len > 0 && is_shell_command(first_ident, ident_len)) {
        return DETECT_SUBPROCESS;  // Known shell command
    }

    // Default: treat as Python (safer)
    return DETECT_NONE;
}

bool tree_sitter_xonsh_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;

    bool error_recovery_mode = valid_symbols[STRING_CONTENT] && valid_symbols[INDENT];
    bool within_brackets = valid_symbols[CLOSE_BRACE] || valid_symbols[CLOSE_PAREN] || valid_symbols[CLOSE_BRACKET];

    bool advanced_once = false;
    if (valid_symbols[ESCAPE_INTERPOLATION] && scanner->delimiters.size > 0 &&
        (lexer->lookahead == '{' || lexer->lookahead == '}') && !error_recovery_mode) {
        Delimiter *delimiter = array_back(&scanner->delimiters);
        if (is_format(delimiter)) {
            lexer->mark_end(lexer);
            bool is_left_brace = lexer->lookahead == '{';
            advance(lexer);
            advanced_once = true;
            if ((lexer->lookahead == '{' && is_left_brace) || (lexer->lookahead == '}' && !is_left_brace)) {
                advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = ESCAPE_INTERPOLATION;
                return true;
            }
            return false;
        }
    }

    if (valid_symbols[STRING_CONTENT] && scanner->delimiters.size > 0 && !error_recovery_mode) {
        Delimiter *delimiter = array_back(&scanner->delimiters);
        int32_t end_char = end_character(delimiter);
        bool has_content = advanced_once;
        while (lexer->lookahead) {
            if ((advanced_once || lexer->lookahead == '{' || lexer->lookahead == '}') && is_format(delimiter)) {
                lexer->mark_end(lexer);
                lexer->result_symbol = STRING_CONTENT;
                return has_content;
            }
            if (lexer->lookahead == '\\') {
                if (is_raw(delimiter)) {
                    // Step over the backslash.
                    advance(lexer);
                    // Step over any escaped quotes.
                    if (lexer->lookahead == end_character(delimiter) || lexer->lookahead == '\\') {
                        advance(lexer);
                    }
                    // Step over newlines
                    if (lexer->lookahead == '\r') {
                        advance(lexer);
                        if (lexer->lookahead == '\n') {
                            advance(lexer);
                        }
                    } else if (lexer->lookahead == '\n') {
                        advance(lexer);
                    }
                    continue;
                }
                if (is_bytes(delimiter)) {
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (lexer->lookahead == 'N' || lexer->lookahead == 'u' || lexer->lookahead == 'U') {
                        // In bytes string, \N{...}, \uXXXX and \UXXXXXXXX are
                        // not escape sequences
                        // https://docs.python.org/3/reference/lexical_analysis.html#string-and-bytes-literals
                        advance(lexer);
                    } else {
                        lexer->result_symbol = STRING_CONTENT;
                        return has_content;
                    }
                } else {
                    lexer->mark_end(lexer);
                    lexer->result_symbol = STRING_CONTENT;
                    return has_content;
                }
            } else if (lexer->lookahead == end_char) {
                if (is_triple(delimiter)) {
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (lexer->lookahead == end_char) {
                        advance(lexer);
                        if (lexer->lookahead == end_char) {
                            if (has_content) {
                                lexer->result_symbol = STRING_CONTENT;
                            } else {
                                advance(lexer);
                                lexer->mark_end(lexer);
                                array_pop(&scanner->delimiters);
                                lexer->result_symbol = STRING_END;
                                scanner->inside_f_string = false;
                            }
                            return true;
                        }
                        lexer->mark_end(lexer);
                        lexer->result_symbol = STRING_CONTENT;
                        return true;
                    }
                    lexer->mark_end(lexer);
                    lexer->result_symbol = STRING_CONTENT;
                    return true;
                }
                if (has_content) {
                    lexer->result_symbol = STRING_CONTENT;
                } else {
                    advance(lexer);
                    array_pop(&scanner->delimiters);
                    lexer->result_symbol = STRING_END;
                    scanner->inside_f_string = false;
                }
                lexer->mark_end(lexer);
                return true;

            } else if (lexer->lookahead == '\n' && has_content && !is_triple(delimiter)) {
                return false;
            }
            advance(lexer);
            has_content = true;
        }
    }

    lexer->mark_end(lexer);

    bool found_end_of_line = false;
    uint32_t indent_length = 0;
    int32_t first_comment_indent_length = -1;
    for (;;) {
        if (lexer->lookahead == '\n') {
            found_end_of_line = true;
            indent_length = 0;
            skip(lexer);
        } else if (lexer->lookahead == ' ') {
            indent_length++;
            skip(lexer);
        } else if (lexer->lookahead == '\r' || lexer->lookahead == '\f') {
            indent_length = 0;
            skip(lexer);
        } else if (lexer->lookahead == '\t') {
            indent_length += 8;
            skip(lexer);
        } else if (lexer->lookahead == '#' && (valid_symbols[INDENT] || valid_symbols[DEDENT] ||
                                               valid_symbols[NEWLINE] || valid_symbols[EXCEPT])) {
            // If we haven't found an EOL yet,
            // then this is a comment after an expression:
            //   foo = bar # comment
            // Just return, since we don't want to generate an indent/dedent
            // token.
            if (!found_end_of_line) {
                return false;
            }
            if (first_comment_indent_length == -1) {
                first_comment_indent_length = (int32_t)indent_length;
            }
            while (lexer->lookahead && lexer->lookahead != '\n') {
                skip(lexer);
            }
            skip(lexer);
            indent_length = 0;
        } else if (lexer->lookahead == '\\') {
            skip(lexer);
            if (lexer->lookahead == '\r') {
                skip(lexer);
            }
            if (lexer->lookahead == '\n' || lexer->eof(lexer)) {
                skip(lexer);
            } else {
                return false;
            }
        } else if (lexer->eof(lexer)) {
            indent_length = 0;
            found_end_of_line = true;
            break;
        } else {
            break;
        }
    }

    if (found_end_of_line) {
        if (scanner->indents.size > 0) {
            uint16_t current_indent_length = *array_back(&scanner->indents);

            if (valid_symbols[INDENT] && indent_length > current_indent_length) {
                array_push(&scanner->indents, indent_length);
                lexer->result_symbol = INDENT;
                return true;
            }

            bool next_tok_is_string_start =
                lexer->lookahead == '\"' || lexer->lookahead == '\'' || lexer->lookahead == '`';

            if ((valid_symbols[DEDENT] ||
                 (!valid_symbols[NEWLINE] && !(valid_symbols[STRING_START] && next_tok_is_string_start) &&
                  !within_brackets)) &&
                indent_length < current_indent_length && !scanner->inside_f_string &&

                // Wait to create a dedent token until we've consumed any
                // comments
                // whose indentation matches the current block.
                first_comment_indent_length < (int32_t)current_indent_length) {
                array_pop(&scanner->indents);
                lexer->result_symbol = DEDENT;
                return true;
            }
        }

        if (valid_symbols[NEWLINE] && !error_recovery_mode) {
            lexer->result_symbol = NEWLINE;
            return true;
        }
    }

    // Handle &, &&, |, || disambiguation
    // This ensures && and || are recognized as logical operators before & is
    // recognized as background operator
    if (valid_symbols[LOGICAL_AND] || valid_symbols[LOGICAL_OR] || valid_symbols[BACKGROUND_AMP]) {
        // Handle & and &&
        if (lexer->lookahead == '&') {
            advance(lexer);
            if (lexer->lookahead == '&') {
                // This is && (logical AND)
                if (valid_symbols[LOGICAL_AND]) {
                    advance(lexer);
                    lexer->mark_end(lexer);
                    lexer->result_symbol = LOGICAL_AND;
                    return true;
                }
                // If LOGICAL_AND is not valid, don't consume anything
                // Let the regular parser handle && as Python's bitwise AND
                return false;
            } else if (valid_symbols[BACKGROUND_AMP]) {
                // Single & - valid background operator
                lexer->mark_end(lexer);
                lexer->result_symbol = BACKGROUND_AMP;
                return true;
            }
            return false;
        }

        // Handle | and ||
        if (lexer->lookahead == '|' && valid_symbols[LOGICAL_OR]) {
            advance(lexer);
            if (lexer->lookahead == '|') {
                advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = LOGICAL_OR;
                return true;
            }
            return false;
        }
    }

    // Handle 'and' and 'or' keywords in subprocess context
    // These are recognized as logical operators inside subprocesses
    if (valid_symbols[KEYWORD_AND] || valid_symbols[KEYWORD_OR]) {
        // Check for 'and' keyword
        if (valid_symbols[KEYWORD_AND] && lexer->lookahead == 'a') {
            advance(lexer);
            if (lexer->lookahead == 'n') {
                advance(lexer);
                if (lexer->lookahead == 'd') {
                    advance(lexer);
                    // Check that next char is not alphanumeric (word boundary)
                    if (!is_alnum(lexer->lookahead) && lexer->lookahead != '_') {
                        lexer->mark_end(lexer);
                        lexer->result_symbol = KEYWORD_AND;
                        return true;
                    }
                }
            }
            return false;
        }

        // Check for 'or' keyword
        if (valid_symbols[KEYWORD_OR] && lexer->lookahead == 'o') {
            advance(lexer);
            if (lexer->lookahead == 'r') {
                advance(lexer);
                // Check that next char is not alphanumeric (word boundary)
                if (!is_alnum(lexer->lookahead) && lexer->lookahead != '_') {
                    lexer->mark_end(lexer);
                    lexer->result_symbol = KEYWORD_OR;
                    return true;
                }
            }
            return false;
        }
    }

    // Check for subprocess macro AND bare subprocess at the start of a line
    // Subprocess macro: identifier! args (not identifier!( which is function macro)
    // Bare subprocess: detected by shell-like heuristics
    //
    // Special handling for string prefixes (f, r, b, u):
    // These chars could start commands like 'bash', 'find', 'rm', 'uname'.
    // If STRING_START is valid and we see a prefix followed by a quote, handle string first.
    // Otherwise, proceed with subprocess detection.
    // Note: backticks are NOT included - unprefixed backticks are regex globs
    bool looks_like_string = (lexer->lookahead == '"' || lexer->lookahead == '\'');

    // Check for subprocess macro AND bare subprocess
    // Both checks use detect_subprocess_line which can return either type
    bool check_subprocess = (valid_symbols[SUBPROCESS_START] || valid_symbols[SUBPROCESS_MACRO_START] ||
                             valid_symbols[BLOCK_MACRO_START]) &&
                            !within_brackets && !error_recovery_mode &&
                            first_comment_indent_length == -1 &&
                            lexer->lookahead != '#' &&
                            !looks_like_string;

    if (check_subprocess) {
        size_t subprocess_macro_end = 0;
        Delimiter string_delim = new_delimiter();
        DetectResult result = detect_subprocess_line(lexer, &subprocess_macro_end, &string_delim);

        if (result == DETECT_BLOCK_MACRO && valid_symbols[BLOCK_MACRO_START]) {
            // Mark the token end to include "with!"
            lexer->mark_end(lexer);
            lexer->result_symbol = BLOCK_MACRO_START;
            return true;
        }

        if (result == DETECT_SUBPROCESS_MACRO && valid_symbols[SUBPROCESS_MACRO_START]) {
            // Mark the token end to include "identifier! "
            lexer->mark_end(lexer);
            lexer->result_symbol = SUBPROCESS_MACRO_START;
            return true;
        }

        if (result == DETECT_SUBPROCESS && valid_symbols[SUBPROCESS_START]) {
            lexer->result_symbol = SUBPROCESS_START;
            return true;
        }

        // Handle path prefix detected by subprocess scanner
        // The prefix chars were already consumed, lexer is now at the quote
        if (result == DETECT_PATH_PREFIX && valid_symbols[PATH_PREFIX]) {
            lexer->mark_end(lexer);
            lexer->result_symbol = PATH_PREFIX;
            return true;
        }

        // Handle string literal detected by subprocess scanner
        // The prefix chars were already consumed, lexer is now at the quote
        if (result == DETECT_STRING && valid_symbols[STRING_START]) {
            // Process the quote(s) for single/triple string
            if (lexer->lookahead == '\'') {
                set_end_character(&string_delim, '\'');
                advance(lexer);
                lexer->mark_end(lexer);
                if (lexer->lookahead == '\'') {
                    advance(lexer);
                    if (lexer->lookahead == '\'') {
                        advance(lexer);
                        lexer->mark_end(lexer);
                        set_triple(&string_delim);
                    }
                }
            } else if (lexer->lookahead == '"') {
                set_end_character(&string_delim, '"');
                advance(lexer);
                lexer->mark_end(lexer);
                if (lexer->lookahead == '"') {
                    advance(lexer);
                    if (lexer->lookahead == '"') {
                        advance(lexer);
                        lexer->mark_end(lexer);
                        set_triple(&string_delim);
                    }
                }
            }

            if (end_character(&string_delim)) {
                array_push(&scanner->delimiters, string_delim);
                lexer->result_symbol = STRING_START;
                scanner->inside_f_string = is_format(&string_delim);
                return true;
            }
        }
    }

    // Path prefix detection: p, pf, pr, P, PF, PR immediately followed by a quote.
    // Must be checked BEFORE STRING_START so the prefix is emitted as PATH_PREFIX
    // and not consumed as part of a string prefix.
    if (first_comment_indent_length == -1 && valid_symbols[PATH_PREFIX]) {
        if (lexer->lookahead == 'p' || lexer->lookahead == 'P') {
            advance(lexer);
            if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
                // p"..." or P"..."
                lexer->mark_end(lexer);
                lexer->result_symbol = PATH_PREFIX;
                return true;
            } else if ((lexer->lookahead == 'f' || lexer->lookahead == 'F' ||
                        lexer->lookahead == 'r' || lexer->lookahead == 'R')) {
                advance(lexer);
                if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
                    // pf"...", pr"...", PF"...", PR"..."
                    lexer->mark_end(lexer);
                    lexer->result_symbol = PATH_PREFIX;
                    return true;
                }
            }
            // Not a path prefix — don't consume, let tokenizer handle as identifier
            return false;
        }
    }

    if (first_comment_indent_length == -1 && valid_symbols[STRING_START]) {
        Delimiter delimiter = new_delimiter();

        bool has_flags = false;
        while (lexer->lookahead) {
            if (lexer->lookahead == 'f' || lexer->lookahead == 'F') {
                set_format(&delimiter);
            } else if (lexer->lookahead == 'r' || lexer->lookahead == 'R') {
                set_raw(&delimiter);
            } else if (lexer->lookahead == 'b' || lexer->lookahead == 'B') {
                set_bytes(&delimiter);
            } else if (lexer->lookahead != 'u' && lexer->lookahead != 'U') {
                break;
            }
            has_flags = true;
            advance(lexer);
        }

        if (lexer->lookahead == '`') {
            // ALL backticks are handled by grammar rules:
            // - `pattern` -> regex_glob
            // - g`pattern` -> glob_pattern
            // - f`pattern` -> formatted_glob
            // Scanner should NOT emit STRING_START for backticks
            return false;
        } else if (lexer->lookahead == '\'') {
            set_end_character(&delimiter, '\'');
            advance(lexer);
            lexer->mark_end(lexer);
            if (lexer->lookahead == '\'') {
                advance(lexer);
                if (lexer->lookahead == '\'') {
                    advance(lexer);
                    lexer->mark_end(lexer);
                    set_triple(&delimiter);
                }
            }
        } else if (lexer->lookahead == '"') {
            set_end_character(&delimiter, '"');
            advance(lexer);
            lexer->mark_end(lexer);
            if (lexer->lookahead == '"') {
                advance(lexer);
                if (lexer->lookahead == '"') {
                    advance(lexer);
                    lexer->mark_end(lexer);
                    set_triple(&delimiter);
                }
            }
        }

        if (end_character(&delimiter)) {
            array_push(&scanner->delimiters, delimiter);
            lexer->result_symbol = STRING_START;
            scanner->inside_f_string = is_format(&delimiter);
            return true;
        }
        if (has_flags) {
            return false;
        }
    }

    return false;
}

unsigned tree_sitter_xonsh_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;

    size_t size = 0;

    buffer[size++] = (char)scanner->inside_f_string;

    size_t delimiter_count = scanner->delimiters.size;
    if (delimiter_count > UINT8_MAX) {
        delimiter_count = UINT8_MAX;
    }
    buffer[size++] = (char)delimiter_count;

    if (delimiter_count > 0) {
        memcpy(&buffer[size], scanner->delimiters.contents, delimiter_count);
    }
    size += delimiter_count;

    uint32_t iter = 1;
    for (; iter < scanner->indents.size && size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; ++iter) {
        buffer[size++] = (char)*array_get(&scanner->indents, iter);
    }

    return size;
}

void tree_sitter_xonsh_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;

    array_delete(&scanner->delimiters);
    array_delete(&scanner->indents);
    array_push(&scanner->indents, 0);

    if (length > 0) {
        size_t size = 0;

        scanner->inside_f_string = (bool)buffer[size++];

        size_t delimiter_count = (uint8_t)buffer[size++];
        if (delimiter_count > 0) {
            array_reserve(&scanner->delimiters, delimiter_count);
            scanner->delimiters.size = delimiter_count;
            memcpy(scanner->delimiters.contents, &buffer[size], delimiter_count);
            size += delimiter_count;
        }

        for (; size < length; size++) {
            array_push(&scanner->indents, (unsigned char)buffer[size]);
        }
    }
}

void *tree_sitter_xonsh_external_scanner_create() {
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    _Static_assert(sizeof(Delimiter) == sizeof(char), "");
#else
    assert(sizeof(Delimiter) == sizeof(char));
#endif
    Scanner *scanner = calloc(1, sizeof(Scanner));
    array_init(&scanner->indents);
    array_init(&scanner->delimiters);
    tree_sitter_xonsh_external_scanner_deserialize(scanner, NULL, 0);
    return scanner;
}

void tree_sitter_xonsh_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    array_delete(&scanner->indents);
    array_delete(&scanner->delimiters);
    free(scanner);
}
