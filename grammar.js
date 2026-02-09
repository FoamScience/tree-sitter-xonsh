/**
 * @file Xonsh grammar for tree-sitter
 * @author Mohammed Elwardi Fadeli
 * @license MIT
 *
 * Xonsh extends Python with shell-like syntax for subprocess execution.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const Python = require('tree-sitter-python/grammar');

module.exports = grammar(Python, {
  name: 'xonsh',

  // Override externals to add xonsh-specific tokens from scanner
  externals: ($, original) => original.concat([
    $._subprocess_start,  // Bare subprocess detection from scanner
    $._logical_and,       // && operator (disambiguated from &)
    $._logical_or,        // || operator
    $._background_amp,    // Single & for background execution
    $._keyword_and,       // 'and' keyword in subprocess context
    $._keyword_or,        // 'or' keyword in subprocess context
    $._subprocess_macro_start,  // Subprocess macro: identifier! (consumed by scanner)
  ]),

  rules: {

    // Env. Vars
    env_variable: $ => seq('$', $.identifier),
    env_variable_braced: $ => seq(
      '${',
      field('expression', $.expression),
      '}',
    ),

    // Env. Var. Assignment: $VAR = value
    env_assignment: $ => seq(
      field('left', $.env_variable),
      '=',
      field('right', $.expression),
    ),

    // Env. Var. Deletion: del $VAR
    env_deletion: $ => prec(2, seq(
      'del',
      field('target', $.env_variable),
    )),

    // Scoped Env. Var.: $VAR=value cmd
    env_scoped_command: $ => prec(5, seq(
      field('env', repeat1($.env_prefix)),
      field('command', $.subprocess_body),
    )),
    env_prefix: $ => seq(
      $.env_variable,
      token.immediate('='),
      field('value', choice(
        $.string,
        $.identifier,
        $.integer,
      )),
    ),

    // Subprocess Operators
    captured_subprocess: $ => seq(
      '$(',
      field('modifier', optional($.subprocess_modifier)),
      field('body', optional($.subprocess_body)),
      ')',
    ),

    // Subprocess output modifiers: @json, @yaml, @name, etc.
    // Built-ins transform output into Python objects.
    // Users can also define custom decorator aliases (e.g., @noerr, @path).
    // Higher precedence than custom_function_glob to resolve @identifier ambiguity
    subprocess_modifier: $ => prec(2, seq('@', $.identifier)),
    captured_subprocess_object: $ => seq(
      '!(',
      field('modifier', optional($.subprocess_modifier)),
      field('body', optional($.subprocess_body)),
      ')',
    ),
    uncaptured_subprocess: $ => seq(
      '$[',
      field('modifier', optional($.subprocess_modifier)),
      field('body', optional($.subprocess_body)),
      ']',
    ),
    uncaptured_subprocess_object: $ => seq(
      '![',
      field('modifier', optional($.subprocess_modifier)),
      field('body', optional($.subprocess_body)),
      ']',
    ),

    // Python Evaluation in Subprocess Context
    python_evaluation: $ => seq(
      '@(',
      field('expression', $.expression),
      ')',
    ),
    tokenized_substitution: $ => seq(
      '@$(',
      field('body', optional($.subprocess_body)),
      ')',
    ),

    // Special @ Object Access: @.env, @.lastcmd, etc.
    at_object: $ => seq(
      '@',
      '.',
      field('attribute', $.identifier),
    ),

    // Help Operators: expr? and expr??
    help_expression: $ => seq(
      field('expression', $.expression),
      '?',
    ),
    super_help_expression: $ => seq(
      field('expression', $.expression),
      '??',
    ),

    // Glob Patterns
    // Regex glob: `pattern` or r`pattern`
    regex_glob: $ => seq(
      choice('`', 'r`'),
      field('pattern', alias(/[^`]+/, $.regex_glob_content)),
      '`',
    ),

    // Regex path glob - returns Path objects: rp`pattern`
    regex_path_glob: $ => seq(
      'rp`',
      field('pattern', alias(/[^`]+/, $.regex_path_content)),
      '`',
    ),
    glob_pattern: $ => seq(
      'g`',
      field('pattern', alias(/[^`]+/, $.glob_pattern_content)),
      '`',
    ),

    // Formatted glob with variable substitution: f`pattern`
    formatted_glob: $ => seq(
      'f`',
      field('pattern', alias(/[^`]+/, $.formatted_glob_content)),
      '`',
    ),

    // Glob path - returns Path objects instead of strings: gp`pattern`
    glob_path: $ => seq(
      'gp`',
      field('pattern', alias(/[^`]+/, $.glob_path_content)),
      '`',
    ),

    // Custom function glob: @func`pattern`
    // Calls a Python function with the pattern string
    // e.g., @foo`bi` calls foo("bi") and expects a list of strings
    // High precedence to win against modified_bare_subprocess
    custom_function_glob: $ => prec(10, seq(
      '@',
      field('function', $.identifier),
      token.immediate('`'),
      field('pattern', alias(/[^`]*/, $.custom_glob_content)),
      '`',
    )),

    // Path Literals
    // Xonsh path prefixes: p (basic), pf (formatted), pr (raw)
    path_string: $ => seq(
      field('prefix', alias(choice('p', 'pf', 'pr', 'P', 'PF', 'PR'), $.path_prefix)),
      field('string', $.string),
    ),

    // Subprocess body
    subprocess_body: $ => seq(
      $.subprocess_command,
      repeat(choice(
        $.subprocess_pipeline,
        $.subprocess_logical,
      )),
    ),

    // Subprocess command and args
    subprocess_command: $ => repeat1($.subprocess_argument),
    subprocess_argument: $ => choice(
      $.subprocess_word,
      $.string,
      $.env_variable,
      $.env_variable_braced,
      $.python_evaluation,
      $.captured_subprocess,
      $.uncaptured_subprocess,
      $.tokenized_substitution,
      $.regex_glob,
      $.glob_pattern,
      $.formatted_glob,
      $.glob_path,
      $.regex_path_glob,
      $.custom_function_glob,
      $.brace_expansion,
      $.brace_literal,
      $.subprocess_redirect,
    ),

    // Brace expansion in subprocess context
    // Range: {1..5}, {a..z} or List: {a,b,c}
    brace_expansion: $ => choice(
      // Range expansion: {start..end} - match entire pattern with token
      alias(
        token(seq('{', /\w+/, '..', /\w+/, '}')),
        $.brace_range
      ),
      // List expansion: {a,b,c} - dots excluded from items
      seq(
        '{',
        alias(/[^{},.]+/, $.brace_item),
        repeat1(seq(',', alias(/[^{},.]+/, $.brace_item))),
        '}',
      ),
    ),

    // Brace literal: {content} without expansion syntax
    // Used when braces contain content that doesn't match expansion patterns
    // e.g., {123} in "bash -c! echo {123}" is literal, not expansion
    // token() avoids conflicts with brace_expansion
    brace_literal: _ => token(prec(1, seq('{', /[^{},.\s]+/, '}'))),

    // Subprocess word - any sequence of non-special characters
    // Use token() with high precedence to prevent ! from being leaked as a separate token by other rules
    // Allows backslash escapes (e.g., \; \$ \space)
    // Allows @ in middle of words (e.g., user@host in URLs) but not at word start
    subprocess_word: _ => token(prec(100, /([^\s$@`'"()\[\]{}|<>&;#\\](@[^\s$`'"()\[\]{}|<>&;#\\]+)?|\\[^\n])+/)),

    subprocess_pipeline: $ => seq(
      $.pipe_operator,
      $.subprocess_command,
    ),

    // Xonsh pipe operators:
    // |    - pipe stdout
    // e|   - pipe stderr (err|)
    // a|   - pipe both stdout and stderr (alias: all|)
    // Use token(prec(101, ...)) for letter-prefixed operators
    pipe_operator: _ => choice(
      '|',
      token(prec(101, 'e|')),
      token(prec(101, 'err|')),
      token(prec(101, 'a|')),
      token(prec(101, 'all|')),
    ),

    // Subprocess Logical Operators: && and ||
    subprocess_logical: $ => seq(
      field('operator', $.logical_operator),
      $.subprocess_command,
    ),

    // Xonsh supports both symbolic (&&, ||) and keyword (and, or) operators.
    // The scanner should handle disambiguation for all of these.
    logical_operator: $ => choice(
      $._logical_and,
      $._logical_or,
      $._keyword_and,
      $._keyword_or,
    ),

    // Redirections
    subprocess_redirect: $ => choice(
      // Redirects with targets
      seq(
        field('operator', $.redirect_operator),
        field('target', $.redirect_target),
      ),
      // Stream merging (no target needed)
      field('operator', $.stream_merge_operator),
    ),

    // Use token to win against subprocess_word
    redirect_operator: _ => choice(
      // Standard redirects
      '>', '>>', '<',
      // Numbered file descriptors - need precedence over word + >
      token(prec(101, '1>')), token(prec(101, '1>>')),
      token(prec(101, '2>')), token(prec(101, '2>>')),
      // Xonsh-specific aliases - need high precedence
      token(prec(101, 'o>')), token(prec(101, 'o>>')),      // stdout (alias for 1>)
      token(prec(101, 'e>')), token(prec(101, 'e>>')),      // stderr (alias for 2>)
      token(prec(101, 'err>')), token(prec(101, 'err>>')),  // stderr
      token(prec(101, 'out>')), token(prec(101, 'out>>')),  // stdout
      token(prec(101, 'all>')), token(prec(101, 'all>>')),  // both stdout and stderr
      '&>',                                                 // both stdout and stderr (bash compat)
      token(prec(101, 'a>')),                               // append both
    ),

    // Stream merging operators (don't take a file target)
    // Need high precedence to win against subprocess_word
    stream_merge_operator: _ => choice(
      token(prec(101, '2>&1')), token(prec(101, '1>&2')),
      token(prec(101, 'err>out')), token(prec(101, 'out>err')),
      token(prec(101, 'err>&1')), token(prec(101, 'out>&2')),
    ),

    redirect_target: $ => choice(
      $.subprocess_word,
      $.string,
      $.env_variable,
      $.env_variable_braced,
      $.python_evaluation,
    ),

    // Background Execution
    background_command: $ => prec(1, seq(
      choice(
        $.captured_subprocess,
        $.captured_subprocess_object,
        $.uncaptured_subprocess,
        $.uncaptured_subprocess_object,
      ),
      $._background_amp,
    )),

    // Xontrib Statement: xontrib load name1 name2 ...
    xontrib_statement: $ => seq(
      'xontrib',
      'load',
      repeat1($.xontrib_name),
    ),

    // Xontrib names can start with numbers (e.g., 1password)
    xontrib_name: _ => /[a-zA-Z0-9_][a-zA-Z0-9_]*/,

    // Macro Call: func!(args)
    macro_call: $ => prec(10, seq(
      field('name', $.identifier),
      token.immediate('!('),
      field('argument', optional($.macro_argument)),
      ')',
    )),

    macro_argument: _ => /[^)]*/,

    // =========================================================================
    // Subprocess Macro: cmd! args (passes rest of line as raw string)
    // e.g., echo! "Hello!", bash -c! echo {123}
    // Different from func!(args) which has ! immediately followed by (
    // =========================================================================

    // Subprocess macro: identifier! followed by space and args
    // e.g., echo! "Hello!", bash -c! echo {123}
    // The scanner emits _subprocess_macro_start after consuming "identifier! "
    subprocess_macro: $ => prec.dynamic(101, seq(
      $._subprocess_macro_start,  // Scanner consumed "identifier! "
      field('argument', alias(/[^\n]+/, $.subprocess_macro_argument)),
    )),

    // Block Macro: with! Context() as var:
    // Captures block content as string for macro processing
    block_macro_statement: $ => prec(20, seq(
      token('with!'),
      field('context', $.expression),
      optional(seq('as', field('alias', prec(20, $.identifier)))),
      ':',
      field('body', $._suite),
    )),

    // Bare Subprocess (detected by scanner heuristics)
    // e.g., "ls -la", "cd $HOME", "cat file | grep pattern"
    // Also handles modified subprocess: "@unthread ./tool.sh", "@json curl api/data"
    // The _subprocess_start token is emitted by the scanner when it senses
    // shell-like patterns (flags, pipes, redirects, path commands, @modifier + command)
    bare_subprocess: $ => seq(
      $._subprocess_start,
      field('modifier', optional($.subprocess_modifier)),
      field('body', $.subprocess_body),
      optional($._background_amp),  // Optional & for background execution
    ),

    // Xonsh Expression (all supported xonsh-specific constructs)
    xonsh_expression: $ => choice(
      $.env_variable,
      $.env_variable_braced,
      $.captured_subprocess,
      $.captured_subprocess_object,
      $.uncaptured_subprocess,
      $.uncaptured_subprocess_object,
      $.python_evaluation,
      $.tokenized_substitution,
      $.regex_glob,
      $.glob_pattern,
      $.formatted_glob,
      $.glob_path,
      $.regex_path_glob,
      $.custom_function_glob,
      $.path_string,
      $.background_command,
      $.at_object,
      $.macro_call,
    ),

    // Xonsh Statements (standalone xonsh constructs)

    // Standalone env_prefix: $VAR="value" (no space around =)
    // Distinct from env_assignment ($VAR = value) and env_scoped_command ($VAR=val cmd)
    env_prefix_statement: $ => prec(3, $.env_prefix),

    xonsh_statement: $ => choice(
      $.env_assignment,
      $.env_deletion,
      $.env_scoped_command,
      $.env_prefix_statement,
      $.help_expression,
      $.super_help_expression,
      $.xontrib_statement,
    ),

    // Add xonsh constructs to primary_expression (for use in Python expressions)
    primary_expression: ($, original) => choice(
      original,
      prec(1, $.xonsh_expression),
    ),

    // Override boolean_operator to also support && and || (xonsh style)
    // This allows ![cmd1] && ![cmd2] to parse correctly at Python level
    boolean_operator: ($, original) => choice(
      original,
      prec.left(11, seq(
        field('left', $.expression),
        field('operator', $._logical_and),
        field('right', $.expression),
      )),
      prec.left(10, seq(
        field('left', $.expression),
        field('operator', $._logical_or),
        field('right', $.expression),
      )),
    ),

    // Override _simple_statement to include xonsh expressions and statements
    _simple_statement: ($, original) => choice(
      original,
      prec.dynamic(10, $.xonsh_expression),
      prec.dynamic(11, $.xonsh_statement),
      // Bare subprocess has high priority - detected by scanner heuristics
      prec.dynamic(100, $.bare_subprocess),
      // Subprocess macro has highest priority (cmd! args)
      prec.dynamic(101, $.subprocess_macro),
    ),

    // Override _compound_statement to include block macro
    _compound_statement: ($, original) => choice(
      original,
      prec.dynamic(15, $.block_macro_statement),
    ),
  },
});
