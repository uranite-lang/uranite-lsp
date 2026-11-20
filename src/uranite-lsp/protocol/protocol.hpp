
// 
// @author hxAri (hxari)
// @create 2026-06-18 15:30
// @update -
// @github https://github.com/uranite-lang/uranite-lsp
// 
// Uranite Copyright (c) 2025 - hxAri <hxari@proton.me>
// Uranite Licence under GNU General Public Licence v3
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
// 
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
// 

#ifndef _URANITE_LSP_PROTOCOL_HPP_
#define _URANITE_LSP_PROTOCOL_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace uranite::lsp {

	struct Position {
		uint32_t line = 0;
		uint32_t character = 0;
	};

	struct Range {
		Position start;
		Position end;
	};

	struct Location {
		std::string uri;
		Range range;
	};

	struct TextDocumentIdentifier {
		std::string uri;
	};

	struct VersionedTextDocumentIdentifier {
		std::string uri;
		int64_t version = 0;
	};

	struct TextDocumentItem {
		std::string uri;
		std::string languageId;
		int64_t version = 0;
		std::string text;
	};

	struct ContentChangeEvent {
		bool hasRange = false;
		Range range;
		std::string text;
	};

	struct Diagnostic {
		Range range;
		int32_t severity = 1;
		std::string source;
		std::string message;
	};

	struct CompletionItem {
		std::string label;
		int32_t kind = 1;
		std::string detail;
		std::string insertText;
	};

	struct SemanticToken {
		uint32_t deltaLine = 0;
		uint32_t deltaStartCharacter = 0;
		uint32_t length = 0;
		uint32_t tokenType = 0;
		uint32_t tokenModifiers = 0;
	};

	struct FoldingRange {
		uint32_t startLine = 0;
		uint32_t startCharacter = 0;
		uint32_t endLine = 0;
		uint32_t endCharacter = 0;
		std::string kind;
	};

	struct DocumentSymbol {
		std::string name;
		int32_t kind = 0;
		Range range;
		Range selectionRange;
		std::vector<DocumentSymbol> children;
	};

	struct InlayHint {
		Position position;
		std::string label;
		int32_t kind = 1;
	};

	struct ParameterInformation {
		std::string label;
		std::string documentation;
	};

	struct SignatureInformation {
		std::string label;
		std::string documentation;
		std::vector<ParameterInformation> parameters;
	};

	struct SignatureHelp {
		std::vector<SignatureInformation> signatures;
		uint32_t activeSignature = 0;
		uint32_t activeParameter = 0;
	};

	struct TextEdit {
		Range range;
		std::string newText;
	};

	struct TextDocumentEdit {
		std::string documentUri;
		std::vector<TextEdit> edits;
	};

	namespace DiagnosticSeverity {
		static constexpr int32_t ERROR = 1;
		static constexpr int32_t WARNING = 2;
		static constexpr int32_t INFORMATION = 3;
		static constexpr int32_t HINT = 4;
	}

	namespace SymbolKind {
		static constexpr int32_t FILE = 1;
		static constexpr int32_t MODULE = 2;
		static constexpr int32_t NAMESPACE = 3;
		static constexpr int32_t PACKAGE = 4;
		static constexpr int32_t CLASS = 5;
		static constexpr int32_t METHOD = 6;
		static constexpr int32_t PROPERTY = 7;
		static constexpr int32_t FIELD = 8;
		static constexpr int32_t CONSTRUCTOR = 9;
		static constexpr int32_t ENUM = 10;
		static constexpr int32_t INTERFACE = 11;
		static constexpr int32_t FUNCTION = 12;
		static constexpr int32_t VARIABLE = 13;
		static constexpr int32_t CONSTANT = 14;
		static constexpr int32_t STRING = 15;
		static constexpr int32_t NUMBER = 16;
		static constexpr int32_t BOOLEAN = 17;
		static constexpr int32_t ARRAY = 18;
		static constexpr int32_t STRUCT = 22;
		static constexpr int32_t ENUM_MEMBER = 22;
		static constexpr int32_t TYPE_PARAMETER = 26;
	}

	namespace CompletionItemKind {
		static constexpr int32_t TEXT = 1;
		static constexpr int32_t METHOD = 2;
		static constexpr int32_t FUNCTION = 3;
		static constexpr int32_t CONSTRUCTOR = 4;
		static constexpr int32_t FIELD = 5;
		static constexpr int32_t VARIABLE = 6;
		static constexpr int32_t CLASS = 7;
		static constexpr int32_t INTERFACE = 8;
		static constexpr int32_t MODULE = 9;
		static constexpr int32_t PROPERTY = 10;
		static constexpr int32_t KEYWORD = 14;
		static constexpr int32_t SNIPPET = 15;
		static constexpr int32_t ENUM = 13;
		static constexpr int32_t ENUM_MEMBER = 20;
		static constexpr int32_t STRUCT = 22;
		static constexpr int32_t TYPE_PARAMETER = 25;
	}

	namespace SemanticTokenTypes {
		static constexpr uint32_t NAMESPACE = 0;
		static constexpr uint32_t TYPE = 1;
		static constexpr uint32_t CLASS = 2;
		static constexpr uint32_t ENUM = 3;
		static constexpr uint32_t INTERFACE = 4;
		static constexpr uint32_t STRUCT = 5;
		static constexpr uint32_t TYPE_PARAMETER = 6;
		static constexpr uint32_t PARAMETER = 7;
		static constexpr uint32_t VARIABLE = 8;
		static constexpr uint32_t PROPERTY = 9;
		static constexpr uint32_t ENUM_MEMBER = 10;
		static constexpr uint32_t FUNCTION = 11;
		static constexpr uint32_t METHOD = 12;
		static constexpr uint32_t KEYWORD = 13;
		static constexpr uint32_t MODIFIER = 14;
		static constexpr uint32_t COMMENT = 15;
		static constexpr uint32_t STRING = 16;
		static constexpr uint32_t NUMBER = 17;
		static constexpr uint32_t OPERATOR = 18;
	}

} // namespace uranite::lsp

#endif // end _URANITE_LSP_PROTOCOL_HPP_
