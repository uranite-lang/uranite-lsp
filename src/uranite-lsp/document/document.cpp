
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

#include <algorithm>
#include <cstring>

#include "uranite-lsp/document/document.hpp"

namespace uranite::lsp {

	Document::Document( const std::string& documentUri, const std::string& initialContent, int64_t initialVersion )
		: documentUri( documentUri ),
		  documentContent( initialContent ),
		  documentVersion( initialVersion ) {
		this->syntaxParser = ts_parser_new();
		ts_parser_set_language( this->syntaxParser, tree_sitter_uranite() );
		this->parsedTree = ts_parser_parse_string(
			this->syntaxParser, nullptr,
			this->documentContent.c_str(),
			static_cast<uint32_t>( this->documentContent.size() )
		);
		this->rebuildLineCache();
	}

	Document::~Document() {
		if( this->parsedTree != nullptr ) {
			ts_tree_delete( this->parsedTree );
		}
		if( this->syntaxParser != nullptr ) {
			ts_parser_delete( this->syntaxParser );
		}
	}

	void Document::rebuildLineCache() {
		this->cachedSourceLines.clear();
		std::string currentLine;
		for( char character : this->documentContent ) {
			if( character == '\n' ) {
				this->cachedSourceLines.push_back( currentLine );
				currentLine.clear();
			}
			else {
				currentLine += character;
			}
		}
		if( currentLine.empty() == false ) {
			this->cachedSourceLines.push_back( currentLine );
		}
	}

	uint32_t Document::computeByteOffset( const Position& position ) const {
		uint32_t byteOffset = 0;
		for( uint32_t lineIndex = 0; lineIndex < position.line && lineIndex < this->cachedSourceLines.size(); ++lineIndex ) {
			byteOffset += static_cast<uint32_t>( this->cachedSourceLines[lineIndex].size() ) + 1;
		}
		if( position.line < this->cachedSourceLines.size() ) {
			uint32_t columnLimit = std::min(
				position.character,
				static_cast<uint32_t>( this->cachedSourceLines[position.line].size() )
			);
			byteOffset += columnLimit;
		}
		return byteOffset;
	}

	Position Document::computePositionFromByte( uint32_t byteOffset ) const {
		Position computedPosition;
		uint32_t remainingBytes = byteOffset;
		for( uint32_t lineIndex = 0; lineIndex < this->cachedSourceLines.size(); ++lineIndex ) {
			uint32_t lineLength = static_cast<uint32_t>( this->cachedSourceLines[lineIndex].size() ) + 1;
			if( remainingBytes < lineLength ) {
				computedPosition.line = lineIndex;
				computedPosition.character = remainingBytes;
				return computedPosition;
			}
			remainingBytes -= lineLength;
		}
		computedPosition.line = static_cast<uint32_t>( this->cachedSourceLines.size() );
		computedPosition.character = 0;
		return computedPosition;
	}

	void Document::refreshSemanticAnalysis( const std::string& documentFilePath, const std::string& workspaceRootPath ) {
		this->cachedSemanticResult = performSemanticAnalysis(
			this->documentContent,
			documentFilePath,
			workspaceRootPath
		);
		this->semanticAnalysisStale = false;
	}

