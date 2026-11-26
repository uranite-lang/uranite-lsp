
//
// @author hxAri (hxari)
// @create 2026-11-22 00:00
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
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "uranite/semantic/typeref.hpp"

#include "uranite-lsp/cst/parse-session.hpp"
#include "uranite-lsp/cst/syntactic-index.hpp"
#include "uranite-lsp/semantic/query-service.hpp"
#include "uranite-lsp/text/document.hpp"
#include "uranite-lsp/text/uri.hpp"

namespace uranite::lsp::semantic {

	namespace {

		using uranite::semantic::CompletionItem;
		using uranite::semantic::FunctionType;
		using uranite::semantic::Symbol;

		std::string trim( const std::string& value ) {
			// Real whitespace characters (the previous " \\t\\r\\n" literal
			// trimmed the letters t/r/n instead of tab, CR and LF).
			const std::size_t first = value.find_first_not_of( " \t\r\n" );
			if( first == std::string::npos ) return "";
			const std::size_t last = value.find_last_not_of( " \t\r\n" );
			return value.substr( first, last - first + 1 );
		}

		int completionKind( Symbol::Kind kind ) {
			switch( kind ) {
				case Symbol::Kind::EnumVariant: return 20; // EnumMember
				case Symbol::Kind::Field: return 5;        // Field
				case Symbol::Kind::Function: return 3;     // Function
				case Symbol::Kind::Module: return 9;       // Module
				case Symbol::Kind::Parameter: return 6;    // Variable
				case Symbol::Kind::Type: return 7;         // Class
				case Symbol::Kind::Variable: return 6;     // Variable
			}
			return 1; // Text
		}

		std::string lower( const std::string& value ) {
			std::string result = value;
			std::transform( result.begin(), result.end(), result.begin(), []( unsigned char c ) {
				return static_cast<char>( std::tolower( c ) );
			} );
			return result;
		}

		std::size_t indentationWidth( const std::string& line ) {
			std::size_t width = 0;
			for( char ch : line ) {
				if( ch == ' ' ) width++;
				else if( ch == '\t' ) width += 4 - ( width % 4 );
				else break;
			}
			return width;
		}

		std::string removeIndentation( const std::string& line, std::size_t columns ) {
			std::size_t position = 0;
			std::size_t width = 0;
			while( position < line.size() && width < columns ) {
				if( line[ position ] == ' ' ) width++;
				else if( line[ position ] == '\t' ) width += 4 - ( width % 4 );
				else break;
				position++;
			}
			return line.substr( position );
		}

		/** @brief Reads a source file through the overlay (unsaved buffers) with disk fallback. */
		std::string fileSourceFor( const OverlayStore* overlayStore, const std::string& path ) {
			if( overlayStore != nullptr ) {
				const std::optional<std::string> overlay = overlayStore->get( path );
				if( overlay.has_value() ) return *overlay;
			}
			std::error_code error;
			if( std::filesystem::is_regular_file( path, error ) == false ) return "";
			std::ifstream file( path );
			if( file.is_open() == false ) return "";
			std::ostringstream buffer;
			buffer << file.rdbuf();
			return buffer.str();
		}

		/** @brief Extracts a complete triple-quoted Uranite docstring with common indentation removed. */
		std::string extractDocstringAfter( const std::string& source, std::size_t fromByte ) {
			std::size_t headerEnd = source.find( '\n', fromByte );
			if( headerEnd == std::string::npos ) headerEnd = source.size();
			std::size_t cursor = headerEnd;
			while( cursor <= source.size() ) {
				std::size_t lineEnd = source.find( '\n', cursor );
				if( lineEnd == std::string::npos ) lineEnd = source.size();
				const std::string line = trim( source.substr( cursor, lineEnd - cursor ) );
				if( line.empty() == false ) {
					if( line.rfind( "\"\"\"", 0 ) != 0 ) return "";
					std::vector<std::string> lines;
					const std::string head = line.substr( 3 );
					const std::size_t inlineClose = head.find( "\"\"\"" );
					if( inlineClose != std::string::npos ) return trim( head.substr( 0, inlineClose ) );
					if( head.empty() == false ) lines.push_back( head );
					std::size_t scan = lineEnd + 1;
					while( scan <= source.size() ) {
						const std::size_t end = source.find( '\n', scan );
						const std::size_t contentEnd = end == std::string::npos ? source.size() : end;
						const std::string next = source.substr( scan, contentEnd - scan );
						const std::size_t stop = next.find( "\"\"\"" );
						if( stop != std::string::npos ) {
							if( stop > 0 ) lines.push_back( next.substr( 0, stop ) );
							break;
						}
						lines.push_back( next );
						if( end == std::string::npos ) break;
						scan = end + 1;
					}
					std::size_t commonIndent = std::numeric_limits<std::size_t>::max();
					for( const std::string& docLine : lines ) {
						if( trim( docLine ).empty() ) continue;
						commonIndent = std::min( commonIndent, indentationWidth( docLine ) );
					}
					if( commonIndent == std::numeric_limits<std::size_t>::max() ) commonIndent = 0;
					std::string result;
					for( const std::string& docLine : lines ) {
						if( result.empty() == false ) result += "\n";
						result += removeIndentation( docLine, commonIndent );
					}
					return trim( result );
				}
				if( lineEnd >= source.size() ) return "";
				cursor = lineEnd + 1;
			}
			return "";
		}

		/** @brief Extracts canonical declaration modifier keywords from a declaration line. */
		std::vector<std::string> extractModifiers( const std::string& lineText ) {
			static const char* modifiers[] = {
				"public", "private", "protect", "abstract", "virtual", "override", "final",
				"readonly", "naked", "native", "async", "const" };
			std::vector<std::string> found;
			for( const char* modifier : modifiers ) {
				const std::string token( modifier );
				std::size_t position = 0;
				while( ( position = lineText.find( token, position ) ) != std::string::npos ) {
					const char before = position == 0 ? ' ' : lineText[ position - 1 ];
					const char after = position + token.size() < lineText.size() ? lineText[ position + token.size() ] : ' ';
					const bool beforeWord = ( before >= 'a' && before <= 'z' ) || ( before >= 'A' && before <= 'Z' ) ||
					                        ( before >= '0' && before <= '9' ) || before == '_';
					const bool afterWord = ( after >= 'a' && after <= 'z' ) || ( after >= 'A' && after <= 'Z' ) ||
					                       ( after >= '0' && after <= '9' ) || after == '_';
					if( beforeWord == false && afterWord == false ) {
						found.push_back( token );
						break;
					}
					position += token.size();
				}
			}
			return found;
		}

		struct ImportTarget {
			enum Kind { None, ModuleSegment, ImportedSymbol };
			Kind kind = None;
			std::string modulePath;
			std::string symbolName;
		};

		ImportTarget importTargetAt( const std::string& source, uint32_t line, uint32_t col ) {
			ImportTarget target;
			cst::ParseSession session;
			session.parse( source );
			TSNode root = session.rootNode();
			if( ts_node_is_null( root ) ) return target;
			TSPoint point = { line, col };
			TSNode node = ts_node_descendant_for_point_range( root, point, point );
			if( ts_node_is_null( node ) ) return target;
			TSNode importNode = TSNode{};
			TSNode current = node;
			for( int depth = 0; depth < 16 && ts_node_is_null( current ) == false; depth++ ) {
				if( std::strcmp( ts_node_type( current ), "import_statement" ) == 0 ) { importNode = current; break; }
				current = ts_node_parent( current );
			}
			if( ts_node_is_null( importNode ) ) return target;
			TSNode dotted = TSNode{};
			uint32_t nch = ts_node_child_count( importNode );
			for( uint32_t i = 0; i < nch; i++ ) {
				TSNode c = ts_node_child( importNode, i );
				if( std::strcmp( ts_node_type( c ), "dotted_identifier" ) == 0 ) dotted = c;
			}
			// Cursor on a segment of the module path.
			if( ts_node_is_null( dotted ) == false ) {
				uint32_t segCount = ts_node_child_count( dotted );
				for( uint32_t i = 0; i < segCount; i++ ) {
					TSNode c = ts_node_child( dotted, i );
					if( std::strcmp( ts_node_type( c ), "identifier" ) != 0 ) continue;
					TSPoint sp = ts_node_start_point( c );
					TSPoint ep = ts_node_end_point( c );
					if( point.row != sp.row || col < sp.column || col > ep.column ) continue;
					std::vector<std::string> segs;
					for( uint32_t j = 0; j <= i; j++ ) {
						TSNode cc = ts_node_child( dotted, j );
						if( std::strcmp( ts_node_type( cc ), "identifier" ) != 0 ) continue;
						int len = ts_node_end_byte( cc ) - ts_node_start_byte( cc );
						segs.push_back( std::string( source.data() + ts_node_start_byte( cc ), len ) );
					}
					for( const std::string& s : segs ) {
						if( target.modulePath.empty() == false ) target.modulePath += ".";
						target.modulePath += s;
					}
					target.kind = ImportTarget::ModuleSegment;
					return target;
				}
			}
			// Cursor on an imported symbol.
			std::function<void( TSNode )> walkItems = [ & ]( TSNode n ) {
				if( ts_node_is_null( n ) ) return;
				const char* t = ts_node_type( n );
				if( std::strcmp( t, "import_item" ) == 0 ) {
					uint32_t ich = ts_node_child_count( n );
					for( uint32_t i = 0; i < ich; i++ ) {
						TSNode c = ts_node_child( n, i );
						if( std::strcmp( ts_node_type( c ), "identifier" ) != 0 ) continue;
						TSPoint sp = ts_node_start_point( c );
						TSPoint ep = ts_node_end_point( c );
						if( point.row != sp.row || col < sp.column || col > ep.column ) continue;
						int len = ts_node_end_byte( c ) - ts_node_start_byte( c );
						target.symbolName = std::string( source.data() + ts_node_start_byte( c ), len );
						target.kind = ImportTarget::ImportedSymbol;
						if( ts_node_is_null( dotted ) == false ) {
							int dlen = ts_node_end_byte( dotted ) - ts_node_start_byte( dotted );
							target.modulePath = std::string( source.data() + ts_node_start_byte( dotted ), dlen );
						}
					}
				}
				uint32_t nch = ts_node_child_count( n );
				for( uint32_t i = 0; i < nch; i++ ) walkItems( ts_node_child( n, i ) );
			};
			walkItems( importNode );
			return target;
		}

		struct DocParameter {
			std::string name;
			std::string type;
			std::string description;
		};

		struct DocRaise {
			std::string type;
			std::string description;
		};

		struct DocReturn {
			std::string type;
			std::string description;
		};

		struct ParsedDocstring {
			std::string space;
			std::string spaceDescription;
			std::string time;
			std::string timeDescription;
			std::vector<DocParameter> parameters;
			std::vector<DocRaise> raises;
			std::vector<DocReturn> returns;
			std::vector<std::string> extendsRelations;
			std::vector<std::string> implementsRelations;
			std::vector<std::string> usesRelations;
			std::vector<std::string> examples;
			std::string descriptions;
		};

		std::string collapseWhitespace( const std::string& value ) {
			std::istringstream stream( value );
			std::string result;
			std::string word;
			while( stream >> word ) {
				if( result.empty() == false ) result += " ";
				result += word;
			}
			return result;
		}

		std::string normalizeParagraphs( const std::vector<std::string>& lines ) {
			std::vector<std::string> paragraphs;
			std::vector<std::string> current;
			auto flush = [ & ]() {
				if( current.empty() ) return;
				std::string paragraph;
				for( const std::string& line : current ) {
					const std::string cleaned = trim( line );
					if( cleaned.empty() ) continue;
					if( paragraph.empty() == false ) paragraph += " ";
					paragraph += cleaned;
				}
				if( paragraph.empty() == false ) paragraphs.push_back( std::move( paragraph ) );
				current.clear();
			};
			for( const std::string& line : lines ) {
				if( trim( line ).empty() ) flush();
				else current.push_back( line );
			}
			flush();
			std::string result;
			for( const std::string& paragraph : paragraphs ) {
				if( result.empty() == false ) result += "\n\n";
				result += paragraph;
			}
			return result;
		}

		void splitNotationDescription( const std::string& value, std::string& notation, std::string& description ) {
			const std::string cleaned = trim( value );
			if( cleaned.empty() ) return;
			if( cleaned[ 0 ] == 'O' && cleaned.size() > 2 && cleaned[ 1 ] == '(' ) {
				std::size_t depth = 0;
				for( std::size_t i = 1; i < cleaned.size(); i++ ) {
					if( cleaned[ i ] == '(' ) depth++;
					else if( cleaned[ i ] == ')' ) {
						depth--;
						if( depth == 0 ) {
							notation = cleaned.substr( 0, i + 1 );
							description = collapseWhitespace( cleaned.substr( i + 1 ) );
							return;
						}
					}
				}
			}
			const std::size_t separator = cleaned.find_first_of( " \t" );
			if( separator == std::string::npos ) notation = cleaned;
			else {
				notation = cleaned.substr( 0, separator );
				description = collapseWhitespace( cleaned.substr( separator + 1 ) );
			}
		}

