import XCTest
import SwiftTreeSitter
import TreeSitterXonsh

final class TreeSitterXonshTests: XCTestCase {
    func testCanLoadGrammar() throws {
        let parser = Parser()
        let language = Language(language: tree_sitter_xonsh())
        XCTAssertNoThrow(try parser.setLanguage(language),
                         "Error loading Xonsh grammar")
    }
}