	void Document::applyContentChanges( const std::vector<ContentChangeEvent>& contentChanges, int64_t newVersion ) {
		std::lock_guard<std::mutex> lockGuard( this->documentMutex );
		this->semanticAnalysisStale = true;

		for( const ContentChangeEvent& contentChange : contentChanges ) {
			if( contentChange.hasRange == false ) {
				this->documentContent = contentChange.text;
				if( this->parsedTree != nullptr ) {
					ts_tree_delete( this->parsedTree );
				}
				this->rebuildLineCache();
				this->parsedTree = ts_parser_parse_string(
					this->syntaxParser, nullptr,
					this->documentContent.c_str(),
					static_cast<uint32_t>( this->documentContent.size() )
				);
				continue;
			}

			uint32_t oldStartByte = this->computeByteOffset( contentChange.range.start );
			uint32_t oldEndByte = this->computeByteOffset( contentChange.range.end );
			uint32_t newEndByte = oldStartByte + static_cast<uint32_t>( contentChange.text.size() );

			TSPoint startPoint = {
				contentChange.range.start.line,
				contentChange.range.start.character
			};
			TSPoint oldEndPoint = {
				contentChange.range.end.line,
				contentChange.range.end.character
			};

			uint32_t newEndLine = contentChange.range.start.line;
			uint32_t newEndColumn = contentChange.range.start.character;
			for( char character : contentChange.text ) {
				if( character == '\n' ) {
					newEndLine++;
					newEndColumn = 0;
				}
				else {
					newEndColumn++;
				}
			}
			TSPoint newEndPoint = { newEndLine, newEndColumn };

			TSInputEdit appliedEdit = {
				oldStartByte,
				oldEndByte,
				newEndByte,
				startPoint,
				oldEndPoint,
				newEndPoint
			};

			if( this->parsedTree != nullptr ) {
				ts_tree_edit( this->parsedTree, &appliedEdit );
			}

			this->documentContent.replace( oldStartByte, oldEndByte - oldStartByte, contentChange.text );
			this->rebuildLineCache();
		}

		TSTree* updatedTree = ts_parser_parse_string(
			this->syntaxParser,
			this->parsedTree,
			this->documentContent.c_str(),
			static_cast<uint32_t>( this->documentContent.size() )
		);
		if( this->parsedTree != nullptr ) {
			ts_tree_delete( this->parsedTree );
		}
		this->parsedTree = updatedTree;
		this->documentVersion = newVersion;
	}

	TSNode Document::rootNode() const {
		if( this->parsedTree == nullptr ) {
			return TSNode{};
		}
		return ts_tree_root_node( this->parsedTree );
	}

	TSNode Document::findNodeAtPosition( const Position& targetPosition ) const {
		if( this->parsedTree == nullptr ) {
			return TSNode{};
		}
		TSPoint targetPoint = { targetPosition.line, targetPosition.character };
		TSNode rootSyntaxNode = ts_tree_root_node( this->parsedTree );
		return ts_node_descendant_for_point_range( rootSyntaxNode, targetPoint, targetPoint );
	}

	std::string Document::nodeText( TSNode targetNode ) const {
		uint32_t startByte = ts_node_start_byte( targetNode );
		uint32_t endByte = ts_node_end_byte( targetNode );
		if( endByte > this->documentContent.size() ) {
			endByte = static_cast<uint32_t>( this->documentContent.size() );
		}
		if( startByte >= endByte ) {
			return "";
		}
		return this->documentContent.substr( startByte, endByte - startByte );
	}

	std::vector<Diagnostic> Document::collectTreeErrors() const {
		std::vector<Diagnostic> collectedDiagnostics;
		if( this->parsedTree == nullptr ) {
			return collectedDiagnostics;
		}

		TSNode rootSyntaxNode = ts_tree_root_node( this->parsedTree );
		TSTreeCursor treeCursor = ts_tree_cursor_new( rootSyntaxNode );

		bool continueWalking = true;
		while( continueWalking ) {
			TSNode currentNode = ts_tree_cursor_current_node( &treeCursor );

			if( ts_node_is_error( currentNode ) || ts_node_is_missing( currentNode ) ) {
				TSPoint startPoint = ts_node_start_point( currentNode );
				TSPoint endPoint = ts_node_end_point( currentNode );

				Diagnostic errorDiagnostic;
				errorDiagnostic.range.start.line = startPoint.row;
				errorDiagnostic.range.start.character = startPoint.column;
				errorDiagnostic.range.end.line = endPoint.row;
				errorDiagnostic.range.end.character = endPoint.column;
				errorDiagnostic.severity = DiagnosticSeverity::ERROR;
				errorDiagnostic.source = "uranite";

				if( ts_node_is_missing( currentNode ) ) {
					errorDiagnostic.message = std::string( "Missing " ) + ts_node_type( currentNode );
				}
				else {
					errorDiagnostic.message = "Syntax error";
					std::string errorNodeText = this->nodeText( currentNode );
					if( errorNodeText.empty() == false && errorNodeText.size() <= 40 ) {
						errorDiagnostic.message += " near '" + errorNodeText + "'";
					}
				}

				collectedDiagnostics.push_back( errorDiagnostic );
			}

			if( ts_tree_cursor_goto_first_child( &treeCursor ) ) {
				continue;
			}
			while( ts_tree_cursor_goto_next_sibling( &treeCursor ) == false ) {
				if( ts_tree_cursor_goto_parent( &treeCursor ) == false ) {
					continueWalking = false;
					break;
				}
			}
		}

		ts_tree_cursor_delete( &treeCursor );
		return collectedDiagnostics;
	}