		std::vector<std::string> splitRelationList( const std::string& value );

		ParsedDocstring parseDocstring( const std::string& docstring ) {
			ParsedDocstring result;
			enum class Section { None, Complexity, Parameters, Raises, Returns, Extends, Implements, Uses, Examples };
			Section section = Section::None;
			std::size_t itemIndent = std::numeric_limits<std::size_t>::max();
			std::vector<std::string> descriptionLines;
			std::vector<std::string> exampleLines;
			std::istringstream stream( docstring );
			std::string line;
			auto appendContinuation = []( std::string& target, const std::string& value ) {
				target = collapseWhitespace( target + " " + value );
			};
			while( std::getline( stream, line ) ) {
				const std::size_t indent = indentationWidth( line );
				const std::string cleaned = trim( line );
				const std::string lowered = lower( cleaned );
				if( indent == 0 ) {
					if( lowered == "complexity:" ) { section = Section::Complexity; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "parameters:" ) { section = Section::Parameters; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "raises:" ) { section = Section::Raises; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "returns:" ) { section = Section::Returns; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "extends:" ) { section = Section::Extends; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "implements:" ) { section = Section::Implements; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "uses:" ) { section = Section::Uses; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( lowered == "examples:" ) { section = Section::Examples; itemIndent = std::numeric_limits<std::size_t>::max(); continue; }
					if( cleaned.empty() == false && section != Section::None ) section = Section::None;
				}

				if( cleaned.empty() ) {
					if( section == Section::None ) descriptionLines.push_back( "" );
					else if( section == Section::Examples ) exampleLines.push_back( "" );
					continue;
				}
				if( section == Section::None ) {
					descriptionLines.push_back( line );
					continue;
				}
				if( section == Section::Examples ) {
					if( indent > 0 ) exampleLines.push_back( line );
					else descriptionLines.push_back( line );
					continue;
				}
				if( indent == 0 ) {
					descriptionLines.push_back( line );
					continue;
				}
				if( section == Section::Complexity ) {
					const std::size_t colon = cleaned.find( ':' );
					if( colon == std::string::npos ) continue;
					const std::string label = lower( trim( cleaned.substr( 0, colon ) ) );
					if( label == "space" ) splitNotationDescription( cleaned.substr( colon + 1 ), result.space, result.spaceDescription );
					else if( label == "time" ) splitNotationDescription( cleaned.substr( colon + 1 ), result.time, result.timeDescription );
					continue;
				}
				if( section == Section::Parameters ) {
					const std::size_t colon = cleaned.find( ':' );
					std::string name;
					std::string type;
					std::string description;
					if( colon != std::string::npos ) {
						const std::string head = trim( cleaned.substr( 0, colon ) );
						const std::size_t open = head.find( '(' );
						const std::size_t close = head.rfind( ')' );
						if( open != std::string::npos && close != std::string::npos && close > open ) {
							name = trim( head.substr( 0, open ) );
							type = trim( head.substr( open + 1, close - open - 1 ) );
						}
						else name = head;
						description = trim( cleaned.substr( colon + 1 ) );
					}
					if( name.empty() == false && name.find_first_of( " \t" ) == std::string::npos &&
					    ( itemIndent == std::numeric_limits<std::size_t>::max() || indent <= itemIndent ) ) {
						itemIndent = indent;
						result.parameters.push_back( { name, type, collapseWhitespace( description ) } );
					}
					else if( result.parameters.empty() == false ) appendContinuation( result.parameters.back().description, cleaned );
					continue;
				}
				if( section == Section::Raises || section == Section::Returns ) {
					const std::size_t colon = cleaned.find( ':' );
					const bool header = colon != std::string::npos && trim( cleaned.substr( 0, colon ) ).empty() == false &&
					                    ( itemIndent == std::numeric_limits<std::size_t>::max() || indent <= itemIndent );
					if( header ) {
						itemIndent = indent;
						const std::string type = trim( cleaned.substr( 0, colon ) );
						const std::string description = collapseWhitespace( cleaned.substr( colon + 1 ) );
						if( section == Section::Raises ) result.raises.push_back( { type, description } );
						else result.returns.push_back( { type, description } );
					}
					else if( section == Section::Raises && result.raises.empty() == false ) {
						appendContinuation( result.raises.back().description, cleaned );
					}
					else if( section == Section::Returns ) {
						if( result.returns.empty() ) result.returns.push_back( { "", collapseWhitespace( cleaned ) } );
						else appendContinuation( result.returns.back().description, cleaned );
					}
					continue;
				}
				if( section == Section::Extends || section == Section::Implements || section == Section::Uses ) {
					std::string relation = cleaned;
					if( relation.rfind( "-", 0 ) == 0 ) relation = trim( relation.substr( 1 ) );
					for( const std::string& name : splitRelationList( relation ) ) {
						if( section == Section::Extends ) result.extendsRelations.push_back( name );
						else if( section == Section::Implements ) result.implementsRelations.push_back( name );
						else result.usesRelations.push_back( name );
					}
				}
			}
			result.descriptions = normalizeParagraphs( descriptionLines );
			std::size_t commonExampleIndent = std::numeric_limits<std::size_t>::max();
			for( const std::string& example : exampleLines ) {
				if( trim( example ).empty() ) continue;
				commonExampleIndent = std::min( commonExampleIndent, indentationWidth( example ) );
			}
			if( commonExampleIndent == std::numeric_limits<std::size_t>::max() ) commonExampleIndent = 0;
			for( const std::string& example : exampleLines ) {
				if( trim( example ).empty() ) {
					if( result.examples.empty() == false && result.examples.back().empty() == false ) result.examples.push_back( "" );
					continue;
				}
				result.examples.push_back( removeIndentation( example, commonExampleIndent ) );
			}
			while( result.examples.empty() == false && result.examples.back().empty() ) result.examples.pop_back();
			return result;
		}

		struct HoverMetadata {
			std::string kind;
			std::string signature;
			ParsedDocstring doc;
			std::vector<std::string> extendsRelations;
			std::vector<std::string> implementsRelations;
			std::vector<std::string> usesRelations;
		};

		void appendUnique( std::vector<std::string>& values, const std::string& value ) {
			if( value.empty() ) return;
			if( std::find( values.begin(), values.end(), value ) != values.end() ) return;
			values.push_back( value );
		}

		/** @brief Canonical Markdown renderer shared by every entity hover path. */
		std::string formatEntityHover( const HoverMetadata& metadata ) {
			std::string result = metadata.kind + ":\n```uranite\n" + metadata.signature + "\n```";
			auto section = [ & ]( const std::string& title, const std::vector<std::string>& lines ) {
				if( lines.empty() ) return;
				result += "\n\n###### " + title;
				for( const std::string& line : lines ) result += "\n" + line;
			};
			std::vector<std::string> complexity;
			if( metadata.doc.space.empty() == false ) {
				std::string line = "- **Space** `" + metadata.doc.space + "`";
				if( metadata.doc.spaceDescription.empty() == false ) line += " " + metadata.doc.spaceDescription;
				complexity.push_back( std::move( line ) );
			}
			if( metadata.doc.time.empty() == false ) {
				std::string line = "- **Time** `" + metadata.doc.time + "`";
				if( metadata.doc.timeDescription.empty() == false ) line += " " + metadata.doc.timeDescription;
				complexity.push_back( std::move( line ) );
			}
			section( "Complexity", complexity );
			std::vector<std::string> extendsLines;
			for( const std::string& name : metadata.extendsRelations ) extendsLines.push_back( "- `" + name + "`" );
			section( "Extends", extendsLines );
			std::vector<std::string> implementsLines;
			for( const std::string& name : metadata.implementsRelations ) implementsLines.push_back( "- `" + name + "`" );
			section( "Implements", implementsLines );
			std::vector<std::string> parameterLines;
			for( const DocParameter& parameter : metadata.doc.parameters ) {
				std::string line = "- `" + parameter.name + "`";
				if( parameter.type.empty() == false ) line += " (`" + parameter.type + "`)";
				if( parameter.description.empty() == false ) line += " " + parameter.description;
				parameterLines.push_back( std::move( line ) );
			}
			section( "Parameters", parameterLines );
			std::vector<std::string> raiseLines;
			for( const DocRaise& raise : metadata.doc.raises ) {
				std::string line = "- `" + raise.type + "`";
				if( raise.description.empty() == false ) line += "\n    " + raise.description;
				raiseLines.push_back( std::move( line ) );
			}
			section( "Raises", raiseLines );
			std::vector<std::string> returnLines;
			for( const DocReturn& entry : metadata.doc.returns ) {
				if( entry.type.empty() ) continue;
				std::string line = "- `" + entry.type + "`:";
				if( entry.description.empty() == false ) line += "\n\t" + entry.description;
				returnLines.push_back( std::move( line ) );
			}
			section( "Returns", returnLines );
			std::vector<std::string> usesLines;
			for( const std::string& name : metadata.usesRelations ) usesLines.push_back( "- `" + name + "`" );
			section( "Uses", usesLines );
			if( metadata.doc.descriptions.empty() == false ) result += "\n\n###### Descriptions\n" + metadata.doc.descriptions;
			if( metadata.doc.examples.empty() == false ) {
				result += "\n\n###### Examples\n```uranite\n";
				for( const std::string& example : metadata.doc.examples ) result += example + "\n";
				result += "```";
			}
			return result;
		}

		std::size_t lineByteOffset( const std::string& source, uint32_t line ) {
			std::size_t start = 0;
			uint32_t current = 0;
			while( current < line && start < source.size() ) {
				const std::size_t newline = source.find( '\n', start );
				if( newline == std::string::npos ) return source.size();
				start = newline + 1;
				current++;
			}
			return current == line ? start : source.size();
		}

		std::string lineTextAt( const std::string& source, uint32_t line ) {
			const std::size_t start = lineByteOffset( source, line );
			if( start >= source.size() ) return "";
			const std::size_t end = source.find( '\n', start );
			return source.substr( start, ( end == std::string::npos ? source.size() : end ) - start );
		}

		std::string packageNameFromSource( const std::string& source ) {
			std::istringstream stream( source );
			std::string line;
			while( std::getline( stream, line ) ) {
				const std::string cleaned = trim( line );
				if( cleaned.rfind( "package ", 0 ) == 0 ) return trim( cleaned.substr( 8 ) );
			}
			return "";
		}

		std::string extractLeadingHeaderDoc( const std::string& source ) {
			std::vector<std::string> lines;
			std::istringstream stream( source );
			std::string line;
			bool started = false;
			while( std::getline( stream, line ) ) {
				const std::string cleaned = trim( line );
				if( cleaned.empty() ) {
					if( started ) lines.push_back( "" );
					continue;
				}
				if( cleaned.rfind( "#", 0 ) != 0 ) break;
				started = true;
				std::string content = trim( cleaned.substr( 1 ) );
				const std::string lowered = lower( content );
				if( content.rfind( "@", 0 ) == 0 || content.find( "Copyright" ) != std::string::npos ||
				    content.find( "Licence" ) != std::string::npos || content.find( "License" ) != std::string::npos ||
				    lowered.find( "this program is free software" ) != std::string::npos ||
				    lowered.find( "you can redistribute it" ) != std::string::npos ||
				    lowered.find( "you should have received a copy" ) != std::string::npos ||
				    content.find( "gnu.org/licenses" ) != std::string::npos ) continue;
				lines.push_back( content );
			}
			return normalizeParagraphs( lines );
		}

		std::pair<uint32_t, std::size_t> findDeclarationLine( const std::string& source, const std::string& name,
		                                                      const std::string& kind ) {
			std::size_t start = 0;
			uint32_t lineNumber = 0;
			while( start <= source.size() ) {
				const std::size_t end = source.find( '\n', start );
				const std::size_t contentEnd = end == std::string::npos ? source.size() : end;
				const std::string line = source.substr( start, contentEnd - start );
				const std::string cleaned = trim( line );
				if( cleaned.rfind( "#", 0 ) != 0 && cleaned.rfind( "//", 0 ) != 0 ) {
					const std::size_t namePos = cleaned.find( name );
					if( namePos != std::string::npos ) {
						const std::string keyword = kind == "class" ? "class"
							: kind == "struct" ? "struct"
							: kind == "interface" ? "interface"
							: kind == "trait" ? "trait"
							: kind == "enum" ? "enum"
							: kind == "type" ? "type"
							: kind == "const" ? "const"
							: kind == "property" ? "property"
							: ( kind == "method" || kind == "function" ) ? "function" : "";
						if( keyword.empty() || cleaned.find( keyword ) != std::string::npos ) {
							return { lineNumber, start };
						}
					}
				}
				if( end == std::string::npos ) break;
				start = end + 1;
				lineNumber++;
			}
			return { 0, std::string::npos };
		}

		std::string removeLeadingModifiers( std::string line ) {
			line = trim( line );
			bool changed = true;
			while( changed ) {
				changed = false;
				for( const std::string& modifier : extractModifiers( line ) ) {
					if( line.rfind( modifier, 0 ) != 0 ) continue;
					const std::size_t end = modifier.size();
					if( end < line.size() && ( ( line[ end ] >= 'a' && line[ end ] <= 'z' ) ||
					                          ( line[ end ] >= 'A' && line[ end ] <= 'Z' ) || line[ end ] == '_' ) ) continue;
					line = trim( line.substr( end ) );
					changed = true;
					break;
				}
			}
			return line;
		}

		std::string stripTrailingColon( std::string value ) {
			value = trim( value );
			if( value.empty() == false && value.back() == ':' ) value.pop_back();
			return trim( value );
		}

		std::string cutTypeHierarchy( std::string value ) {
			value = stripTrailingColon( value );
			const std::vector<std::string> markers = { " extends ", " implements ", " uses ", " = " };
			std::size_t cut = std::string::npos;
			for( const std::string& marker : markers ) {
				const std::size_t found = value.find( marker );
				if( found != std::string::npos && ( cut == std::string::npos || found < cut ) ) cut = found;
			}
			if( cut != std::string::npos ) value = value.substr( 0, cut );
			return trim( value );
		}

		std::string replaceDeclarationName( std::string declaration, const std::string& localName,
		                                    const std::string& qualifiedName, const std::string& keyword ) {
			declaration = trim( declaration );
			std::size_t position = keyword.empty() ? 0 : declaration.find( keyword );
			if( position == std::string::npos ) return declaration;
			position += keyword.size();
			while( position < declaration.size() && std::isspace( static_cast<unsigned char>( declaration[ position ] ) ) ) position++;
			if( localName.empty() == false && declaration.compare( position, localName.size(), localName ) == 0 ) {
				declaration.replace( position, localName.size(), qualifiedName );
			}
			else declaration.insert( position, qualifiedName );
			return declaration;
		}

		std::string typeKindName( const uranite::semantic::TypeSharedPointer& type, const std::string& declarationLine ) {
			if( trim( declarationLine ).rfind( "type ", 0 ) != std::string::npos ||
			    trim( declarationLine ).find( " type " ) != std::string::npos ) return "type";
			if( type == nullptr ) return "type";
			switch( type->kind ) {
				case uranite::semantic::Type::Kind::Class: return "class";
				case uranite::semantic::Type::Kind::Struct: return "struct";
				case uranite::semantic::Type::Kind::Interface: return "interface";
				case uranite::semantic::Type::Kind::Trait: return "trait";
				case uranite::semantic::Type::Kind::Enum: return "enum";
				default: return "type";
			}
		}

		std::string entityKind( Symbol::Kind symbolKind, const uranite::semantic::TypeSharedPointer& type,
		                        const std::string& declarationLine, uint32_t column ) {
			const std::size_t first = declarationLine.find_first_not_of( " \t" );
			const bool indented = first == std::string::npos ? column > 0 : first > 0;
			switch( symbolKind ) {
				case Symbol::Kind::EnumVariant: return "enum variant";
				case Symbol::Kind::Field: return "property";
				case Symbol::Kind::Function:
					if( declarationLine.find( "property " ) != std::string::npos ) return "property";
					return indented ? "method" : "function";
				case Symbol::Kind::Module: return "module";
				case Symbol::Kind::Parameter: return "parameter";
				case Symbol::Kind::Type: return typeKindName( type, declarationLine );
				case Symbol::Kind::Variable: {
					const std::vector<std::string> modifiers = extractModifiers( declarationLine );
					return std::find( modifiers.begin(), modifiers.end(), "const" ) != modifiers.end() ? "const" : "variable";
				}
			}
			return "variable";
		}

		std::string kindFromCompletionKind( int kind ) {
			switch( kind ) {
				case 3: return "function";
				case 5: return "property";
				case 6: return "variable";
				case 7: return "class";
				case 8: return "interface";
				case 9: return "module";
				case 13: return "enum";
				case 20: return "enum variant";
				case 21: return "const";
				case 22: return "struct";
				default: return "type";
			}
		}

		std::unordered_map<std::string, std::string> sourceImportOrigins( const std::string& source ) {
			std::unordered_map<std::string, std::string> origins;
			std::istringstream stream( source );
			std::string line;
			std::string moduleName;
			bool collecting = false;
			auto consume = [ & ]( std::string text ) {
				text.erase( std::remove( text.begin(), text.end(), '{' ), text.end() );
				text.erase( std::remove( text.begin(), text.end(), '}' ), text.end() );
				std::size_t start = 0;
				while( start <= text.size() ) {
					const std::size_t comma = text.find( ',', start );
					const std::string item = trim( text.substr( start, comma == std::string::npos ? std::string::npos : comma - start ) );
					if( item.empty() == false && item.rfind( "#", 0 ) != 0 && item.rfind( "//", 0 ) != 0 ) {
						std::istringstream itemStream( item );
						std::string original;
						std::string alias;
						std::string as;
						itemStream >> original >> as >> alias;
						const std::string local = as == "as" && alias.empty() == false ? alias : original;
						if( local.empty() == false && moduleName.empty() == false ) origins[ local ] = moduleName + "." + original;
					}
					if( comma == std::string::npos ) break;
					start = comma + 1;
				}
			};
			while( std::getline( stream, line ) ) {
				const std::string cleaned = trim( line );
				if( cleaned.rfind( "from ", 0 ) == 0 ) {
					const std::size_t importPos = cleaned.find( " import" );
					if( importPos == std::string::npos ) continue;
					moduleName = trim( cleaned.substr( 5, importPos - 5 ) );
					std::string rest = trim( cleaned.substr( importPos + 7 ) );
					collecting = rest.find( '{' ) != std::string::npos && rest.find( '}' ) == std::string::npos;
					consume( rest );
					if( collecting == false ) moduleName.clear();
					continue;
				}
				if( collecting ) {
					consume( cleaned );
					if( cleaned.find( '}' ) != std::string::npos ) { collecting = false; moduleName.clear(); }
				}
			}
			return origins;
		}

		std::vector<std::string> splitRelationList( const std::string& value ) {
			std::vector<std::string> result;
			std::string current;
			for( char ch : value ) {
				if( ch == ',' ) {
					if( trim( current ).empty() == false ) result.push_back( trim( current ) );
					current.clear();
				}
				else current.push_back( ch );
			}
			if( trim( current ).empty() == false ) result.push_back( trim( current ) );
			return result;
		}

		void hierarchyFromDeclaration( const std::string& declarationLine, std::vector<std::string>& extendsRelations,
		                               std::vector<std::string>& implementsRelations, std::vector<std::string>& usesRelations ) {
			const std::vector<std::pair<std::string, std::vector<std::string>*>> markers = {
				{ " extends ", &extendsRelations }, { " implements ", &implementsRelations }, { " uses ", &usesRelations }
			};
			for( const auto& marker : markers ) {
				const std::size_t start = declarationLine.find( marker.first );
				if( start == std::string::npos ) continue;
				std::size_t end = declarationLine.size();
				for( const auto& other : markers ) {
					if( other.first == marker.first ) continue;
					const std::size_t found = declarationLine.find( other.first, start + marker.first.size() );
					if( found != std::string::npos && found < end ) end = found;
				}
				if( end > 0 && declarationLine[ end - 1 ] == ':' ) end--;
				for( const std::string& name : splitRelationList( declarationLine.substr( start + marker.first.size(), end - start - marker.first.size() ) ) ) {
					appendUnique( *marker.second, name );
				}
			}
		}

		std::string joinModifiers( const std::vector<std::string>& modifiers ) {
			std::string result;
			for( const std::string& modifier : modifiers ) {
				if( result.empty() == false ) result += " ";
				result += modifier;
			}
			return result;
		}

		std::string genericSuffix( const uranite::semantic::TypeSharedPointer& type ) {
			if( type == nullptr ) return "";
			if( auto classType = std::dynamic_pointer_cast<uranite::semantic::ClassType>( type ) ) {
				if( classType->astDeclaration != nullptr && classType->astDeclaration->genericParameters.empty() == false ) {
					std::string result = "<";
					for( std::size_t i = 0; i < classType->astDeclaration->genericParameters.size(); i++ ) {
						if( i > 0 ) result += ", ";
						result += classType->astDeclaration->genericParameters[ i ]->name;
					}
					return result + ">";
				}
			}
			const std::string text = type->toString();
			const std::size_t open = text.find( '<' );
			return open == std::string::npos ? "" : text.substr( open );
		}

		std::string fallbackFunctionSignature( const std::string& keyword, const std::string& qualifiedName,
		                                       const uranite::semantic::TypeSharedPointer& type ) {
			auto function = std::dynamic_pointer_cast<uranite::semantic::FunctionType>( type );
			if( function == nullptr ) return keyword + " " + qualifiedName + "()";
			std::string result = keyword + " " + qualifiedName;
			if( function->genericParameterNames.empty() == false ) {
				result += "<";
				for( std::size_t i = 0; i < function->genericParameterNames.size(); i++ ) {
					if( i > 0 ) result += ", ";
					result += function->genericParameterNames[ i ];
				}
				result += ">";
			}
			result += "( ";
			for( std::size_t i = 0; i < function->parameterTypes.size(); i++ ) {
				if( i > 0 ) result += ", ";
				const std::string parameterType = function->parameterTypes[ i ] == nullptr ? "Any" : function->parameterTypes[ i ]->toString();
				if( i < function->parameterNames.size() && function->parameterNames[ i ].empty() == false ) {
					result += parameterType + " " + function->parameterNames[ i ];
				}
				else result += parameterType;
			}
			result += " )";
			if( function->returnType != nullptr ) result += " -> " + function->returnType->toString();
			return result;
		}

	} // namespace

