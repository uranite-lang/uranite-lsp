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

#include "uranite-lsp/semantic/query-service.hpp"

#include "uranite-lsp/cst/parse-session.hpp"
#include "uranite-lsp/cst/syntactic-index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "uranite/semantic/typeref.hpp"

namespace uranite::lsp::semantic {

	namespace {

		using uranite::semantic::CompletionItem;
		using uranite::semantic::FunctionType;
		using uranite::semantic::Symbol;

		std::string trim( const std::string& value ) {
			const std::size_t first = value.find_first_not_of( " \\t\\r\\n" );
			if( first == std::string::npos ) return "";
			const std::size_t last = value.find_last_not_of( " \\t\\r\\n" );
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
		if( this->index == nullptr ) return hover;
		const IndexedSymbol* symbol = this->index->symbolAt( line, col );
		if( symbol == nullptr ) return hover;

		std::string contents = fmt::format( "**{}**\n\n_{}_", symbol->name, kindName( symbol->kind ) );
		if( symbol->type != nullptr ) {
			contents += fmt::format( "\n\n`{}`", symbol->type->toString() );
		}
		if( symbol->qualifiedName.empty() == false && symbol->qualifiedName != symbol->name ) {
			contents += fmt::format( "\n\n`{}`", symbol->qualifiedName );
		}
		hover.contents = std::move( contents );
		hover.valid = true;
		return hover;
	}

	QueryLocation QueryService::definition( uint32_t line, uint32_t col ) const {
		QueryLocation location;
		if( this->index == nullptr ) return location;
		const IndexedSymbol* usage = this->index->symbolAt( line, col );
		if( usage == nullptr ) return location;

		const std::vector<const IndexedSymbol*> definitions = this->index->definitionsFor( usage->qualifiedName );
		if( definitions.empty() ) return location;
		for( const IndexedSymbol* definition : definitions ) {
			if( definition->symbol == usage->symbol ) return toLocation( *definition );
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
				std::string fromModule;
				std::string partial;
				std::size_t fromPos = before.find( "from " );
				std::size_t importPos = before.find( " import" );
				if( fromPos != std::string::npos && importPos != std::string::npos && importPos > fromPos ) {
					fromModule = trim( before.substr( fromPos + 5, importPos - ( fromPos + 5 ) ) );
					std::string afterImport = before.substr( importPos + 7 );
					std::size_t lastComma = afterImport.rfind( ',' );
					partial = trim( lastComma == std::string::npos ? afterImport : afterImport.substr( lastComma + 1 ) );
				}
				if( this->moduleGraph != nullptr && fromModule.empty() == false ) {
					for( const auto& item : this->moduleGraph->exportedSymbols( fromModule ) ) {
						if( partial.empty() == false && item.first.rfind( partial, 0 ) != 0 ) continue;
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
				}
				return output;
			}

			bool isFrom = before.rfind( "from ", 0 ) == 0;
			std::string prefix = isFrom ? trim( before.substr( 5 ) ) : trim( before.substr( 7 ) );
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

		CompletionRequest request;
		request.enabled = true;
		request.line = line;
		request.col = col;
		Frontend frontend( this->overlayStore, this->moduleGraph, this->modulesPath );
		const FrontendResult analysis = frontend.analyze( this->uri, this->text, request );

		auto shouldIncludeForContext = [&]( const CompletionItem& item ) -> bool {
			switch( context.kind ) {
				case cst::CompletionContextKind::MemberAccess:
					return true;
				case cst::CompletionContextKind::TypeAnnotation:
					return item.kind == uranite::semantic::Symbol::Kind::Type;
				case cst::CompletionContextKind::CallArgument:
				case cst::CompletionContextKind::StructInit:
				case cst::CompletionContextKind::Identifier:
				case cst::CompletionContextKind::Keyword:
				case cst::CompletionContextKind::Unknown:
				default:
					return true;
			}
		};

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

		if( context.kind == cst::CompletionContextKind::Keyword ||
		    context.kind == cst::CompletionContextKind::Identifier ||
		    context.kind == cst::CompletionContextKind::Unknown ) {
			static const char* keywords[] = {
				"public", "private", "protected", "abstract", "virtual", "override", "final",
				"naked", "native", "const", "Readonly", "if", "elif", "else", "while", "for",
				"match", "in", "try", "except", "finally", "raise", "defer", "return", "yield",
				"async", "await", "unsafe", "asm", "volatile" };
			for( const char* keyword : keywords ) {
				if( prefixMatch( keyword ) == false ) continue;
				addCompletion( keyword, 14, "keyword", keyword, 4 );
			}
		}

		if( context.kind == cst::CompletionContextKind::CallArgument ||
		    context.kind == cst::CompletionContextKind::Identifier ||
		    context.kind == cst::CompletionContextKind::Unknown ) {
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
				if( existingLabels.count( symbolName ) ) continue;
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
						newLine += ", " + symbolName;
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
