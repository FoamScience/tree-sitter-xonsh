; Folding queries for xonsh
; Used by tree-sitter-aware editors (Neovim, Helix, Zed)

; ===========================================================================
; Compound statements (Python-inherited with body via _suite)
; ===========================================================================

(function_definition body: (block) @fold)
(class_definition body: (block) @fold)
(if_statement consequence: (block) @fold)
(elif_clause consequence: (block) @fold)
(else_clause body: (block) @fold)
(for_statement body: (block) @fold)
(while_statement body: (block) @fold)
(with_statement body: (block) @fold)
(try_statement body: (block) @fold)
(except_clause (block) @fold)
(finally_clause (block) @fold)
(match_statement body: (block) @fold)
(case_clause consequence: (block) @fold)

; ===========================================================================
; Xonsh-specific
; ===========================================================================

(block_macro_statement body: (block) @fold)

; Decorated definitions — fold the whole decorator + def/class
(decorated_definition) @fold

; ===========================================================================
; Brackets / multi-line constructs
; ===========================================================================

(argument_list) @fold
(parameters) @fold
(parenthesized_expression) @fold
(list) @fold
(dictionary) @fold
(set) @fold
(tuple) @fold

; ===========================================================================
; Imports
; ===========================================================================

(import_from_statement) @fold

; ===========================================================================
; Strings and comments
; ===========================================================================

(string) @fold
(comment) @fold