	QueryService::QueryService( std::shared_ptr<FrontendResult> result, std::shared_ptr<SymbolIndex> index,
	                            std::shared_ptr<OverlayStore> overlayStore, const std::string& modulesPath,
	                            const std::string& uri, const std::string& text,
	                            std::shared_ptr<ModuleGraph> moduleGraph )
		: result( std::move( result ) ), index( std::move( index ) ), overlayStore( std::move( overlayStore ) ),
		  modulesPath( modulesPath ), uri( uri ), text( text ), moduleGraph( std::move( moduleGraph ) ) {
	}

	QueryLocation QueryService::toLocation( const IndexedSymbol& symbol ) {
		QueryLocation location;
		location.path = symbol.pathname;
		location.line = symbol.line;
		location.col = symbol.col;
		location.endLine = symbol.endLine;
		location.endCol = symbol.endCol;
		location.valid = true;
		return location;
	}

	const IndexedSymbol* QueryService::definitionFor( const IndexedSymbol* symbol ) const {
		if( symbol == nullptr ) return nullptr;
		if( symbol->isDefinition ) return symbol;
		std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( symbol->qualifiedName );
		if( definitions.empty() ) definitions = this->index->definitionsFor( symbol->name );
		if( definitions.empty() == false ) {
			for( const IndexedSymbol* candidate : definitions ) {
				if( candidate->symbol == symbol->symbol ) return candidate;
			}
			if( symbol->container.empty() == false ) {
				for( const IndexedSymbol* candidate : definitions ) {
					if( candidate->container == symbol->container ) return candidate;
				}
			}
			return definitions.front();
		}
		for( const IndexedSymbol* candidate : this->index->allDefinitions() ) {
			if( candidate->name == symbol->name ) return candidate;
		}
		return nullptr;
	}

	const IndexedSymbol* QueryService::importDefinition( const std::string& modulePath, const std::string& name ) const {
		if( this->index == nullptr ) return nullptr;
		int preferredKind = 0;
		if( this->moduleGraph != nullptr ) {
			for( const ModuleGraph::ExportInfo& info : this->moduleGraph->exportedSymbolInfos( modulePath ) ) {
				if( info.name == name ) { preferredKind = info.kind; break; }
			}
		}
		const bool wantsType = preferredKind == 7 || preferredKind == 8 || preferredKind == 13 || preferredKind == 22;
		const bool wantsCallable = preferredKind == 3 || preferredKind == 5;
		auto acceptable = [ & ]( const IndexedSymbol* candidate ) {
			if( candidate == nullptr || candidate->isDefinition == false || candidate->name != name ) return false;
			if( wantsType && candidate->kind != Symbol::Kind::Type ) return false;
			if( wantsCallable && candidate->kind != Symbol::Kind::Function ) return false;
			if( wantsCallable ) {
				const std::string source = fileSourceFor( this->overlayStore.get(), candidate->pathname );
				if( source.empty() == false ) {
					const std::string line = lineTextAt( source, candidate->line );
					const std::size_t first = line.find_first_not_of( " \t" );
					if( first != std::string::npos && first > 0 ) return false;
				}
			}
			return true;
		};
		const std::vector<std::string> keys = { modulePath + "." + name, name };
		for( const std::string& key : keys ) {
			const std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( key );
			for( const IndexedSymbol* candidate : definitions ) {
				if( acceptable( candidate ) ) return candidate;
			}
		}
		std::string moduleFile;
		if( this->moduleGraph != nullptr ) {
			std::vector<std::string> segments;
			std::string current;
			for( char ch : modulePath ) {
				if( ch == '.' ) {
					if( current.empty() == false ) { segments.push_back( current ); current.clear(); }
				}
				else current.push_back( ch );
			}
			if( current.empty() == false ) segments.push_back( current );
			moduleFile = this->moduleGraph->findModule( segments );
		}
		std::string moduleDirectory;
		if( moduleFile.empty() == false ) {
			const std::size_t slash = moduleFile.find_last_of( "/\\" );
			if( slash != std::string::npos ) moduleDirectory = moduleFile.substr( 0, slash + 1 );
		}
		for( const IndexedSymbol* candidate : this->index->allDefinitions() ) {
			if( acceptable( candidate ) == false ) continue;
			if( moduleDirectory.empty() == false && candidate->pathname.rfind( moduleDirectory, 0 ) != 0 ) continue;
			return candidate;
		}
		return nullptr;
	}

