package tree_sitter_xonsh_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_xonsh "github.com/FoamScience/tree-sitter-xonsh/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_xonsh.Language())
	if language == nil {
		t.Errorf("Error loading Xonsh grammar")
	}
}