	std::vector<FoldingRange> Document::collectFoldingRanges() const {
		std::vector<FoldingRange> collectedRanges;
		if( this->parsedTree == nullptr ) {
			return collectedRanges;
		}

		TSNode rootSyntaxNode = ts_tree_root_node( this->parsedTree );
		TSTreeCursor treeCursor = ts_tree_cursor_new( rootSyntaxNode );

		bool continueWalking = true;
		while( continueWalking ) {
			TSNode currentNode = ts_tree_cursor_current_node( &treeCursor );
			const char* nodeTypeName = ts_node_type( currentNode );

			bool isFoldable = (
				std::strcmp( nodeTypeName, "indented_body" ) == 0 ||
				std::strcmp( nodeTypeName, "import_item_brace_list" ) == 0 ||
				std::strcmp( nodeTypeName, "block_comment" ) == 0 ||
				std::strcmp( nodeTypeName, "docstring_comment" ) == 0 ||
				std::strcmp( nodeTypeName, "export_block" ) == 0
			);

			if( isFoldable ) {
				TSPoint startPoint = ts_node_start_point( currentNode );
				TSPoint endPoint = ts_node_end_point( currentNode );

				if( endPoint.row > startPoint.row ) {
					FoldingRange foldRange;
					foldRange.startLine = startPoint.row;
					foldRange.startCharacter = startPoint.column;
					foldRange.endLine = endPoint.row;
					foldRange.endCharacter = endPoint.column;

					if( std::strcmp( nodeTypeName, "block_comment" ) == 0 ||
						std::strcmp( nodeTypeName, "docstring_comment" ) == 0 ) {
						foldRange.kind = "comment";
					}
					else if( std::strcmp( nodeTypeName, "import_item_brace_list" ) == 0 ) {
						foldRange.kind = "imports";
					}
					else {
						foldRange.kind = "region";
					}

					collectedRanges.push_back( foldRange );
				}
			}

			if( ts_tree_cursor_goto_first_child( &treeCursor ) ) {
				continue;
			}
			while( ts_tree_cursor_goto_next_sibling( &treeCursor ) == false ) {
				if( ts_tree_cursor_goto_parent( &treeCursor ) == false ) {
					continueWalking = false;
					break;
				}
			}
		}

		ts_tree_cursor_delete( &treeCursor );
		return collectedRanges;
	}

	static int32_t nodeTypeToSymbolKind( const char* nodeTypeName ) {
		if( std::strcmp( nodeTypeName, "function_declaration" ) == 0 ) return SymbolKind::FUNCTION;
		if( std::strcmp( nodeTypeName, "class_declaration" ) == 0 ) return SymbolKind::CLASS;
		if( std::strcmp( nodeTypeName, "struct_declaration" ) == 0 ) return SymbolKind::STRUCT;
		if( std::strcmp( nodeTypeName, "enum_declaration" ) == 0 ) return SymbolKind::ENUM;
		if( std::strcmp( nodeTypeName, "interface_declaration" ) == 0 ) return SymbolKind::INTERFACE;
		if( std::strcmp( nodeTypeName, "trait_declaration" ) == 0 ) return SymbolKind::INTERFACE;
		if( std::strcmp( nodeTypeName, "constant_declaration" ) == 0 ) return SymbolKind::CONSTANT;
		if( std::strcmp( nodeTypeName, "variable_declaration" ) == 0 ) return SymbolKind::VARIABLE;
		if( std::strcmp( nodeTypeName, "type_alias_declaration" ) == 0 ) return SymbolKind::TYPE_PARAMETER;
		if( std::strcmp( nodeTypeName, "package_declaration" ) == 0 ) return SymbolKind::PACKAGE;
		if( std::strcmp( nodeTypeName, "enum_variant" ) == 0 ) return SymbolKind::ENUM_MEMBER;
		if( std::strcmp( nodeTypeName, "property_declaration" ) == 0 ) return SymbolKind::PROPERTY;
		return -1;
	}