	std::pair<std::string, std::string> QueryService::importOriginFor( const std::string& localName ) const {
		if( this->result == nullptr || this->result->program == nullptr ) return {};
		for( const ast::nodes::ImportDeclarationSharedPointer& importDecl : this->result->program->imports ) {
			if( importDecl == nullptr || importDecl->isFromImport == false ) continue;
			for( const ast::nodes::ImportItem& importItem : importDecl->importItems ) {
				const std::string importedLocalName = importItem.alias.empty() ? importItem.name : importItem.alias;
				if( importedLocalName != localName ) continue;
				std::string moduleName;
				for( std::size_t i = 0; i < importDecl->path.size(); i++ ) {
					if( i > 0 ) moduleName += ".";
					moduleName += importDecl->path[ i ];
				}
				return { moduleName, importItem.name };
			}
		}
		return {};
	}

	std::string QueryService::buildEntityHover( const IndexedSymbol* symbol, const IndexedSymbol* definition,
	                                            const std::string& qualifiedName, const std::string& modulePath,
	                                            const std::string& localName,
	                                            const ModuleGraph::ExportInfo* exportInfo ) const {
		if( symbol == nullptr && exportInfo == nullptr ) return "";
		const IndexedSymbol* anchor = definition != nullptr ? definition : ( symbol != nullptr && symbol->isDefinition ? symbol : nullptr );
		uranite::semantic::TypeSharedPointer type = definition != nullptr ? definition->type : ( symbol != nullptr ? symbol->type : nullptr );
		if( type == nullptr && this->result != nullptr ) {
			const auto found = this->result->moduleTypes.find( localName );
			if( found != this->result->moduleTypes.end() ) type = found->second;
		}

		std::string sourcePath = anchor == nullptr ? "" : anchor->pathname;
		if( sourcePath.empty() && this->moduleGraph != nullptr && modulePath.empty() == false ) {
			std::vector<std::string> segments;
			std::string current;
			for( char ch : modulePath ) {
				if( ch == '.' ) {
					if( current.empty() == false ) { segments.push_back( current ); current.clear(); }
				}
				else current.push_back( ch );
			}
			if( current.empty() == false ) segments.push_back( current );
			sourcePath = this->moduleGraph->findModule( segments );
		}
		const std::string source = sourcePath.empty() ? "" : fileSourceFor( this->overlayStore.get(), sourcePath );
		std::size_t lineStart = std::string::npos;
		if( anchor != nullptr && source.empty() == false ) lineStart = lineByteOffset( source, anchor->line );
		std::string declarationLine = lineStart == std::string::npos ? "" : lineTextAt( source, anchor->line );
		if( declarationLine.empty() && exportInfo != nullptr ) declarationLine = exportInfo->detail;

		const Symbol::Kind symbolKind = symbol != nullptr ? symbol->kind : Symbol::Kind::Type;
		std::string kind = symbol != nullptr
			? entityKind( symbolKind, type, declarationLine, anchor == nullptr ? 0 : anchor->col )
			: ( exportInfo == nullptr ? "type" : kindFromCompletionKind( exportInfo->kind ) );
		if( declarationLine.empty() && source.empty() == false && localName.empty() == false ) {
			const auto declaration = findDeclarationLine( source, localName, kind );
			if( declaration.second != std::string::npos ) {
				lineStart = declaration.second;
				declarationLine = lineTextAt( source, declaration.first );
			}
		}

		const std::string actualPackage = packageNameFromSource( source );
		const std::string publicModule = modulePath.empty() ? actualPackage : modulePath;
		const std::unordered_map<std::string, std::string> sourceImports = sourceImportOrigins( source );
		auto qualifyBare = [ & ]( const std::string& rawName ) {
			const std::string cleaned = trim( rawName );
			if( cleaned.empty() || cleaned.find( '.' ) != std::string::npos ) return cleaned;
			const auto sourceImport = sourceImports.find( cleaned );
			if( sourceImport != sourceImports.end() ) return sourceImport->second;
			const auto userImport = this->importOriginFor( cleaned );
			if( userImport.first.empty() == false ) return userImport.first + "." + userImport.second;
			if( cleaned == localName && publicModule.empty() == false ) return publicModule + "." + cleaned;
			if( this->result != nullptr ) {
				const auto typeFound = this->result->moduleTypes.find( cleaned );
				if( typeFound != this->result->moduleTypes.end() && typeFound->second != nullptr ) {
					const std::string qualified = typeFound->second->qualified;
					if( qualified.empty() == false && qualified != cleaned ) return qualified;
				}
			}
			return publicModule.empty() ? cleaned : publicModule + "." + cleaned;
		};
		auto qualifyTypeText = [ & ]( const std::string& rawText, const uranite::semantic::TypeSharedPointer& baseType ) {
			std::string text = trim( rawText );
			if( text.empty() || baseType == nullptr ) {
				const std::size_t open = text.find( '<' );
				const std::string base = trim( text.substr( 0, open ) );
				if( base.empty() || base.find_first_of( "[]?" ) != std::string::npos ) return text;
				const std::string qualified = qualifyBare( base );
				return qualified + ( open == std::string::npos ? "" : text.substr( open ) );
			}
			const std::string baseName = baseType->name;
			if( text.rfind( baseName, 0 ) != 0 ) return text;
			std::string replacement = baseName;
			const auto sourceImport = sourceImports.find( baseName );
			const auto userImport = this->importOriginFor( baseName );
			if( sourceImport != sourceImports.end() ) replacement = sourceImport->second;
			else if( userImport.first.empty() == false ) replacement = userImport.first + "." + userImport.second;
			else if( baseName == localName && publicModule.empty() == false ) replacement = publicModule + "." + baseName;
			else if( baseType->qualified.empty() == false && baseType->qualified != baseName &&
			         ( baseType->kind == uranite::semantic::Type::Kind::Class ||
			           baseType->kind == uranite::semantic::Type::Kind::Struct ||
			           baseType->kind == uranite::semantic::Type::Kind::Interface ||
			           baseType->kind == uranite::semantic::Type::Kind::Trait ||
			           baseType->kind == uranite::semantic::Type::Kind::Enum ) ) replacement = baseType->qualified;
			return replacement + text.substr( baseName.size() );
		};
		auto qualifiedTypeName = [ & ]( const uranite::semantic::TypeSharedPointer& relationType ) {
			if( relationType == nullptr ) return std::string();
			if( relationType->qualified.empty() == false && relationType->qualified != relationType->name ) return relationType->qualified;
			if( relationType->package.empty() == false ) return relationType->package + "." + relationType->name;
			return qualifyBare( relationType->name );
		};

		const std::vector<std::string> modifiers = extractModifiers( declarationLine );
		const std::string modifierPrefix = modifiers.empty() ? "" : joinModifiers( modifiers ) + " ";
		const std::string withoutModifiers = removeLeadingModifiers( declarationLine );
		std::string displayQualifiedName = qualifiedName;
		const bool typeAlias = kind == "type" && ( withoutModifiers.rfind( "type ", 0 ) == 0 ||
		                                          declarationLine.find( " type " ) != std::string::npos );
		if( typeAlias && publicModule.empty() == false ) displayQualifiedName = publicModule + "." + localName;
		if( ( kind == "method" || kind == "property" ) && definition != nullptr && definition->container.empty() == false &&
		    displayQualifiedName.find( definition->container + "." ) == std::string::npos ) {
			const std::string ownerPackage = actualPackage.empty() ? publicModule : actualPackage;
			if( ownerPackage.empty() == false ) displayQualifiedName = ownerPackage + "." + definition->container + "." + localName;
		}

		const bool callable = kind == "function" || kind == "method" || ( kind == "property" && withoutModifiers.rfind( "property ", 0 ) == 0 );
		std::string signature;
		if( kind == "module" ) signature = displayQualifiedName;
		else if( callable && declarationLine.empty() == false ) {
			const std::string keyword = kind == "property" ? "property" : "function";
			std::string replaced = replaceDeclarationName( declarationLine, localName, displayQualifiedName, keyword );
			std::size_t keywordPos = replaced.find( keyword );
			if( keywordPos == std::string::npos && keyword == "property" ) {
				replaced = replaceDeclarationName( declarationLine, localName, displayQualifiedName, "function" );
				keywordPos = replaced.find( "function" );
			}
			if( keywordPos != std::string::npos ) signature = modifierPrefix + stripTrailingColon( replaced.substr( keywordPos ) );
		}
		else if( kind == "class" || kind == "struct" || kind == "interface" || kind == "trait" || kind == "enum" || kind == "type" ) {
			std::string declaration = withoutModifiers;
			for( const char* keyword : { "class", "struct", "interface", "trait", "enum", "type" } ) {
				if( declaration.rfind( std::string( keyword ) + " ", 0 ) == 0 ) {
					declaration = trim( declaration.substr( std::strlen( keyword ) + 1 ) );
					break;
				}
			}
			declaration = replaceDeclarationName( cutTypeHierarchy( declaration ), localName, displayQualifiedName, "" );
			signature = modifierPrefix + declaration;
		}
		else if( kind == "const" ) {
			std::string typeText = type == nullptr ? "" : qualifyTypeText( type->toString(), type );
			if( declarationLine.empty() == false ) {
				const std::size_t namePos = withoutModifiers.find( localName );
				if( namePos != std::string::npos ) typeText = trim( withoutModifiers.substr( 0, namePos ) );
			}
			signature = modifierPrefix + "const " + displayQualifiedName + ": " + typeText;
		}
		else if( declarationLine.empty() == false && !withoutModifiers.empty() ) {
			std::string typeText = type == nullptr ? "" : qualifyTypeText( type->toString(), type );
			const std::size_t namePos = withoutModifiers.find( localName );
			if( namePos != std::string::npos ) typeText = trim( withoutModifiers.substr( 0, namePos ) );
			if( definition != nullptr && definition->col == 0 && displayQualifiedName.find( '.' ) != std::string::npos ) {
				signature = modifierPrefix + displayQualifiedName + ": " + typeText;
			}
			else signature = modifierPrefix + typeText + " " + localName;
		}
		else if( callable ) signature = modifierPrefix + fallbackFunctionSignature( kind == "property" ? "property" : "function", displayQualifiedName, type );
		else if( kind == "class" || kind == "struct" || kind == "interface" || kind == "trait" || kind == "enum" || kind == "type" ) {
			signature = modifierPrefix + displayQualifiedName + genericSuffix( type );
		}
		else signature = modifierPrefix + ( type == nullptr ? localName : qualifyTypeText( type->toString(), type ) + " " + localName );
		if( signature.empty() ) signature = displayQualifiedName.empty() ? localName : displayQualifiedName;

		HoverMetadata metadata;
		metadata.kind = kind;
		metadata.signature = std::move( signature );
		std::string docstring = lineStart == std::string::npos || source.empty() ? "" : extractDocstringAfter( source, lineStart );
		if( docstring.empty() && exportInfo != nullptr ) docstring = exportInfo->documentation;
		metadata.doc = parseDocstring( docstring );

		if( typeAlias == false ) {
				auto addTypeRelation = [ & ]( std::vector<std::string>& target, const uranite::semantic::TypeSharedPointer& relationType ) {
					appendUnique( target, qualifiedTypeName( relationType ) );
				};
			auto addRelation = [ & ]( std::vector<std::string>& target, const std::string& rawName ) {
				const std::string qualified = qualifyBare( rawName );
				const std::string base = trim( qualified.substr( 0, qualified.find( '<' ) ) );
				for( const std::string& existing : target ) {
					if( trim( existing.substr( 0, existing.find( '<' ) ) ) == base ) return;
				}
				appendUnique( target, qualified );
			};
			if( auto classType = std::dynamic_pointer_cast<uranite::semantic::ClassType>( type ) ) {
				addTypeRelation( metadata.extendsRelations, classType->baseClass );
				for( const auto& interfaceType : classType->interfaces ) addTypeRelation( metadata.implementsRelations, interfaceType );
				if( classType->astDeclaration != nullptr ) {
					for( const std::string& trait : classType->astDeclaration->usedTraits ) addRelation( metadata.usesRelations, trait );
				}
			}
			else if( auto enumType = std::dynamic_pointer_cast<uranite::semantic::EnumType>( type ) ) {
				addTypeRelation( metadata.extendsRelations, enumType->baseClass );
				for( const auto& interfaceType : enumType->interfaces ) addTypeRelation( metadata.implementsRelations, interfaceType );
			}
			else if( auto interfaceType = std::dynamic_pointer_cast<uranite::semantic::InterfaceType>( type ) ) {
				for( const auto& parent : interfaceType->parentInterfaces ) addTypeRelation( metadata.extendsRelations, parent );
			}
			else if( auto structType = std::dynamic_pointer_cast<uranite::semantic::StructType>( type ) ) {
				if( structType->astDeclaration != nullptr ) {
					for( const std::string& trait : structType->astDeclaration->usedTraits ) addRelation( metadata.usesRelations, trait );
				}
			}
			std::vector<std::string> declarationExtends;
			std::vector<std::string> declarationImplements;
			std::vector<std::string> declarationUses;
			hierarchyFromDeclaration( declarationLine, declarationExtends, declarationImplements, declarationUses );
			for( const std::string& relation : declarationExtends ) addRelation( metadata.extendsRelations, relation );
			for( const std::string& relation : declarationImplements ) addRelation( metadata.implementsRelations, relation );
			for( const std::string& relation : declarationUses ) addRelation( metadata.usesRelations, relation );
			for( const std::string& relation : metadata.doc.extendsRelations ) addRelation( metadata.extendsRelations, relation );
			for( const std::string& relation : metadata.doc.implementsRelations ) addRelation( metadata.implementsRelations, relation );
			for( const std::string& relation : metadata.doc.usesRelations ) addRelation( metadata.usesRelations, relation );
		}

		auto functionType = std::dynamic_pointer_cast<uranite::semantic::FunctionType>( type );
		if( functionType != nullptr ) {
			for( DocParameter& parameter : metadata.doc.parameters ) {
				if( parameter.type.empty() == false ) continue;
				for( std::size_t i = 0; i < functionType->parameterNames.size(); i++ ) {
					if( functionType->parameterNames[ i ] != parameter.name ) continue;
					if( i < functionType->parameterTypes.size() && functionType->parameterTypes[ i ] != nullptr ) {
						parameter.type = functionType->parameterTypes[ i ]->toString();
					}
					break;
				}
			}
		}

		auto relationBase = []( const std::string& value ) {
			return trim( value.substr( 0, value.find( '<' ) ) );
		};
		auto hasRaise = [ & ]( const std::string& typeName ) {
			const std::string base = relationBase( typeName );
			for( const DocRaise& raise : metadata.doc.raises ) {
				if( relationBase( raise.type ) == base ) return true;
			}
			return false;
		};
		for( DocRaise& raise : metadata.doc.raises ) raise.type = qualifyBare( raise.type );
		if( functionType != nullptr ) {
			for( const auto& exceptionType : functionType->exceptionTypes ) {
				const std::string qualified = qualifiedTypeName( exceptionType );
				if( hasRaise( qualified ) == false ) metadata.doc.raises.push_back( { qualified, "" } );
			}
		}
		const std::size_t raisesPos = declarationLine.find( " raises " );
		if( raisesPos != std::string::npos ) {
			std::size_t end = declarationLine.find( " ->", raisesPos + 8 );
			if( end == std::string::npos ) end = declarationLine.size();
			if( end > 0 && declarationLine[ end - 1 ] == ':' ) end--;
			for( const std::string& raised : splitRelationList( declarationLine.substr( raisesPos + 8, end - raisesPos - 8 ) ) ) {
				const std::string qualified = qualifyBare( raised );
				if( hasRaise( qualified ) == false ) metadata.doc.raises.push_back( { qualified, "" } );
			}
		}

		for( DocReturn& entry : metadata.doc.returns ) {
			if( entry.type.empty() ) {
				if( functionType != nullptr && functionType->returnType != nullptr ) {
					entry.type = qualifyTypeText( functionType->returnType->toString(), functionType->returnType );
				}
			}
			else {
				const std::size_t open = entry.type.find( '<' );
				const std::string base = trim( entry.type.substr( 0, open ) );
				entry.type = qualifyBare( base ) + ( open == std::string::npos ? "" : entry.type.substr( open ) );
			}
		}
		if( functionType != nullptr && functionType->returnType != nullptr &&
		    ( functionType->returnType->isVoid() == false || metadata.doc.returns.empty() == false ) ) {
			const std::string qualified = qualifyTypeText( functionType->returnType->toString(), functionType->returnType );
			const std::string base = relationBase( qualified );
			bool present = false;
			for( const DocReturn& entry : metadata.doc.returns ) {
				if( relationBase( entry.type ) == base ) { present = true; break; }
			}
			if( present == false ) metadata.doc.returns.push_back( { qualified, "" } );
		}
		metadata.doc.returns.erase( std::remove_if( metadata.doc.returns.begin(), metadata.doc.returns.end(),
			[]( const DocReturn& entry ) { return entry.type.empty(); } ), metadata.doc.returns.end() );
		return formatEntityHover( metadata );
	}

