//
// @author hxAri (hxari)
// @create 2026-11-23 00:00
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

#include "uranite-lsp/cst/syntactic-index.hpp"

#include <cstring>
#include <string>

namespace uranite::lsp::cst {

	namespace {

		const char* nodeType( TSNode node ) {
			if( ts_node_is_null( node ) ) return "";
			return ts_node_type( node );
		}

		std::string nodeText( const std::string& source, TSNode node ) {
			if( ts_node_is_null( node ) ) return "";
			uint32_t start = ts_node_start_byte( node );
			uint32_t end = ts_node_end_byte( node );
			if( end <= start || end > source.size() ) return "";
			return source.substr( start, end - start );
		}

		std::string prefixAt( const std::string& source, uint32_t line, uint32_t column ) {
			uint32_t currentLine = 0;
			std::size_t lineStart = 0;
			for( std::size_t i = 0; i <= source.size(); i++ ) {
				if( i == source.size() || source[ i ] == '\n' ) {
					if( currentLine == line ) {
						std::size_t end = lineStart + column;
						if( end > i ) end = i;
						std::size_t start = end;
						while( start > lineStart ) {
							char ch = source[ start - 1 ];
							if( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ||
							    ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '.' ) {
								start--;
							}
							else break;
						}
						return source.substr( start, end - start );
					}
					currentLine++;
					lineStart = i + 1;
				}
			}
			return "";
		}

	} // namespace

	CompletionContext SyntacticIndex::completionContext( const std::string& source, TSNode root,
	                                                    uint32_t line, uint32_t column ) {
		CompletionContext result;
		result.prefix = prefixAt( source, line, column );
		if( ts_node_is_null( root ) ) {
			result.kind = CompletionContextKind::Identifier;
			return result;
		}

		TSPoint point = { line, column };
		TSNode node = ts_node_descendant_for_point_range( root, point, point );
		if( ts_node_is_null( node ) ) node = root;

		// Check line text for import contexts as CST can be incomplete.
		{
			uint32_t currentLine = 0;
			std::size_t lineStart = 0;
			for( std::size_t i = 0; i <= source.size(); i++ ) {
				if( i == source.size() || source[ i ] == '\n' ) {
					if( currentLine == line ) {
						std::string lineText = source.substr( lineStart, i - lineStart );
						std::size_t col = column < lineText.size() ? column : lineText.size();
						std::string before = lineText.substr( 0, col );
						// Trim leading spaces for checks
						std::size_t first = before.find_first_not_of( " \t" );
						std::string trimmed = ( first == std::string::npos ) ? "" : before.substr( first );
						if( trimmed.rfind( "from ", 0 ) == 0 ) {
							std::size_t importPos = trimmed.find( " import" );
							if( importPos != std::string::npos ) {
								result.kind = CompletionContextKind::ImportSymbol;
								return result;
							}
							result.kind = CompletionContextKind::ImportPath;
							return result;
						}
						if( trimmed.rfind( "import ", 0 ) == 0 ) {
							result.kind = CompletionContextKind::ImportPath;
							return result;
						}
						break;
					}
					currentLine++;
					lineStart = i + 1;
				}
			}
		}

		TSNode current = node;
		for( int depth = 0; depth < 16 && ts_node_is_null( current ) == false; depth++ ) {
			const char* t = nodeType( current );
			if( t == nullptr ) { current = ts_node_parent( current ); continue; }

			if( std::strcmp( t, "import_declaration" ) == 0 ||
			    std::strcmp( t, "import_statement" ) == 0 ||
			    std::strcmp( t, "import_from_statement" ) == 0 ||
			    std::strcmp( t, "from_import_declaration" ) == 0 ) {
				// Determine if after import keyword
				std::string text = nodeText( source, current );
				std::size_t importPos = text.find( " import" );
				if( importPos != std::string::npos ) {
					// if cursor is after import keyword in this node
					uint32_t nodeStart = ts_node_start_byte( current );
					uint32_t cursorByte = 0;
					// approximate: use line/column to byte not easy; fallback to ImportPath
					(void) nodeStart; (void) cursorByte;
					if( text.rfind( "from ", 0 ) == 0 ) result.kind = CompletionContextKind::ImportSymbol;
					else result.kind = CompletionContextKind::ImportPath;
					return result;
				}
				result.kind = CompletionContextKind::ImportPath;
				return result;
			}

			if( std::strcmp( t, "member_access_expression" ) == 0 ||
			    std::strcmp( t, "field_access" ) == 0 ||
			    std::strcmp( t, "attribute" ) == 0 ) {
				result.kind = CompletionContextKind::MemberAccess;
				result.afterDot = true;
				return result;
			}

			if( std::strcmp( t, "type_specifier" ) == 0 ||
			    std::strcmp( t, "generic_type" ) == 0 ||
			    std::strcmp( t, "type_identifier" ) == 0 ||
			    std::strcmp( t, "return_type" ) == 0 ) {
				result.kind = CompletionContextKind::TypeAnnotation;
				return result;
			}

			if( std::strcmp( t, "call_expression" ) == 0 ||
			    std::strcmp( t, "arguments" ) == 0 ||
			    std::strcmp( t, "argument_list" ) == 0 ) {
				result.kind = CompletionContextKind::CallArgument;
				return result;
			}

			if( std::strcmp( t, "struct_initializer" ) == 0 ||
			    std::strcmp( t, "object_literal" ) == 0 ||
			    std::strcmp( t, "struct_literal" ) == 0 ) {
				result.kind = CompletionContextKind::StructInit;
				return result;
			}

			current = ts_node_parent( current );
		}

		// Fallback: check if line starts at statement position
		{
			uint32_t currentLine = 0;
			std::size_t lineStart = 0;
			for( std::size_t i = 0; i <= source.size(); i++ ) {
				if( i == source.size() || source[ i ] == '\n' ) {
					if( currentLine == line ) {
						std::string lineText = source.substr( lineStart, i - lineStart );
						std::size_t col = column < lineText.size() ? column : lineText.size();
						std::string before = lineText.substr( 0, col );
						std::size_t first = before.find_first_not_of( " \t" );
						std::string trimmed = ( first == std::string::npos ) ? "" : before.substr( first );
						if( trimmed.empty() || trimmed == result.prefix ) {
							result.kind = CompletionContextKind::Keyword;
							return result;
						}
						break;
					}
					currentLine++;
					lineStart = i + 1;
				}
			}
		}

		result.kind = CompletionContextKind::Identifier;
		return result;
	}

} // namespace uranite::lsp::cst
