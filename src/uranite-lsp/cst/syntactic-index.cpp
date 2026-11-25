
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

#include <cstring>
#include <string>
#include <vector>

#include "uranite-lsp/cst/syntactic-index.hpp"

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

		std::string trim( const std::string& value ) {
			const std::size_t first = value.find_first_not_of( " \t\r\n" );
			if( first == std::string::npos ) return "";
			const std::size_t last = value.find_last_not_of( " \t\r\n" );
			return value.substr( first, last - first + 1 );
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
		// Split the source into lines first for multi-line brace-block detection.
		std::vector<std::string> allLines;
		{
			std::size_t start = 0;
			for( std::size_t i = 0; i <= source.size(); i++ ) {
				if( i == source.size() || source[ i ] == '\n' ) {
					allLines.push_back( source.substr( start, i - start ) );
					start = i + 1;
				}
			}
		}

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

						// Single-line "from " / "import " forms.
						if( trimmed.rfind( "from ", 0 ) == 0 ) {
							std::size_t importPos = trimmed.find( " import" );
							if( importPos != std::string::npos ) {
								result.kind = CompletionContextKind::ImportSymbol;
								result.modulePath = trim( trimmed.substr( 5, importPos - 5 ) );
								return result;
							}
							result.kind = CompletionContextKind::ImportPath;
							result.modulePath = trim( trimmed.substr( 5 ) );
							return result;
						}
						if( trimmed.rfind( "import ", 0 ) == 0 ) {
							result.kind = CompletionContextKind::ImportPath;
							result.modulePath = trim( trimmed.substr( 7 ) );
							return result;
						}

						// Multi-line brace import blocks: opener on a preceding
						// line, cursor on any line before the matching close brace.
						if( allLines.size() > 1 && line > 0 ) {
							for( uint32_t back = 1; back <= 30 && line >= back; back++ ) {
								const std::string& opener = allLines[ line - back ];
								const std::size_t firstNonSpace = opener.find_first_not_of( " \t" );
								const std::string trimmedOpener = firstNonSpace == std::string::npos ? "" : opener.substr( firstNonSpace );
								if( trimmedOpener.rfind( "from ", 0 ) == 0 ) {
									const std::size_t importPos = trimmedOpener.find( " import" );
									if( importPos != std::string::npos && trimmedOpener.find( '{' ) != std::string::npos ) {
										// Confirm the cursor is before the close brace.
										uint32_t closeLine = line;
										for( uint32_t forward = line; forward < allLines.size(); forward++ ) {
											if( allLines[ forward ].find( '}' ) != std::string::npos ) { closeLine = forward; break; }
										}
										if( line <= closeLine ) {
											result.kind = CompletionContextKind::ImportSymbol;
											result.modulePath = trim( trimmedOpener.substr( 5, importPos - 5 ) );
											return result;
										}
									}
								}
								if( back > 1 && firstNonSpace != std::string::npos &&
								    opener.find_first_not_of( " \t" ) == 0 ) break;
							}
						}

						// G7: static / namespace access after a double colon.
						if( trimmed.size() >= 2 && trimmed.compare( trimmed.size() - 2, 2, "::" ) == 0 ) {
							result.kind = CompletionContextKind::Identifier;
							result.afterDoubleColon = true;
							return result;
						}
						break;
					}
					currentLine++;
					lineStart = i + 1;
				}
			}
		}

		// G10: asm arch blocks. The installed grammar build does not always
		// expose assembly nodes (whole blocks parse as error recovery), so the
		// region is detected textually: an "asm [volatile]:" opener above with
		// the cursor deeper indented than the opener.
		{
			std::vector<std::string> lines;
			{
				std::size_t lineStart = 0;
				for( std::size_t i = 0; i <= source.size(); i++ ) {
					if( i == source.size() || source[ i ] == '\n' ) {
						lines.push_back( source.substr( lineStart, i - lineStart ) );
						lineStart = i + 1;
					}
				}
			}
			auto indentOf = []( const std::string& text ) -> uint32_t {
				uint32_t count = 0;
				for( char c : text ) {
					if( c == ' ' || c == '\t' ) count++;
					else break;
				}
				return count;
			};
			if( line < lines.size() && line > 0 ) {
				const uint32_t cursorIndent = indentOf( lines[ line ] );
				for( uint32_t back = 1; back <= 30 && line >= back; back++ ) {
					const std::string& candidate = lines[ line - back ];
					const std::string t = trim( candidate );
					if( t.empty() ) continue;
					const uint32_t candidateIndent = indentOf( candidate );
					if( ( t == "asm volatile:" || t == "asm:" ) && candidateIndent < cursorIndent ) {
						result.kind = CompletionContextKind::AsmBlock;
						return result;
					}
					if( candidateIndent < cursorIndent ) break;
				}
			}
		}

		TSNode current = node;
		for( int depth = 0; depth < 16 && ts_node_is_null( current ) == false; depth++ ) {
			const char* t = nodeType( current );
			if( t == nullptr ) { current = ts_node_parent( current ); continue; }

			// G10: any cursor inside an inline assembly statement classifies as AsmBlock,
			// checked first so operand clauses do not shadow as call arguments.
			if( std::strcmp( t, "inline_assembly_statement" ) == 0 ||
			    std::strcmp( t, "assembly_arch_variant" ) == 0 ||
			    std::strcmp( t, "assembly_output_clause" ) == 0 ||
			    std::strcmp( t, "assembly_input_clause" ) == 0 ||
			    std::strcmp( t, "assembly_clobber_clause" ) == 0 ||
			    std::strcmp( t, "assembly_operand" ) == 0 ) {
				result.kind = CompletionContextKind::AsmBlock;
				return result;
			}

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
				// Error recovery can glue expression tokens into a type node;
				// a standalone '=' before the cursor marks an assignment value,
				// which is always an expression position.
				{
					uint32_t currentLine = 0;
					std::size_t lineStart = 0;
					for( std::size_t i = 0; i <= source.size(); i++ ) {
						if( i == source.size() || source[ i ] == '\n' ) {
							if( currentLine == line ) {
								const std::string lineText = source.substr( lineStart, i - lineStart );
								const std::size_t segmentEnd = column < lineText.size() ? column : lineText.size();
								const std::string before = lineText.substr( 0, segmentEnd );
								for( std::size_t position = 0; position < before.size(); position++ ) {
									if( before[ position ] != '=' ) continue;
									const char previous = position == 0 ? ' ' : before[ position - 1 ];
									const char next = position + 1 < before.size() ? before[ position + 1 ] : ' ';
									if( previous == '=' || next == '=' || next == '>' ||
									    previous == '<' || previous == '>' || previous == '!' ) continue;
									result.kind = CompletionContextKind::Identifier;
									return result;
								}
								break;
							}
							currentLine++;
							lineStart = i + 1;
						}
					}
				}
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
			    std::strcmp( t, "struct_literal" ) == 0 ||
			    // G9: the actual grammar node kind for brace bodies.
			    std::strcmp( t, "dict_literal" ) == 0 ) {
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