	QueryHover QueryService::hover( uint32_t line, uint32_t col ) const {
		QueryHover hover;
		if( this->index == nullptr ) return hover;
		ImportTarget target = importTargetAt( this->text, line, col );
		if( target.kind != ImportTarget::None ) {
			if( target.kind == ImportTarget::ModuleSegment ) {
				std::string filePath;
				if( this->moduleGraph != nullptr ) {
					std::vector<std::string> segments;
					std::string current;
					for( char ch : target.modulePath ) {
						if( ch == '.' ) {
							if( current.empty() == false ) { segments.push_back( current ); current.clear(); }
						}
						else current.push_back( ch );
					}
					if( current.empty() == false ) segments.push_back( current );
					filePath = this->moduleGraph->findModule( segments );
				}
				HoverMetadata metadata;
				metadata.kind = "module";
				metadata.signature = target.modulePath;
				if( filePath.empty() == false ) {
					const std::string source = fileSourceFor( this->overlayStore.get(), filePath );
					std::size_t packageOffset = std::string::npos;
					std::size_t scan = 0;
					std::string sourceLine;
					while( scan <= source.size() ) {
						const std::size_t end = source.find( '\n', scan );
						const std::string current = trim( source.substr( scan, ( end == std::string::npos ? source.size() : end ) - scan ) );
						if( current.rfind( "package ", 0 ) == 0 ) { packageOffset = scan; break; }
						if( end == std::string::npos ) break;
						scan = end + 1;
					}
					if( packageOffset != std::string::npos ) metadata.doc = parseDocstring( extractDocstringAfter( source, packageOffset ) );
					if( metadata.doc.descriptions.empty() ) metadata.doc = parseDocstring( extractLeadingHeaderDoc( source ) );
				}
				hover.contents = formatEntityHover( metadata );
				hover.valid = true;
				return hover;
			}
			std::optional<ModuleGraph::ExportInfo> exportInfoValue;
			if( this->moduleGraph != nullptr ) {
				const auto infos = this->moduleGraph->exportedSymbolInfos( target.modulePath );
				for( const auto& info : infos ) {
					if( info.name == target.symbolName ) { exportInfoValue = info; break; }
				}
			}
			const ModuleGraph::ExportInfo* exportInfo = exportInfoValue.has_value() ? &*exportInfoValue : nullptr;
			const IndexedSymbol* definition = importDefinition( target.modulePath, target.symbolName );
			if( definition == nullptr && exportInfo == nullptr && this->moduleGraph != nullptr ) {
				std::vector<std::string> segments;
				std::string current;
				for( char ch : target.modulePath ) {
					if( ch == '.' ) {
						if( current.empty() == false ) { segments.push_back( current ); current.clear(); }
					}
					else current.push_back( ch );
				}
				if( current.empty() == false ) segments.push_back( current );
				segments.push_back( target.symbolName );
				if( this->moduleGraph->findModule( segments ).empty() == false ) {
					HoverMetadata metadata;
					metadata.kind = "module";
					metadata.signature = target.modulePath + "." + target.symbolName;
					hover.contents = formatEntityHover( metadata );
					hover.valid = true;
					return hover;
				}
			}
			hover.contents = buildEntityHover( definition, definition, target.modulePath + "." + target.symbolName,
				target.modulePath, target.symbolName, exportInfo );
			hover.valid = hover.contents.empty() == false;
			return hover;
		}

		const IndexedSymbol* symbol = this->index->symbolAt( line, col );
		if( symbol == nullptr ) return hover;
		const IndexedSymbol* definition = definitionFor( symbol );
		const auto origin = importOriginFor( symbol->name );
		const std::string qualifiedName = origin.first.empty() ? symbol->qualifiedName : origin.first + "." + origin.second;
		hover.contents = buildEntityHover( symbol, definition, qualifiedName, origin.first, symbol->name, nullptr );
		hover.valid = hover.contents.empty() == false;
		return hover;
	}

	std::string QueryService::entityHoverDocumentation( const std::string& modulePath, const std::string& symbolName ) const {
		if( modulePath.empty() || symbolName.empty() ) return "";
		std::optional<ModuleGraph::ExportInfo> exportInfoValue;
		if( this->moduleGraph != nullptr ) {
			for( const ModuleGraph::ExportInfo& info : this->moduleGraph->exportedSymbolInfos( modulePath ) ) {
				if( info.name == symbolName ) { exportInfoValue = info; break; }
			}
		}
		const ModuleGraph::ExportInfo* exportInfo = exportInfoValue.has_value() ? &*exportInfoValue : nullptr;
		const IndexedSymbol* definition = importDefinition( modulePath, symbolName );
		return buildEntityHover( definition, definition, modulePath + "." + symbolName,
			modulePath, symbolName, exportInfo );
	}

	QueryLocation QueryService::definition( uint32_t line, uint32_t col ) const {
		QueryLocation location;
		if( this->index == nullptr ) return location;
		// Imports: packages, submodules and imported symbols.
		ImportTarget target = importTargetAt( this->text, line, col );
		if( target.kind != ImportTarget::None ) {
			if( target.kind == ImportTarget::ModuleSegment ) {
				std::vector<std::string> segs;
				std::string cur;
				for( char ch : target.modulePath ) {
					if( ch == '.' ) { if( cur.empty() == false ) { segs.push_back( cur ); cur.clear(); } }
					else cur.push_back( ch );
				}
				if( cur.empty() == false ) segs.push_back( cur );
				std::string filePath = this->moduleGraph == nullptr ? "" : this->moduleGraph->findModule( segs );
				if( filePath.empty() == false ) {
					location.path = filePath;
					location.line = 0;
					location.col = 0;
					location.endLine = 0;
					location.endCol = 0;
					location.valid = true;
				}
				return location;
			}
			if( this->moduleGraph != nullptr ) {
				auto infos = this->moduleGraph->exportedSymbolInfos( target.modulePath );
				for( const auto& info : infos ) {
					if( info.name != target.symbolName ) continue;
					std::string qualified = target.modulePath + "." + info.name;
					std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( qualified );
					if( definitions.empty() ) {
						definitions = this->index->definitionsFor( info.name );
					}
					if( definitions.empty() ) {
						for( const IndexedSymbol* candidate : this->index->allDefinitions() ) {
							if( candidate == nullptr ) continue;
							if( candidate->name != info.name ) continue;
							if( candidate->isDefinition == false ) continue;
							if( candidate->pathname.empty() ) continue;
							// Prefer a definition in the resolved module file.
							if( this->moduleGraph != nullptr ) {
								std::vector<std::string> segs;
								std::string cur;
								for( char ch : target.modulePath ) {
									if( ch == '.' ) { if( cur.empty() == false ) { segs.push_back( cur ); cur.clear(); } }
									else cur.push_back( ch );
								}
								if( cur.empty() == false ) segs.push_back( cur );
								const std::string modFile = this->moduleGraph->findModule( segs );
								if( modFile.empty() == false ) {
									if( candidate->pathname != modFile ) {
										std::string modDir = modFile;
										const std::size_t slash = modDir.find_last_of( "/\\" );
										if( slash != std::string::npos ) modDir = modDir.substr( 0, slash + 1 );
										if( candidate->pathname.rfind( modDir, 0 ) != 0 ) continue;
									}
								}
							}
							definitions.push_back( candidate );
							break;
						}
					}
					if( definitions.empty() == false ) {
						return toLocation( *definitions.front() );
					}
					// Fall back to the module file's package declaration line.
					std::string filePath = this->moduleGraph->findModule( [ &target ]() {
						std::vector<std::string> segs;
						std::string cur;
						for( char ch : target.modulePath ) {
							if( ch == '.' ) { if( cur.empty() == false ) { segs.push_back( cur ); cur.clear(); } }
							else cur.push_back( ch );
						}
						if( cur.empty() == false ) segs.push_back( cur );
						return segs;
					}() );
					if( filePath.empty() == false ) {
						location.path = filePath;
						location.line = 0;
						location.col = 0;
						location.endLine = 0;
						location.endCol = 0;
						location.valid = true;
					}
					return location;
				}
			}
			return location;
		}

		const IndexedSymbol* usage = this->index->symbolAt( line, col );
		if( usage == nullptr ) return location;

		const std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( usage->qualifiedName );
		if( definitions.empty() ) return location;
		for( const IndexedSymbol* definition : definitions ) {
			if( definition->symbol == usage->symbol ) return toLocation( *definition );
		}
		// Prefer the declaration inside the receiver's own class so overridden
		// methods jump to the subclass declaration rather than the base.
		if( usage->container.empty() == false ) {
			for( const IndexedSymbol* definition : definitions ) {
				if( definition->container == usage->container ) return toLocation( *definition );
			}
		}
		return toLocation( *definitions.front() );
	}