	std::vector<DocumentSymbol> Document::collectDocumentSymbols() const {
		std::vector<DocumentSymbol> collectedSymbols;
		if( this->parsedTree == nullptr ) {
			return collectedSymbols;
		}

		TSNode rootSyntaxNode = ts_tree_root_node( this->parsedTree );
		uint32_t topLevelChildCount = ts_node_child_count( rootSyntaxNode );

		for( uint32_t childIndex = 0; childIndex < topLevelChildCount; ++childIndex ) {
			TSNode childNode = ts_node_child( rootSyntaxNode, childIndex );
			const char* nodeTypeName = ts_node_type( childNode );

			int32_t symbolKind = nodeTypeToSymbolKind( nodeTypeName );
			if( symbolKind < 0 ) {
				continue;
			}

			TSNode nameNode = ts_node_child_by_field_name( childNode, "declared_name", 13 );
			if( ts_node_is_null( nameNode ) ) {
				nameNode = ts_node_child_by_field_name( childNode, "package_path", 12 );
			}
			if( ts_node_is_null( nameNode ) ) {
				continue;
			}

			DocumentSymbol documentSymbol;
			documentSymbol.name = this->nodeText( nameNode );
			documentSymbol.kind = symbolKind;

			TSPoint nodeStart = ts_node_start_point( childNode );
			TSPoint nodeEnd = ts_node_end_point( childNode );
			documentSymbol.range.start = { nodeStart.row, nodeStart.column };
			documentSymbol.range.end = { nodeEnd.row, nodeEnd.column };

			TSPoint nameStart = ts_node_start_point( nameNode );
			TSPoint nameEnd = ts_node_end_point( nameNode );
			documentSymbol.selectionRange.start = { nameStart.row, nameStart.column };
			documentSymbol.selectionRange.end = { nameEnd.row, nameEnd.column };

			collectedSymbols.push_back( documentSymbol );
		}

		return collectedSymbols;
	}

	static uint32_t captureNameToTokenType( const char* captureName ) {
		if( std::strcmp( captureName, "keyword" ) == 0 ) return SemanticTokenTypes::KEYWORD;
		if( std::strcmp( captureName, "keyword.control.flow" ) == 0 ) return SemanticTokenTypes::KEYWORD;
		if( std::strcmp( captureName, "keyword.modifier" ) == 0 ) return SemanticTokenTypes::MODIFIER;
		if( std::strcmp( captureName, "keyword.modifier.access" ) == 0 ) return SemanticTokenTypes::MODIFIER;
		if( std::strcmp( captureName, "keyword.memory" ) == 0 ) return SemanticTokenTypes::KEYWORD;
		if( std::strcmp( captureName, "keyword.coroutine" ) == 0 ) return SemanticTokenTypes::KEYWORD;
		if( std::strcmp( captureName, "keyword.operator" ) == 0 ) return SemanticTokenTypes::OPERATOR;
		if( std::strcmp( captureName, "keyword.directive" ) == 0 ) return SemanticTokenTypes::KEYWORD;
		if( std::strcmp( captureName, "type" ) == 0 ) return SemanticTokenTypes::TYPE;
		if( std::strcmp( captureName, "type.definition" ) == 0 ) return SemanticTokenTypes::TYPE;
		if( std::strcmp( captureName, "type.parameter" ) == 0 ) return SemanticTokenTypes::TYPE_PARAMETER;
		if( std::strcmp( captureName, "function" ) == 0 ) return SemanticTokenTypes::FUNCTION;
		if( std::strcmp( captureName, "function.call" ) == 0 ) return SemanticTokenTypes::FUNCTION;
		if( std::strcmp( captureName, "function.method.call" ) == 0 ) return SemanticTokenTypes::METHOD;
		if( std::strcmp( captureName, "variable" ) == 0 ) return SemanticTokenTypes::VARIABLE;
		if( std::strcmp( captureName, "variable.parameter" ) == 0 ) return SemanticTokenTypes::PARAMETER;
		if( std::strcmp( captureName, "variable.builtin" ) == 0 ) return SemanticTokenTypes::VARIABLE;
		if( std::strcmp( captureName, "constant" ) == 0 ) return SemanticTokenTypes::VARIABLE;
		if( std::strcmp( captureName, "constant.builtin" ) == 0 ) return SemanticTokenTypes::VARIABLE;
		if( std::strcmp( captureName, "property" ) == 0 ) return SemanticTokenTypes::PROPERTY;
		if( std::strcmp( captureName, "module" ) == 0 ) return SemanticTokenTypes::NAMESPACE;
		if( std::strcmp( captureName, "number" ) == 0 ) return SemanticTokenTypes::NUMBER;
		if( std::strcmp( captureName, "number.float" ) == 0 ) return SemanticTokenTypes::NUMBER;
		if( std::strcmp( captureName, "string" ) == 0 ) return SemanticTokenTypes::STRING;
		if( std::strcmp( captureName, "string.special" ) == 0 ) return SemanticTokenTypes::STRING;
		if( std::strcmp( captureName, "character" ) == 0 ) return SemanticTokenTypes::STRING;
		if( std::strcmp( captureName, "comment" ) == 0 ) return SemanticTokenTypes::COMMENT;
		if( std::strcmp( captureName, "comment.block" ) == 0 ) return SemanticTokenTypes::COMMENT;
		if( std::strcmp( captureName, "comment.documentation" ) == 0 ) return SemanticTokenTypes::COMMENT;
		if( std::strcmp( captureName, "operator" ) == 0 ) return SemanticTokenTypes::OPERATOR;
		return SemanticTokenTypes::VARIABLE;
	}

