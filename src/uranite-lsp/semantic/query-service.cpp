
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
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
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

		const char* kindName( Symbol::Kind kind ) {
			switch( kind ) {
				case Symbol::Kind::EnumVariant: return "enum variant";
				case Symbol::Kind::Field: return "field";
				case Symbol::Kind::Function: return "function";
				case Symbol::Kind::Module: return "module";
				case Symbol::Kind::Parameter: return "parameter";
				case Symbol::Kind::Type: return "type";
				case Symbol::Kind::Variable: return "variable";
			}
			return "symbol";
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

		/** @brief Extracts a triple-quoted Uranite docstring attached after a declaration header. */
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
					const std::string head = trim( line.substr( 3 ) );
					const std::size_t closed = head.find( "\"\"\"" );
					if( closed != std::string::npos ) return trim( head.substr( 0, closed ) );
					std::string result = head;
					std::size_t scan = lineEnd + 1;
					while( scan <= source.size() ) {
						std::size_t end = source.find( '\n', scan );
						if( end == std::string::npos ) end = source.size();
						const std::string next = source.substr( scan, end - scan );
						const std::size_t stop = next.find( "\"\"\"" );
						if( stop != std::string::npos ) {
							result += "\n" + trim( next.substr( 0, stop ) );
							return trim( result );
						}
						result += "\n" + trim( next );
						scan = end + 1;
					}
					return trim( result );
				}
				if( lineEnd >= source.size() ) return "";
				cursor = lineEnd + 1;
			}
			return "";
		}

		/** @brief Extracts declaration modifier keywords from a declaration line. */
		std::vector<std::string> extractModifiers( const std::string& lineText ) {
			static const char* modifiers[] = {
				"public", "private", "protect", "abstract", "virtual", "override", "final",
				"static", "naked", "native", "const", "readonly", "Readonly", "async", "extern" };
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

	QueryHover QueryService::hover( uint32_t line, uint32_t col ) const {
		QueryHover hover;
		// Imports: packages, submodules and imported symbols.
		ImportTarget target = importTargetAt( this->text, line, col );
		if( target.kind != ImportTarget::None ) {
			if( target.kind == ImportTarget::ModuleSegment ) {
				std::string filePath;
				if( this->moduleGraph != nullptr ) {
					std::vector<std::string> segs;
					std::string cur;
					for( char ch : target.modulePath ) {
						if( ch == '.' ) { if( cur.empty() == false ) { segs.push_back( cur ); cur.clear(); } }
						else cur.push_back( ch );
					}
					if( cur.empty() == false ) segs.push_back( cur );
					filePath = this->moduleGraph->findModule( segs );
				}
				std::string contents = fmt::format( "**{}**\n\n_module_", target.modulePath );
				if( filePath.empty() == false ) {
					contents += fmt::format( "\n\n`{}`", filePath );
					const std::string source = fileSourceFor( this->overlayStore.get(), filePath );
					const std::string doc = extractDocstringAfter( source, 0 );
					if( doc.empty() == false ) contents += "\n\n" + doc;
					if( this->moduleGraph != nullptr ) {
						auto exports = this->moduleGraph->exportedSymbols( target.modulePath );
						if( exports.empty() == false ) {
							contents += "\n\nExports: ";
							std::size_t count = 0;
							for( const auto& item : exports ) {
								if( count > 0 ) contents += ", ";
								contents += item.first;
								if( ++count >= 12 ) { contents += ", ..."; break; }
							}
						}
					}
				}
				hover.contents = std::move( contents );
				hover.valid = true;
				return hover;
			}
			if( this->moduleGraph != nullptr ) {
				auto infos = this->moduleGraph->exportedSymbolInfos( target.modulePath );
				for( const auto& info : infos ) {
					if( info.name != target.symbolName ) continue;
					std::string contents = fmt::format( "**{}**\n\n_{}_", info.name, kindName( static_cast<uranite::semantic::Symbol::Kind>( info.kind == 3 ? 3 : info.kind ) ) );
					if( info.detail.empty() == false ) contents += fmt::format( "\n\n`{}`", info.detail );
					contents += fmt::format( "\n\n`{}`", target.modulePath + "." + info.name );
					if( info.documentation.empty() == false ) contents += "\n\n" + info.documentation;
					hover.contents = std::move( contents );
					hover.valid = true;
					return hover;
				}
			}
			std::string qualified = target.modulePath + "." + target.symbolName;
			const std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( qualified );
			if( definitions.empty() == false ) {
				const IndexedSymbol* symbol = definitions.front();
				std::string contents = fmt::format( "**{}**\n\n_{}_", symbol->name, kindName( symbol->kind ) );
				if( symbol->type != nullptr ) contents += fmt::format( "\n\n`{}`", symbol->type->toString() );
				if( symbol->qualifiedName.empty() == false && symbol->qualifiedName != symbol->name ) {
					contents += fmt::format( "\n\n`{}`", symbol->qualifiedName );
				}
				hover.contents = std::move( contents );
				hover.valid = true;
				return hover;
			}
			return hover;
		}

		const IndexedSymbol* symbol = this->index->symbolAt( line, col );
		if( symbol == nullptr ) return hover;

		std::string contents = fmt::format( "**{}**\n\n_{}_", symbol->name, kindName( symbol->kind ) );
		if( symbol->type != nullptr ) {
			contents += fmt::format( "\n\n`{}`", symbol->type->toString() );
		}
		if( symbol->qualifiedName.empty() == false && symbol->qualifiedName != symbol->name ) {
			contents += fmt::format( "\n\n`{}`", symbol->qualifiedName );
		}

		// G12: surface declaration modifiers and the attached docstring from the definition site.
		const IndexedSymbol* definition = symbol;
		if( symbol->isDefinition == false ) {
			const std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( symbol->qualifiedName );
			if( definitions.empty() == false ) {
				definition = definitions.front();
				for( const IndexedSymbol* candidate : definitions ) {
					if( candidate->symbol == symbol->symbol ) {
						definition = candidate;
						break;
					}
				}
				if( definition->symbol != symbol->symbol && symbol->container.empty() == false ) {
					for( const IndexedSymbol* candidate : definitions ) {
						if( candidate->container == symbol->container ) {
							definition = candidate;
							break;
						}
					}
				}
			}
		}
		if( definition != nullptr && definition->pathname.empty() == false ) {
			const std::string source = fileSourceFor( this->overlayStore.get(), definition->pathname );
			if( source.empty() == false ) {
				std::size_t lineStart = 0;
				uint32_t walked = 0;
				while( walked < definition->line && lineStart < source.size() ) {
					const std::size_t newline = source.find( '\n', lineStart );
					if( newline == std::string::npos ) break;
					lineStart = newline + 1;
					walked++;
				}
				if( walked == definition->line ) {
					std::size_t lineEnd = source.find( '\n', lineStart );
					if( lineEnd == std::string::npos ) lineEnd = source.size();
					const std::string declarationLine = source.substr( lineStart, lineEnd - lineStart );
					const std::vector<std::string> modifiers = extractModifiers( declarationLine );
					if( modifiers.empty() == false ) {
						std::string joined;
						for( std::size_t i = 0; i < modifiers.size(); i++ ) {
							if( i > 0 ) joined += " ";
							joined += modifiers[ i ];
						}
						contents += fmt::format( "\n\n_{}_", joined );
					}
					const std::string docstring = extractDocstringAfter( source, lineStart );
					if( docstring.empty() == false ) {
						contents += fmt::format( "\n\n{}", docstring );
					}
				}
			}
		}
		hover.contents = std::move( contents );
		hover.valid = true;
		return hover;
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
					const std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( qualified );
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
		if( this->overlayStore == nullptr ) return output;

		cst::ParseSession session;
		session.parse( this->text );
		cst::CompletionContext context = cst::SyntacticIndex::completionContext( this->text, session.rootNode(), line, col );

		auto addCompletion = [&]( const std::string& label, int kind, const std::string& detail,
		                          const std::string& insertText, int sortPriority ) {
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

		auto prefixMatch = [&]( const std::string& label ) -> bool {
			if( context.prefix.empty() ) return true;
			return label.rfind( context.prefix, 0 ) == 0;
		};

		if( context.kind == cst::CompletionContextKind::ImportPath ||
		    context.kind == cst::CompletionContextKind::ImportSymbol ) {
			// Parse current import line for module path and partial.
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
			while( before.empty() == false && ( before.back() == ' ' || before.back() == '\t' ) ) before.pop_back();

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
						if( ch == '.' ) { if( current.empty() == false ) { moduleSegments.insert( current ); current.clear(); } }
						else current.push_back( ch );
					}
					if( current.empty() == false ) moduleSegments.insert( current );
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
									if( ln.find( '}' ) != std::string::npos ) closeLine = forward;
									found = true;
									break;
								}
								cl++;
								ls = i + 1;
							}
						}
						if( found == false || forward >= closeLine ) break;
					}
					if( sourceBlock.empty() ) sourceBlock = before;
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
				for( const auto& item : this->moduleGraph->exportedSymbols( fromModule ) ) {
					if( partial.empty() == false && item.first.rfind( partial, 0 ) != 0 ) continue;
					if( alreadyListed.count( item.first ) > 0 ) continue;
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
						if( ch == '.' ) { if( cur.empty() == false ) { segs.push_back( cur ); cur.clear(); } }
						else cur.push_back( ch );
					}
					if( cur.empty() == false ) segs.push_back( cur );
				}
				std::string filePath = this->moduleGraph->findModule( segs );
				std::string dir = filePath;
				if( dir.empty() == false && std::filesystem::is_regular_file( dir ) ) dir = std::filesystem::path( dir ).parent_path().string();
				if( dir.empty() == false && std::filesystem::is_directory( dir ) ) {
					for( const std::string& name : this->moduleGraph->listModules( dir ) ) {
						if( partial.empty() == false && name.rfind( partial, 0 ) != 0 ) continue;
						if( alreadyListed.count( name ) > 0 ) continue;
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

			bool isFrom = before.rfind( "from ", 0 ) == 0;
			// "before" was right-trimmed, so a bare "import" line can be shorter
			// than the keyword: guard the offsets (crash found by the e2e suite).
			std::string prefix;
			if( isFrom ) {
				prefix = trim( before.size() > 5 ? before.substr( 5 ) : "" );
			}
			else if( before.size() >= 6 && before.compare( 0, 6, "import" ) == 0 ) {
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
					for( const std::string& root : this->moduleGraph->allModuleRoots() ) {
						for( const std::string& name : this->moduleGraph->listModules( root ) ) {
							if( seen.count( name ) ) continue;
							seen.insert( name );
							if( partial.empty() == false && name.rfind( partial, 0 ) != 0 ) continue;
							addCompletion( name, 9, "module", name, 0 );
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

		auto shouldIncludeForContext = [&]( const CompletionItem& item ) -> bool {
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
					if( shouldIncludeForContext( item ) == false ) continue;
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
						if( currentLine == line ) { lineText = this->text.substr( lineStart, i - lineStart ); break; }
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
					while( segmentStart > 0 && before[ segmentStart - 1 ] != ' ' && before[ segmentStart - 1 ] != '\t' &&
					       before[ segmentStart - 1 ] != '(' && before[ segmentStart - 1 ] != ',' ) {
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

		return output;
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