	std::vector<QueryLocation> QueryService::locationsFor( uint32_t line, uint32_t col, bool includeDeclaration ) const {
		std::vector<QueryLocation> locations;
		if( this->index == nullptr ) return locations;
		const IndexedSymbol* usage = this->index->symbolAt( line, col );
		if( usage == nullptr ) return locations;

		std::set<std::string> seen;
		for( const IndexedSymbol* reference : this->index->referencesFor( usage->qualifiedName ) ) {
			const QueryLocation location = toLocation( *reference );
			const std::string key = fmt::format( "{}:{}:{}", location.path, location.line, location.col );
			if( seen.insert( key ).second ) locations.push_back( location );
		}
		if( includeDeclaration ) {
			for( const IndexedSymbol* definition : this->index->definitionsFor( usage->qualifiedName ) ) {
				const QueryLocation location = toLocation( *definition );
				const std::string key = fmt::format( "{}:{}:{}", location.path, location.line, location.col );
				if( seen.insert( key ).second ) locations.push_back( location );
			}
		}
		return locations;
	}

	std::vector<QueryLocation> QueryService::references( uint32_t line, uint32_t col, bool includeDeclaration ) const {
		return this->locationsFor( line, col, includeDeclaration );
	}

	std::vector<QueryCompletion> QueryService::completion( uint32_t line, uint32_t col ) const {
		std::vector<QueryCompletion> output;
		if( this->overlayStore == nullptr ) {
			return output;
		}
		cst::ParseSession session;
		session.parse( this->text );
		cst::CompletionContext context = cst::SyntacticIndex::completionContext( this->text, session.rootNode(), line, col );
		std::function<void( const std::string&, int, const std::string&, const std::string&, int )> addCompletion = [&]( const std::string& label, int kind, const std::string& detail, const std::string& insertText, int sortPriority ) {
			QueryCompletion completion;
			completion.label = label;
			completion.kind = kind;
			completion.detail = detail;
			completion.insertText = insertText;
			completion.insertTextFormat = 1;
			completion.sortPriority = sortPriority;
			completion.valid = true;
			output.push_back( std::move( completion ) );
		};
		std::function<bool( const std::string& )> prefixMatch = [&]( const std::string& label ) -> bool {
			if( context.prefix.empty() ) return true;
			return label.rfind( context.prefix, 0 ) == 0;
		};
		if( context.kind == cst::CompletionContextKind::ImportPath ||
		    context.kind == cst::CompletionContextKind::ImportSymbol ) {
			std::string lineText;
			{
				uint32_t currentLine = 0;
				std::size_t lineStart = 0;
				for( std::size_t i = 0; i <= this->text.size(); i++ ) {
					if( i == this->text.size() || this->text[ i ] == '\n' ) {
						if( currentLine == line ) { lineText = this->text.substr( lineStart, i - lineStart ); break; }
						currentLine++;
						lineStart = i + 1;
					}
				}
			}
			std::size_t colByte = col < lineText.size() ? col : lineText.size();
			std::string before = lineText.substr( 0, colByte );
			while( before.empty() == false && ( before.back() == ' ' || before.back() == '\t' ) ) {
				before.pop_back();
			}
			if( context.kind == cst::CompletionContextKind::ImportSymbol ) {
				// Resolve the target module: single-line "from X import" or the
				// opener of a multi-line brace block.
				std::string fromModule = context.modulePath;
				std::string partial;
				std::size_t fromPos = before.find( "from " );
				std::size_t importPos = before.find( " import" );
				if( fromPos != std::string::npos && importPos != std::string::npos && importPos > fromPos ) {
					fromModule = trim( before.substr( fromPos + 5, importPos - ( fromPos + 5 ) ) );
					std::string afterImport = before.substr( importPos + 7 );
					std::size_t lastComma = afterImport.rfind( ',' );
					partial = trim( lastComma == std::string::npos ? afterImport : afterImport.substr( lastComma + 1 ) );
				}
				partial.erase( std::remove( partial.begin(), partial.end(), '{' ), partial.end() );
				partial.erase( std::remove( partial.begin(), partial.end(), '}' ), partial.end() );
				partial = trim( partial );

				// Already-listed symbols/submodules in the current import clause.
				std::set<std::string> alreadyListed;
				{
					std::set<std::string> moduleSegments;
					{
						std::string current;
						for( char ch : fromModule ) {
							if( ch == '.' ) {
								if( current.empty() == false ) {
									moduleSegments.insert( current );
									current.clear();
								}
							}
							else {
								current.push_back( ch );
							}
						}
						if( current.empty() == false ) {
							moduleSegments.insert( current );
						}
					}
					std::string sourceBlock = before;
					if( context.modulePath.empty() == false ) {
						uint32_t openLine = line;
						for( uint32_t back = 0; back <= 30 && line >= back; back++ ) {
							uint32_t candidateLine = line - back;
							std::size_t ls = 0; uint32_t cl = 0;
							for( std::size_t i = 0; i <= this->text.size(); i++ ) {
								if( i == this->text.size() || this->text[ i ] == '\n' ) {
									if( cl == candidateLine ) {
										const std::string ln = this->text.substr( ls, i - ls );
										if( ln.find( " import" ) != std::string::npos && ln.find( '{' ) != std::string::npos ) {
											openLine = candidateLine;
										}
										break;
									}
									cl++;
									ls = i + 1;
								}
							}
						}
						sourceBlock.clear();
						uint32_t closeLine = openLine;
						for( uint32_t forward = openLine; forward < 100000; forward++ ) {
							std::size_t ls = 0; uint32_t cl = 0;
							bool found = false;
							for( std::size_t i = 0; i <= this->text.size(); i++ ) {
								if( i == this->text.size() || this->text[ i ] == '\n' ) {
									if( cl == forward ) {
										const std::string ln = this->text.substr( ls, i - ls );
										sourceBlock += ln + "\n";
										if( ln.find( '}' ) != std::string::npos ) {
											closeLine = forward;
										}
										found = true;
										break;
									}
									cl++;
									ls = i + 1;
								}
							}
							if( found == false || forward >= closeLine ) {
								break;
							}
						}
						if( sourceBlock.empty() ) {
							sourceBlock = before;
						}
					}
					std::string token;
					for( char ch : sourceBlock ) {
						if( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) || ch == '_' || ( ch >= '0' && ch <= '9' ) ) {
							token.push_back( ch );
						}
						else {
							if( token.empty() == false && moduleSegments.count( token ) == 0 &&
								token != "from" && token != "import" && token != "as" ) {
								alreadyListed.insert( token );
							}
							token.clear();
						}
					}
					if( token.empty() == false && moduleSegments.count( token ) == 0 &&
						token != "from" && token != "import" && token != "as" ) {
						alreadyListed.insert( token );
					}
				}
				if( this->moduleGraph != nullptr && fromModule.empty() == false ) {

					// Exported symbols of the target module.
					for( const std::pair<std::string,int> &item : this->moduleGraph->exportedSymbols( fromModule ) ) {
						if( partial.empty() == false && item.first.rfind( partial, 0 ) != 0 ) {
							continue;
						}
						if( alreadyListed.count( item.first ) > 0 ) {
							continue;
						}
						QueryCompletion completion;
						completion.label = item.first;
						completion.kind = item.second;
						completion.detail = fromModule;
						completion.documentation = "Exported from `" + fromModule + "`";
						completion.insertText = item.first;
						completion.insertTextFormat = 1;
						completion.sortPriority = 0;
						completion.valid = true;
						completion.resolvable = true;
						completion.dataModule = fromModule;
						completion.dataSymbol = item.first;
						completion.dataUri = this->uri;
						output.push_back( std::move( completion ) );
					}

					// Direct child submodules under the target module.
					std::vector<std::string> segs;
					{
						std::string cur;
						for( char ch : fromModule ) {
							if( ch == '.' ) {
								if( cur.empty() == false ) {
									segs.push_back( cur );
									cur.clear();
								}
							}
							else {
								cur.push_back( ch );
							}
						}
						if( cur.empty() == false ) {
							segs.push_back( cur );
						}
					}
					std::string filePath = this->moduleGraph->findModule( segs );
					std::string dir = filePath;
					if( dir.empty() == false && std::filesystem::is_regular_file( dir ) ) {
						dir = std::filesystem::path( dir ).parent_path().string();
					}
					if( dir.empty() == false && std::filesystem::is_directory( dir ) ) {
						for( const std::string& name : this->moduleGraph->listModules( dir ) ) {
							if( partial.empty() == false && name.rfind( partial, 0 ) != 0 ) {
								continue;
							}
							if( alreadyListed.count( name ) > 0 ) {
								continue;
							}
							QueryCompletion completion;
							completion.label = name;
							completion.kind = 9;
							completion.detail = fromModule;
							completion.documentation = "Submodule of `" + fromModule + "`";
							completion.insertText = name;
							completion.insertTextFormat = 1;
							completion.sortPriority = 0;
							completion.valid = true;
							output.push_back( std::move( completion ) );
						}
					}
				}
				return output;
			}
			bool isFrom = before.rfind( "from ", 0 ) == 0 || before == "from";
			bool isImport = before.rfind( "import ", 0 ) == 0 || before == "import";
			std::string prefix;
			if( isFrom ) {
				prefix = trim( before.size() > 5 ? before.substr( 5 ) : "" );
			}
			else if( isImport ) {
				prefix = trim( before.size() > 7 ? before.substr( 7 ) : "" );
			}
			else {
				prefix = trim( before );
			}
			std::vector<std::string> segments;
			{
				std::string current;
				for( char ch : prefix ) {
					if( ch == '.' ) { if( current.empty() == false ) { segments.push_back( current ); current.clear(); } }
					else current.push_back( ch );
				}
				if( current.empty() == false ) segments.push_back( current );
			}
			std::string partial;
			if( prefix.empty() == false && prefix.back() != '.' ) {
				if( segments.empty() == false ) { partial = segments.back(); segments.pop_back(); }
			}
			if( this->moduleGraph != nullptr ) {
				if( segments.empty() ) {
					std::unordered_set<std::string> seen;
					std::function<void( const std::string& )> addRoot = [&]( const std::string& name ) {
						if( name.empty() || seen.count( name ) ) {
							return;
						}
						seen.insert( name );
						if( partial.empty() == false && name.rfind( partial, 0 ) != 0 ) {
							return;
						}
						addCompletion( name, 9, "module", name, 0 );
					};
					addRoot( "uranite" );
					for( const std::string& root : this->moduleGraph->allModuleRoots() ) {
						for( const std::string& name : this->moduleGraph->listModules( root ) ) {
							addRoot( name );
						}
					}
				}
				else {
					bool listed = false;
					if( segments.size() == 1 && segments[ 0 ] == "uranite" ) {
						std::unordered_set<std::string> seen;
						for( const std::string& root : this->moduleGraph->allModuleRoots() ) {
							for( const std::string& name : this->moduleGraph->listModules( root ) ) {
								if( seen.count( name ) ) continue;
								seen.insert( name );
								if( partial.empty() == false && name.rfind( partial, 0 ) != 0 ) continue;
								addCompletion( name, 9, "module", name, 0 );
							}
						}
						listed = true;
					}
					if( listed == false ) {
						std::string modulePath = this->moduleGraph->findModule( segments );
						std::string dir = modulePath;
						if( dir.empty() == false ) {
							if( std::filesystem::is_regular_file( dir ) ) dir = std::filesystem::path( dir ).parent_path().string();
						}
						else {
							for( const std::string& root : this->moduleGraph->allModuleRoots() ) {
								std::filesystem::path candidate = std::filesystem::path( root );
								for( const std::string& seg : segments ) candidate /= seg;
								if( std::filesystem::is_directory( candidate ) ) { dir = candidate.string(); break; }
							}
						}
						if( dir.empty() == false && std::filesystem::is_directory( dir ) ) {
							for( const std::string& name : this->moduleGraph->listModules( dir ) ) {
								if( partial.empty() == false && name.rfind( partial, 0 ) != 0 ) continue;
								addCompletion( name, 9, "module", name, 0 );
							}
						}
					}
				}
			}
			return output;
		}
		std::function<bool( const CompletionItem& )> shouldIncludeForContext = [&]( const CompletionItem& item ) -> bool {
			switch( context.kind ) {
				case cst::CompletionContextKind::MemberAccess:
					return true;
				case cst::CompletionContextKind::TypeAnnotation:
					return item.kind == uranite::semantic::Symbol::Kind::Type;
				case cst::CompletionContextKind::CallArgument:
				case cst::CompletionContextKind::StructInit:
				case cst::CompletionContextKind::AsmBlock:
				case cst::CompletionContextKind::Identifier:
				case cst::CompletionContextKind::Keyword:
				case cst::CompletionContextKind::Unknown:
				default:
					return true;
			}
		};