	std::vector<SemanticToken> Document::collectSemanticTokens() const {
		std::vector<SemanticToken> collectedTokens;
		if( this->parsedTree == nullptr ) {
			return collectedTokens;
		}

		std::string highlightsQuerySource =
			#include "highlights-query-embedded.inc"
		;

		uint32_t queryErrorOffset = 0;
		TSQueryError queryErrorType = TSQueryErrorNone;
		TSQuery* highlightsQuery = ts_query_new(
			tree_sitter_uranite(),
			highlightsQuerySource.c_str(),
			static_cast<uint32_t>( highlightsQuerySource.size() ),
			&queryErrorOffset,
			&queryErrorType
		);

		if( highlightsQuery == nullptr ) {
			return collectedTokens;
		}

		TSQueryCursor* queryCursor = ts_query_cursor_new();
		ts_query_cursor_exec( queryCursor, highlightsQuery, ts_tree_root_node( this->parsedTree ) );

		struct RawSemanticToken {
			uint32_t startRow;
			uint32_t startColumn;
			uint32_t length;
			uint32_t tokenType;
		};
		std::vector<RawSemanticToken> rawTokens;

		TSQueryMatch queryMatch;
		uint32_t captureIndex = 0;
		while( ts_query_cursor_next_capture( queryCursor, &queryMatch, &captureIndex ) ) {
			TSQueryCapture matchedCapture = queryMatch.captures[captureIndex];
			TSNode capturedNode = matchedCapture.node;

			uint32_t captureNameLength = 0;
			const char* captureName = ts_query_capture_name_for_id(
				highlightsQuery, matchedCapture.index, &captureNameLength
			);

			TSPoint nodeStart = ts_node_start_point( capturedNode );
			TSPoint nodeEnd = ts_node_end_point( capturedNode );

			if( nodeStart.row == nodeEnd.row ) {
				RawSemanticToken rawToken;
				rawToken.startRow = nodeStart.row;
				rawToken.startColumn = nodeStart.column;
				rawToken.length = nodeEnd.column - nodeStart.column;
				rawToken.tokenType = captureNameToTokenType( captureName );
				rawTokens.push_back( rawToken );
			}
		}

		ts_query_cursor_delete( queryCursor );
		ts_query_delete( highlightsQuery );

		std::sort( rawTokens.begin(), rawTokens.end(),
			[]( const RawSemanticToken& leftToken, const RawSemanticToken& rightToken ) {
				if( leftToken.startRow != rightToken.startRow ) {
					return leftToken.startRow < rightToken.startRow;
				}
				return leftToken.startColumn < rightToken.startColumn;
			}
		);

		uint32_t previousRow = 0;
		uint32_t previousColumn = 0;
		for( const RawSemanticToken& rawToken : rawTokens ) {
			SemanticToken encodedToken;
			encodedToken.deltaLine = rawToken.startRow - previousRow;
			if( encodedToken.deltaLine > 0 ) {
				encodedToken.deltaStartCharacter = rawToken.startColumn;
			}
			else {
				encodedToken.deltaStartCharacter = rawToken.startColumn - previousColumn;
			}
			encodedToken.length = rawToken.length;
			encodedToken.tokenType = rawToken.tokenType;
			encodedToken.tokenModifiers = 0;
			collectedTokens.push_back( encodedToken );

			previousRow = rawToken.startRow;
			previousColumn = rawToken.startColumn;
		}

		return collectedTokens;
	}

} // namespace uranite::lsp