		// The compiler parser is not hardened against unbalanced delimiters;
		// never re-analyze structurally broken mid-typing buffers for completions.
		if( text::looksParseable( this->text ) ) {
			CompletionRequest request;
			request.enabled = true;
			request.line = line;
			request.col = col;
			Frontend frontend( this->overlayStore, this->moduleGraph, this->modulesPath );
			try {
				const FrontendResult analysis = frontend.analyze( this->uri, this->text, request );
				for( const CompletionItem& item : analysis.completions ) {
					if( shouldIncludeForContext( item ) ) {
						QueryCompletion completion;
						completion.label = item.label;
						completion.detail = item.detail;
						completion.documentation = item.documentation;
						completion.insertText = item.insertText.empty() == false ? item.insertText : item.label;
						completion.insertTextFormat = item.insertTextFormat;
						completion.kind = completionKind( item.kind );
						completion.sortPriority = item.sortPriority;
						completion.valid = true;
						output.push_back( std::move( completion ) );
					}
				}
			}
			catch( const std::bad_alloc& ) {
				// The compiler exploded on pathological input; degrade to the
				// syntactic completions already collected.
			}
		}

		// self. / parent. member completion: the compiler does not assign a
		// receiver type for these expressions, so answer from the enclosing
		// class or struct of this document, walking the inheritance chain.
		if( context.kind == cst::CompletionContextKind::MemberAccess && this->result != nullptr ) {
			std::string lineText;
			{
				uint32_t currentLine = 0;
				std::size_t lineStart = 0;
				for( std::size_t i = 0; i <= this->text.size(); i++ ) {
					if( i == this->text.size() || this->text[ i ] == '\n' ) {
						if( currentLine == line ) {
							lineText = this->text.substr( lineStart, i - lineStart );
							break;
						}
						currentLine++;
						lineStart = i + 1;
					}
				}
			}
			std::size_t colByte = col < lineText.size() ? col : lineText.size();
			std::string before = lineText.substr( 0, colByte );
			bool selfReceiver = false;
			bool parentReceiver = false;
			{
				const std::size_t dot = before.rfind( '.' );
				if( dot != std::string::npos ) {
					std::size_t segmentStart = dot;
					while( segmentStart > 0 && before[ segmentStart - 1 ] != ' ' && before[ segmentStart - 1 ] != '\t' && before[ segmentStart - 1 ] != '(' && before[ segmentStart - 1 ] != ',' ) {
						segmentStart--;
					}
					const std::string receiver = before.substr( segmentStart, dot - segmentStart );
					selfReceiver = receiver == "self";
					parentReceiver = receiver == "parent";
				}
			}
			if( selfReceiver || parentReceiver ) {
				// Find the enclosing class or struct name through the CST.
				std::string className;
				{
					TSPoint point = { line, col };
					TSNode node = ts_node_descendant_for_point_range( session.rootNode(), point, point );
					for( int depth = 0; depth < 32 && ts_node_is_null( node ) == false; depth++ ) {
						const char* type = ts_node_type( node );
						if( type != nullptr &&
							( std::strcmp( type, "class_declaration" ) == 0 || std::strcmp( type, "struct_declaration" ) == 0 ) ) {
							std::size_t start = ts_node_start_byte( node );
							std::size_t end = ts_node_end_byte( node );
							if( end > this->text.size() ) end = this->text.size();
							const std::string nodeText = this->text.substr( start, std::min<std::size_t>( end - start, 256 ) );
							const std::size_t keyword = nodeText.find( type[ 0 ] == 'c' ? "class " : "struct " );
							if( keyword != std::string::npos ) {
								std::size_t position = keyword + 6;
								while( position < nodeText.size() && nodeText[ position ] == ' ' ) position++;
								std::size_t nameEnd = position;
								while( nameEnd < nodeText.size() &&
									( std::isalnum( static_cast<unsigned char>( nodeText[ nameEnd ] ) ) != 0 || nodeText[ nameEnd ] == '_' ) ) {
									nameEnd++;
								}
								className = nodeText.substr( position, nameEnd - position );
							}
							break;
						}
						node = ts_node_parent( node );
					}
				}
				if( className.empty() == false ) {
					const auto typeFound = this->result->moduleTypes.find( className );
					if( typeFound != this->result->moduleTypes.end() && typeFound->second != nullptr ) {
						auto classType = std::dynamic_pointer_cast<uranite::semantic::ClassType>( typeFound->second );
						if( parentReceiver && classType != nullptr ) {
							classType = std::dynamic_pointer_cast<uranite::semantic::ClassType>( classType->baseClass );
						}
						std::set<std::string> memberSeen;
						for( const QueryCompletion& item : output ) memberSeen.insert( item.label );
						int guard = 0;
						while( classType != nullptr && guard < 16 ) {
							for( const uranite::semantic::FieldInfo& field : classType->fields ) {
								if( field.name.empty() || memberSeen.count( field.name ) > 0 ) continue;
								memberSeen.insert( field.name );
								addCompletion( field.name, 5, field.type ? field.type->toString() : "",
									field.name, 0 );
							}
							for( const uranite::semantic::MethodInfo& method : classType->methods ) {
								if( method.name.empty() || memberSeen.count( method.name ) > 0 ) continue;
								memberSeen.insert( method.name );
								addCompletion( method.name, 3, method.type ? method.type->toString() : "",
									method.name, 0 );
							}
							classType = std::dynamic_pointer_cast<uranite::semantic::ClassType>( classType->baseClass );
							guard++;
						}
					}
				}
			}
		}

		// G5: primitive and builtin types are always offered in type positions,
		// together with every type known from loaded modules (the compiler does
		// not collect completions inside generic type arguments).
		if( context.kind == cst::CompletionContextKind::TypeAnnotation ) {
			std::set<std::string> seenTypes;
			for( const QueryCompletion& item : output ) seenTypes.insert( item.label );
			if( this->result != nullptr ) {
				for( const auto& entry : this->result->moduleTypes ) {
					if( entry.first.empty() == false && seenTypes.count( entry.first ) == 0 ) {
						seenTypes.insert( entry.first );
						addCompletion( entry.first, 7, "type", entry.first, 2 );
					}
				}
			}
			static const char* builtinTypes[] = {
				"I8", "I16", "I32", "I64", "Int", "U8", "U16", "U32", "U64",
				"F32", "F64", "Bool", "Boolean", "String", "Char", "Void", "Any", "Meta" };
			for( const char* builtin : builtinTypes ) {
				if( seenTypes.count( builtin ) > 0 ) continue;
				seenTypes.insert( builtin );
				addCompletion( builtin, 7, "builtin type", builtin, 2 );
			}
		}

		// The compiler only collects scope completions when the cursor lands on
		// an existing IdentifierExpression. Empty expression/statement lines
		// therefore need an explicit imported-symbol fallback.
		if( this->result != nullptr && this->result->program != nullptr && this->moduleGraph != nullptr &&
		    ( context.kind == cst::CompletionContextKind::Identifier ||
		      context.kind == cst::CompletionContextKind::CallArgument ||
		      context.kind == cst::CompletionContextKind::Keyword ||
		      context.kind == cst::CompletionContextKind::Unknown ) ) {
			auto addImported = [ & ]( const std::string& moduleName, const std::string& originalName,
			                          const std::string& localName, const ModuleGraph::ExportInfo* info ) {
				if( localName.empty() == false && prefixMatch( localName ) == false ) return;
				auto existing = std::find_if( output.begin(), output.end(), [ &localName ]( const QueryCompletion& item ) {
					return item.label == localName;
				} );
				if( existing != output.end() ) {
					if( info != nullptr ) {
						if( existing->detail.empty() ) existing->detail = info->detail;
						if( existing->documentation.empty() ) {
							existing->documentation = info->documentation.empty()
								? fmt::format( "Exported from `{}`", moduleName ) : info->documentation;
						}
						if( existing->kind == 0 ) existing->kind = info->kind;
					}
					if( existing->insertText.empty() ) existing->insertText = localName;
					if( existing->resolvable == false && info != nullptr ) {
						existing->resolvable = true;
						existing->dataModule = moduleName;
						existing->dataSymbol = originalName;
						existing->dataUri = this->uri;
					}
					return;
				}
				QueryCompletion completion;
				completion.label = localName;
				completion.kind = info == nullptr ? 9 : info->kind;
				completion.detail = info == nullptr ? moduleName : info->detail;
				completion.documentation = info == nullptr
					? "Submodule of `" + moduleName + "`"
					: ( info->documentation.empty() ? fmt::format( "Exported from `{}`", moduleName ) : info->documentation );
				completion.insertText = localName;
				completion.insertTextFormat = 1;
				completion.sortPriority = 1;
				completion.valid = true;
				completion.resolvable = info != nullptr;
				completion.dataModule = moduleName;
				completion.dataSymbol = originalName;
				completion.dataUri = this->uri;
				output.push_back( std::move( completion ) );
			};

			for( const ast::nodes::ImportDeclarationSharedPointer& importDecl : this->result->program->imports ) {
				if( importDecl == nullptr ) continue;
				std::string moduleName;
				for( std::size_t i = 0; i < importDecl->path.size(); i++ ) {
					if( i > 0 ) moduleName += ".";
					moduleName += importDecl->path[ i ];
				}
				if( moduleName.empty() ) continue;
				const std::vector<ModuleGraph::ExportInfo> infos = this->moduleGraph->exportedSymbolInfos( moduleName );
				if( importDecl->isFromImport && importDecl->importAll ) {
					for( const ModuleGraph::ExportInfo& info : infos ) addImported( moduleName, info.name, info.name, &info );
					continue;
				}
				if( importDecl->isFromImport == false ) {
					const std::string localName = importDecl->alias.empty() ? importDecl->path.back() : importDecl->alias;
					addImported( moduleName, importDecl->path.back(), localName, nullptr );
					continue;
				}
				for( const ast::nodes::ImportItem& importItem : importDecl->importItems ) {
					const std::string localName = importItem.alias.empty() ? importItem.name : importItem.alias;
					const ModuleGraph::ExportInfo* matched = nullptr;
					for( const ModuleGraph::ExportInfo& info : infos ) {
						if( info.name == importItem.name ) { matched = &info; break; }
					}
					addImported( moduleName, importItem.name, localName, matched );
				}
			}
		}

		// G15: export blocks complete the file's own top-level symbols.
		if( context.kind != cst::CompletionContextKind::ImportPath &&
			context.kind != cst::CompletionContextKind::ImportSymbol &&
			context.kind != cst::CompletionContextKind::AsmBlock &&
			this->index != nullptr && this->uri.empty() == false ) {
			std::string lineText;
			{
				uint32_t currentLine = 0;
				std::size_t lineStart = 0;
				for( std::size_t i = 0; i <= this->text.size(); i++ ) {
					if( i == this->text.size() || this->text[ i ] == '\n' ) {
						if( currentLine == line ) { lineText = this->text.substr( lineStart, i - lineStart ); break; }
						currentLine++;
						lineStart = i + 1;
					}
				}
			}
			const std::size_t first = lineText.find_first_not_of( " \t" );
			const std::string trimmed = first == std::string::npos ? "" : lineText.substr( first );
			if( trimmed.rfind( "export {", 0 ) == 0 ) {
				std::error_code canonicalError;
				const std::string canonicalSelf = std::filesystem::weakly_canonical(
					text::uriToPath( this->uri ), canonicalError ).string();
				std::set<std::string> offered;
				for( const QueryCompletion& item : output ) offered.insert( item.label );
				for( const IndexedSymbol* definition : this->index->allDefinitions() ) {
					if( definition->isDefinition == false || definition->pathname.empty() ) continue;
					if( definition->pathname != canonicalSelf ) continue;
					if( offered.count( definition->name ) > 0 ) continue;
					offered.insert( definition->name );
					addCompletion( definition->name, completionKind( definition->kind ),
						"exported symbol", definition->name, 1 );
				}
			}
		}

		if( context.kind == cst::CompletionContextKind::Keyword ||
			context.kind == cst::CompletionContextKind::Identifier ||
			context.kind == cst::CompletionContextKind::Unknown ) {
			// G3: grammar-aligned keyword table (uranite-tsitter/grammar.js).
			static const char* keywords[] = {
				// access and declaration modifiers
				"public", "private", "protect", "abstract", "virtual", "override", "final",
				"static", "naked", "native", "const", "readonly", "Readonly", "mut", "async",
				// declarations
				"class", "struct", "enum", "backed", "interface", "trait", "type", "function",
				"property", "extern", "implements", "extends", "raises", "unit",
				// control flow
				"if", "elif", "else", "while", "for", "in", "match", "switch", "case",
				"try", "except", "finally", "raise", "defer", "return", "yield", "break",
				"continue", "pass", "delete",
				// expressions and operators
				"new", "self", "parent", "lambda", "await", "unsafe", "asm", "volatile",
				"move", "addressof", "instanceof", "subclassof", "as", "is", "not", "and", "or",
				// literals
				"None", "True", "False" };
			for( const char* keyword : keywords ) {
				if( prefixMatch( keyword ) == false ) continue;
				addCompletion( keyword, 14, "keyword", keyword, 4 );
			}
		}

		// G10: architecture targets are offered strictly inside asm blocks.
		if( context.kind == cst::CompletionContextKind::AsmBlock ) {
			static const char* asmTargets[] = { "x86-64", "aarch64", "riscv64", "arm" };
			for( const char* target : asmTargets ) {
				if( prefixMatch( target ) == false ) continue;
				addCompletion( target, 14, "asm target", target, 4 );
			}
		}

		// Auto-import unimported exported symbols (AST-backed).
		if( this->moduleGraph != nullptr &&
			context.kind != cst::CompletionContextKind::ImportPath &&
			context.kind != cst::CompletionContextKind::ImportSymbol ) {
			std::set<std::string> existingLabels;
			for( const QueryCompletion& item : output ) existingLabels.insert( item.label );

			std::set<std::string> importedSymbols;
			std::map<std::string, uint32_t> importLineByModule;
			std::map<std::string, std::string> importTextByModule;
			if( this->result != nullptr && this->result->program != nullptr ) {
				for( const ast::nodes::ImportDeclarationSharedPointer& importDecl : this->result->program->imports ) {
					if( importDecl == nullptr ) continue;
					std::string moduleName;
					for( std::size_t i = 0; i < importDecl->path.size(); i++ ) {
						if( i > 0 ) moduleName += ".";
						moduleName += importDecl->path[ i ];
					}
					if( importDecl->isFromImport ) {
						for( const ast::nodes::ImportItem& item : importDecl->importItems ) {
							importedSymbols.insert( item.alias.empty() ? item.name : item.alias );
						}
						if( importDecl->source != nullptr && importDecl->source->location != nullptr ) {
							uint32_t importLine = importDecl->source->location->line > 0 ? importDecl->source->location->line - 1 : 0;
							importLineByModule[ moduleName ] = importLine;
							// capture line text
							uint32_t currentLine = 0;
							std::size_t lineStart = 0;
							for( std::size_t i = 0; i <= this->text.size(); i++ ) {
								if( i == this->text.size() || this->text[ i ] == '\n' ) {
									if( currentLine == importLine ) { importTextByModule[ moduleName ] = this->text.substr( lineStart, i - lineStart ); break; }
									currentLine++;
									lineStart = i + 1;
								}
							}
						}
					}
					else {
						if( importDecl->alias.empty() == false ) importedSymbols.insert( importDecl->alias );
						else if( importDecl->path.empty() == false ) importedSymbols.insert( importDecl->path.back() );
					}
				}
			}

			// insertion line: after package declaration and leading module doccomments
			uint32_t insertLine = 0;
			{
				std::istringstream s2( this->text );
				std::string l;
				uint32_t idx = 0;
				bool inBlockComment = false;
				bool seenPackage = false;
				while( std::getline( s2, l ) ) {
					std::string t = trim( l );
					if( inBlockComment ) {
						if( t.find( "*/" ) != std::string::npos ) inBlockComment = false;
						idx++;
						if( seenPackage ) { insertLine = idx; break; }
						continue;
					}
					if( t.rfind( "/*", 0 ) == 0 ) {
						if( t.find( "*/" ) == std::string::npos ) inBlockComment = true;
						idx++;
						if( seenPackage ) { insertLine = idx; break; }
						continue;
					}
					if( t.empty() || t.rfind( "#", 0 ) == 0 || t.rfind( "//", 0 ) == 0 ) {
						idx++;
						if( seenPackage ) { insertLine = idx; }
						continue;
					}
					if( t.rfind( "package ", 0 ) == 0 ) {
						seenPackage = true;
						insertLine = idx + 1;
						idx++;
						continue;
					}
					if( seenPackage ) { insertLine = idx; break; }
					break;
				}
				if( seenPackage == false ) insertLine = 0;
			}

			for( const auto& entry : this->moduleGraph->allExportedSymbols() ) {
				const std::string& symbolName = entry.first;
				const std::string& moduleName = entry.second;
				if( importedSymbols.count( symbolName ) ) continue;

				QueryCompletion completion;
				completion.label = symbolName;
				completion.kind = 3;
				completion.detail = moduleName;
				completion.documentation = "Auto-import from `" + moduleName + "`";
				completion.insertText = symbolName;
				completion.insertTextFormat = 1;
				completion.sortPriority = 5;
				completion.valid = true;
				completion.resolvable = true;
				completion.dataModule = moduleName;
				completion.dataSymbol = symbolName;
				completion.dataUri = this->uri;

				QueryCompletion::TextEdit edit;
				auto lineIt = importLineByModule.find( moduleName );
				if( lineIt != importLineByModule.end() ) {
					std::string lineText = importTextByModule[ moduleName ];
					// append to existing from module import
					std::string newLine = lineText;
					if( newLine.find( symbolName ) == std::string::npos ) {
						while( newLine.empty() == false && ( newLine.back() == ' ' || newLine.back() == '\t' || newLine.back() == '\r' ) ) newLine.pop_back();
						// G4: brace-list imports merge before the closing brace.
						const std::size_t importKeyword = newLine.find( " import " );
						const std::size_t closeBrace = newLine.rfind( '}' );
						if( importKeyword != std::string::npos && closeBrace != std::string::npos &&
						    closeBrace > importKeyword &&
						    newLine.compare( importKeyword + 8, 1, "{" ) == 0 ) {
							std::size_t contentEnd = closeBrace;
							while( contentEnd > importKeyword + 8 &&
							       ( newLine[ contentEnd - 1 ] == ' ' || newLine[ contentEnd - 1 ] == '\t' ) ) {
								contentEnd--;
							}
							newLine = newLine.substr( 0, contentEnd ) + ", " + symbolName + newLine.substr( closeBrace );
						}
						else {
							newLine += ", " + symbolName;
						}
					}
					edit.startLine = lineIt->second;
					edit.startCol = 0;
					edit.endLine = lineIt->second;
					edit.endCol = static_cast<uint32_t>( lineText.size() );
					edit.newText = newLine;
					importTextByModule[ moduleName ] = newLine;
				}
				else {
					edit.startLine = insertLine;
					edit.startCol = 0;
					edit.endLine = insertLine;
					edit.endCol = 0;
					edit.newText = "from " + moduleName + " import " + symbolName + "\n";
					// mark module so we don't create duplicate import lines
					importLineByModule[ moduleName ] = insertLine;
					importTextByModule[ moduleName ] = "from " + moduleName + " import " + symbolName;
				}
				completion.additionalTextEdits.push_back( edit );

				// When the frontend already offered this symbol as a scope item
				// (module symbols enter scope on partial imports), enrich that
				// existing item with the auto-import edit instead of duplicating.
				auto existing = std::find_if( output.begin(), output.end(),
					[ &symbolName ]( const QueryCompletion& item ) { return item.label == symbolName; } );
				if( existing != output.end() ) {
					if( existing->additionalTextEdits.empty() ) {
						existing->additionalTextEdits.push_back( edit );
						existing->resolvable = true;
						existing->dataModule = moduleName;
						existing->dataSymbol = symbolName;
						existing->dataUri = this->uri;
					}
					continue;
				}
				output.push_back( std::move( completion ) );
				existingLabels.insert( symbolName );
			}
		}

		// Analyzer scope collection can emit the same imported symbol more than
		// once. Keep the first item while merging metadata from later duplicates.
		std::vector<QueryCompletion> deduplicated;
		std::unordered_map<std::string, std::size_t> positionByLabel;
		for( QueryCompletion& item : output ) {
			if( item.label.empty() ) continue;
			const auto found = positionByLabel.find( item.label );
			if( found == positionByLabel.end() ) {
				positionByLabel[ item.label ] = deduplicated.size();
				deduplicated.push_back( std::move( item ) );
				continue;
			}
			QueryCompletion& kept = deduplicated[ found->second ];
			if( kept.detail.empty() ) kept.detail = item.detail;
			if( kept.documentation.empty() ) kept.documentation = item.documentation;
			if( kept.kind == 0 ) kept.kind = item.kind;
			if( kept.insertText.empty() ) kept.insertText = item.insertText;
			if( kept.resolvable == false && item.resolvable ) {
				kept.resolvable = true;
				kept.dataModule = item.dataModule;
				kept.dataSymbol = item.dataSymbol;
				kept.dataUri = item.dataUri;
			}
			if( kept.additionalTextEdits.empty() && item.additionalTextEdits.empty() == false ) {
				kept.additionalTextEdits = std::move( item.additionalTextEdits );
			}
		}
		return deduplicated;
	}

	QuerySignature QueryService::signatureHelp( uint32_t line, uint32_t col ) const {
		QuerySignature signature;
		if( this->index == nullptr ) return signature;
		const IndexedCall* call = this->index->callAt( line, col );
		if( call == nullptr ) {
			// The compiler's call-expression ranges can end early (a known
			// attribution quirk); tolerate later arguments by accepting the
			// closest call that starts on the same line before the cursor.
			const std::vector<IndexedCall>& calls = this->index->calls();
			for( const IndexedCall& candidate : calls ) {
				if( candidate.line == line && candidate.col <= col ) {
					if( call == nullptr || candidate.col > call->col ) call = &candidate;
				}
			}
		}
		if( call == nullptr || call->type == nullptr ) return signature;

		const std::shared_ptr<FunctionType> function = std::dynamic_pointer_cast<FunctionType>( call->type );
		if( function == nullptr ) return signature;

		std::vector<std::string> parameters;
		parameters.reserve( function->parameterTypes.size() );
		for( const auto& parameter : function->parameterTypes ) {
			parameters.push_back( parameter == nullptr ? "?" : parameter->toString() );
		}

		std::string joined;
		for( std::size_t i = 0; i < parameters.size(); i++ ) {
			if( i > 0 ) joined += ", ";
			joined += parameters[ i ];
		}
		const std::string returnType = function->returnType == nullptr ? "Void" : function->returnType->toString();
		signature.label = fmt::format( "{}( {} ) -> {}", call->name, joined, returnType );

		for( std::size_t i = 0; i < call->argumentLines.size() && i < call->argumentCols.size(); i++ ) {
			if( line > call->argumentLines[ i ] ||
			    ( line == call->argumentLines[ i ] && col >= call->argumentCols[ i ] ) ) {
				signature.activeParameter = static_cast<uint32_t>( i );
			}
			else break;
		}
		signature.valid = true;
		return signature;
	}

	QueryLocation QueryService::prepareRename( uint32_t line, uint32_t col ) const {
		if( this->index == nullptr ) return QueryLocation();
		const IndexedSymbol* symbol = this->index->symbolAt( line, col );
		if( symbol == nullptr ) return QueryLocation();
		return toLocation( *symbol );
	}

	std::vector<QueryLocation> QueryService::rename( uint32_t line, uint32_t col ) const {
		return this->locationsFor( line, col, true );
	}

	std::vector<QueryWorkspaceSymbol> QueryService::workspaceSymbols( const std::string& query ) const {
		std::vector<QueryWorkspaceSymbol> output;
		if( this->index == nullptr ) return output;
		const std::string needle = lower( query );
		for( const IndexedSymbol* definition : this->index->allDefinitions() ) {
			if( needle.empty() == false && lower( definition->qualifiedName ).find( needle ) == std::string::npos &&
			    lower( definition->name ).find( needle ) == std::string::npos ) {
				continue;
			}
			QueryWorkspaceSymbol item;
			item.name = definition->qualifiedName.empty() ? definition->name : definition->qualifiedName;
			item.location = toLocation( *definition );
			output.push_back( std::move( item ) );
			if( output.size() >= 200 ) break;
		}
		return output;
	}

} // namespace uranite::lsp::semantic
