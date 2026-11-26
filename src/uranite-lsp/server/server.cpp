
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
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <sstream>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

#include "uranite/diagnostic/diagnostic.hpp"

#include "uranite-lsp/cst/fold-provider.hpp"
#include "uranite-lsp/cst/highlight-provider.hpp"
#include "uranite-lsp/cst/language.hpp"
#include "uranite-lsp/cst/parse-session.hpp"
#include "uranite-lsp/cst/query-set.hpp"
#include "uranite-lsp/cst/symbol-provider.hpp"
#include "uranite-lsp/jsonrpc/error.hpp"
#include "uranite-lsp/jsonrpc/message.hpp"
#include "uranite-lsp/protocol/convert.hpp"
#include "uranite-lsp/semantic/query-service.hpp"
#include "uranite-lsp/server/server.hpp"
#include "uranite-lsp/text/document.hpp"
#include "uranite-lsp/text/position.hpp"
#include "uranite-lsp/text/uri.hpp"
#include "uranite-lsp/workspace/document-store.hpp"

namespace uranite::lsp::server {

	namespace {

		using semantic::QueryLocation;

		static std::string trim( const std::string& value ) {
			std::size_t start = 0;
			while( start < value.size() && ( value[ start ] == ' ' || value[ start ] == '\t' || value[ start ] == '\r' || value[ start ] == '\n' ) ) start++;
			std::size_t end = value.size();
			while( end > start && ( value[ end - 1 ] == ' ' || value[ end - 1 ] == '\t' || value[ end - 1 ] == '\r' || value[ end - 1 ] == '\n' ) ) end--;
			return value.substr( start, end - start );
		}

		text::PositionEncoding toEncoding( const std::string& encoding ) {
			if( encoding == "utf-8" ) return text::PositionEncoding::Utf8;
			return text::PositionEncoding::Utf16;
		}

		std::string lineAtText( const std::string& text, uint32_t line ) {
			uint32_t current = 0;
			std::size_t start = 0;
			for( std::size_t i = 0; i <= text.size(); i++ ) {
				if( i == text.size() || text[ i ] == '\n' ) {
					if( current == line ) return text.substr( start, i - start );
					current++;
					start = i + 1;
				}
			}
			return "";
		}

		std::string toDocumentUri( const std::string& path ) {
			if( path.compare( 0, 7, "file://" ) == 0 ) return path;
			return text::pathToUri( path );
		}

		std::string documentTextFor( const workspace::DocumentStore& documents, const std::string& uriOrPath ) {
			const workspace::DocumentEntry* entry = documents.get( uriOrPath );
			if( entry != nullptr ) return entry->document.text();
			const std::string uri = toDocumentUri( uriOrPath );
			entry = documents.get( uri );
			if( entry != nullptr ) return entry->document.text();
			return "";
		}

		llvm::json::Object rangeObject( const QueryLocation& location, text::PositionEncoding encoding, const workspace::DocumentStore& documents ) {
			const std::string documentText = documentTextFor( documents, location.path );
			const std::string startLineText = lineAtText( documentText, location.line );
			const std::string endLineText = ( location.endLine == location.line ) ? startLineText : lineAtText( documentText, location.endLine );
			return llvm::json::Object{
				{ "start", llvm::json::Object{ { "line", location.line }, { "character", text::toLspCharacter( startLineText, location.col, encoding ) } } },
				{ "end", llvm::json::Object{ { "line", location.endLine }, { "character", text::toLspCharacter( endLineText, location.endCol, encoding ) } } },
			};
		}

		llvm::json::Object locationObject( const QueryLocation& location, text::PositionEncoding encoding, const workspace::DocumentStore& documents ) {
			return llvm::json::Object{
				{ "uri", toDocumentUri( location.path ) },
				{ "range", rangeObject( location, encoding, documents ) },
			};
		}

		llvm::json::Array locationsArray( const std::vector<QueryLocation>& locations, text::PositionEncoding encoding, const workspace::DocumentStore& documents ) {
			llvm::json::Array output;
			for( const QueryLocation& location : locations ) {
				if( location.valid ) output.push_back( locationObject( location, encoding, documents ) );
			}
			return output;
		}

		bool requestPosition( const llvm::json::Value& params, std::string& uri, uint32_t& line, uint32_t& col,
		                      text::PositionEncoding encoding, const workspace::DocumentStore& documents ) {
			const llvm::json::Object* object = params.getAsObject();
			if( object == nullptr ) return false;
			const llvm::json::Object* document = object->getObject( "textDocument" );
			const llvm::json::Object* position = object->getObject( "position" );
			if( document == nullptr || position == nullptr ) return false;
			const auto uriValue = document->getString( "uri" );
			const auto lineValue = position->getInteger( "line" );
			const auto colValue = position->getInteger( "character" );
			if( uriValue.has_value() == false || lineValue.has_value() == false || colValue.has_value() == false ) return false;
			uri = uriValue->str();
			line = static_cast<uint32_t>( std::max<int64_t>( 0, *lineValue ) );
			uint32_t character = static_cast<uint32_t>( std::max<int64_t>( 0, *colValue ) );
			std::string documentText;
			const workspace::DocumentEntry* entry = documents.get( uri );
			if( entry != nullptr ) documentText = entry->document.text();
			col = text::toByteOffset( lineAtText( documentText, line ), character, encoding );
			return true;
		}

		std::string applyRangeEdit( const std::string& text, const llvm::json::Object& range, const std::string& newText,
		                            text::PositionEncoding encoding ) {
			const llvm::json::Object* start = range.getObject( "start" );
			const llvm::json::Object* end = range.getObject( "end" );
			if( start == nullptr || end == nullptr ) return text;
			const int64_t startLine = std::max<int64_t>( 0, start->getInteger( "line" ).value_or( 0 ) );
			const int64_t startCharacter = std::max<int64_t>( 0, start->getInteger( "character" ).value_or( 0 ) );
			const int64_t endLine = std::max<int64_t>( 0, end->getInteger( "line" ).value_or( 0 ) );
			const int64_t endCharacter = std::max<int64_t>( 0, end->getInteger( "character" ).value_or( 0 ) );

			std::vector<std::size_t> lineStarts;
			lineStarts.push_back( 0 );
			for( std::size_t i = 0; i < text.size(); i++ ) {
				if( text[ i ] == '\n' ) lineStarts.push_back( i + 1 );
			}
			auto offsetOf = [ & ]( int64_t line, int64_t character ) -> std::size_t {
				if( static_cast<std::size_t>( line ) >= lineStarts.size() ) return text.size();
				const std::size_t lineStart = lineStarts[ static_cast<std::size_t>( line ) ];
				std::size_t lineEnd = lineStart;
				while( lineEnd < text.size() && text[ lineEnd ] != '\n' ) lineEnd++;
				const std::string lineText = text.substr( lineStart, lineEnd - lineStart );
				const uint32_t byteOffset = text::toByteOffset( lineText, static_cast<uint32_t>( character ), encoding );
				return lineStart + std::min<std::size_t>( byteOffset, lineText.size() );
			};
			const std::size_t startOffset = offsetOf( startLine, startCharacter );
			const std::size_t endOffset = offsetOf( endLine, endCharacter );
			if( startOffset > endOffset || startOffset > text.size() ) return text;
			return text.substr( 0, startOffset ) + newText + text.substr( std::min( endOffset, text.size() ) );
		}

	} // namespace

	static std::string requestUri( const llvm::json::Value& params ) {
		const llvm::json::Object* object = params.getAsObject();
		if( object == nullptr ) return "";
		const llvm::json::Object* document = object->getObject( "textDocument" );
		if( document == nullptr ) return "";
		return document->getString( "uri" ).value_or( "" ).str();
	}

	static std::string resolveModulesPath() {
		const char* environment = std::getenv( "URANITE_MODULES_PATH" );
		if( environment != nullptr && std::filesystem::is_directory( environment ) ) return environment;

		static const char* candidates[] = {
			"/usr/local/lib/uranite/stdlibs",
			"/usr/local/share/uranite/stdlibs",
			"../uranite/stdlibs",
			"./stdlibs",
		};
		for( const char* candidate : candidates ) {
			if( std::filesystem::is_directory( candidate ) ) return candidate;
		}
		return "";
	}

	Server::Server( std::FILE* in, std::FILE* out ) : input( in ), output( out ) {
		this->overlayStore = std::make_shared<semantic::OverlayStore>();
		this->moduleGraph = std::make_shared<semantic::ModuleGraph>();
		this->modulesPath = resolveModulesPath();
		if( this->modulesPath.empty() == false ) this->moduleGraph->setModulesPathOverride( this->modulesPath );
		if( this->queries.load( "" ) == false ) {
			jsonrpc::writeNotification( this->output, "window/logMessage",
				llvm::json::Object{ { "type", 1 }, { "message", "failed to compile queries: " + this->queries.lastError() } } );
		}
	}

	Server::~Server() {
		this->scheduler.stop();
	}

	void Server::flushAnalysis() {
		this->scheduler.flush();
	}

	bool Server::readAndDispatch( bool flush ) {
		const jsonrpc::ReadResult read = jsonrpc::readMessageResult( this->input );
		if( read.status == jsonrpc::ReadStatus::EndOfFile ) return false;
		if( read.status == jsonrpc::ReadStatus::Malformed ) {
			jsonrpc::writeError( this->output, nullptr,
				static_cast<int>( jsonrpc::ErrorCode::ParseError ), read.error );
			return true;
		}
		if( read.message.has_value() == false ) return true;
		if( read.message->method == "exit" ) return false;
		this->handle( *read.message );
		if( flush ) this->flushAnalysis();
		return true;
	}

	int Server::run() {
		while( this->readAndDispatch( false ) ) {
		}
		// LSP exit code: zero only when shutdown was received before exit.
		return this->shutdownRequested ? 0 : 1;
	}

	bool Server::step() {
		return this->readAndDispatch( true );
	}

	std::shared_ptr<semantic::QueryService> Server::queryServiceFor( const std::string& uri ) const {
		std::shared_ptr<semantic::FrontendResult> result;
		{
			std::lock_guard<std::mutex> lock( this->resultsMutex );
			const auto found = this->analysisResults.find( uri );
			if( found == this->analysisResults.end() ) return nullptr;
			result = found->second;
		}
		if( result == nullptr || result->symbolIndex == nullptr ) return nullptr;

		std::string text;
		const workspace::DocumentEntry* entry = this->documents.get( uri );
		if( entry != nullptr ) text = entry->document.text();
		return std::make_shared<semantic::QueryService>( result, result->symbolIndex, this->overlayStore,
			this->modulesPath, uri, text, this->moduleGraph );
	}

	void Server::runAnalysis( const std::string& uri ) {
		const workspace::DocumentEntry* entry = this->documents.get( uri );
		if( entry == nullptr ) return;

		const std::string text = entry->document.text();
		this->scheduler.schedule( uri, [ this, uri, text ]( uint64_t generation ) {
			this->analyzeAndPublish( uri, text, generation );
		} );
	}

	void Server::analyzeAndPublish( const std::string& uri, const std::string& text, uint64_t generation ) {
		if( this->scheduler.currentGeneration( uri ) != generation ) return;


		// The compiler parser is not hardened against unbalanced delimiters;
		// mid-typing buffers must never reach Frontend::analyze.
		if( text::looksParseable( text ) == false ) {
			llvm::json::Array diagnostics;
			llvm::json::Object params;
			params[ "uri" ] = uri;
			params[ "diagnostics" ] = std::move( diagnostics );
			jsonrpc::writeNotification( this->output, "textDocument/publishDiagnostics",
				llvm::json::Value( std::move( params ) ) );
			return;
		}

		semantic::Frontend frontend( this->overlayStore, this->moduleGraph, this->modulesPath );
		semantic::FrontendResult result;
		try {
			result = frontend.analyze( uri, text );
		}
		catch( const std::bad_alloc& ) {
			// The compiler exploded on pathological input; keep any prior
			// analysis result and skip publishing for this generation.
			return;
		}
		catch( const std::exception& e ) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock( this->resultsMutex );
			this->analysisResults[ uri ] = std::make_shared<semantic::FrontendResult>( std::move( result ) );
		}
		if( this->scheduler.currentGeneration( uri ) != generation ) return;

		std::shared_ptr<semantic::FrontendResult> current;
		{
			std::lock_guard<std::mutex> lock( this->resultsMutex );
			current = this->analysisResults[ uri ];
		}


		llvm::json::Array diagnostics;
		for( const auto& item : current->diagnostics.diagnostics() ) {
			llvm::json::Object diagnostic;
			int severity = 1;
			if( item.level == diagnostic::Level::Warning ) severity = 2;
			else if( item.level == diagnostic::Level::Note ) severity = 3;
			diagnostic[ "severity" ] = severity;
			std::string message = item.messages;
			if( item.hinting.empty() == false ) message += " (" + item.hinting + ")";
			diagnostic[ "message" ] = message;

			int startLine = 0;
			int startCol = 0;
			int endLine = 0;
			int endCol = 0;
			protocol::sourceToLspRange( item.source, startLine, startCol, endLine, endCol, toEncoding( this->positionEncoding ), text );
			diagnostic[ "range" ] = llvm::json::Object{
				{ "start", llvm::json::Object{ { "line", startLine }, { "character", startCol } } },
				{ "end", llvm::json::Object{ { "line", endLine }, { "character", endCol } } },
			};
			diagnostics.push_back( std::move( diagnostic ) );
		}

		llvm::json::Object params;
		params[ "uri" ] = uri;
		params[ "diagnostics" ] = std::move( diagnostics );
		jsonrpc::writeNotification( this->output, "textDocument/publishDiagnostics",
			llvm::json::Value( std::move( params ) ) );
	}

	void Server::handle( const jsonrpc::Message& msg ) {
		// LSP shutdown semantics: once shutdown is received, every further
		// non-exit request is rejected with InvalidRequest and notifications
		// are dropped.
		if( this->shutdownRequested == true && msg.method != "exit" ) {
			if( msg.id.has_value() ) {
				jsonrpc::writeError( this->output, *msg.id,
					static_cast<int>( jsonrpc::ErrorCode::InvalidRequest ),
					"Server is shutting down" );
			}
			return;
		}

		if( msg.method == "initialize" ) {
			// Position encoding negotiation (LSP 3.17).
			this->positionEncoding = "utf-16";
			if( msg.params.getAsObject() != nullptr ) {
				const llvm::json::Object* params = msg.params.getAsObject();
				const llvm::json::Object* capabilities = params->getObject( "capabilities" );
				const llvm::json::Object* general = ( capabilities != nullptr ) ? capabilities->getObject( "general" ) : nullptr;
				const llvm::json::Array* encodings = ( general != nullptr ) ? general->getArray( "positionEncodings" ) : nullptr;
				bool utf16Supported = false;
				bool utf8Supported = false;
				if( encodings != nullptr ) {
					for( const llvm::json::Value& value : *encodings ) {
						const auto encoding = value.getAsString();
						if( encoding.has_value() == false ) continue;
						if( encoding->str() == "utf-16" ) utf16Supported = true;
						if( encoding->str() == "utf-8" ) utf8Supported = true;
					}
				}
				if( utf16Supported ) this->positionEncoding = "utf-16";
				else if( utf8Supported ) this->positionEncoding = "utf-8";
				else this->positionEncoding = "utf-16";
			}

			llvm::json::Object caps;
			llvm::json::Object textSync;
			textSync[ "openClose" ] = true;
			textSync[ "change" ] = 2; // Incremental (range-based change events supported)
			textSync[ "save" ] = true;
			caps[ "textDocumentSync" ] = std::move( textSync );
			caps[ "hoverProvider" ] = true;
			caps[ "definitionProvider" ] = true;
			caps[ "referencesProvider" ] = true;
			caps[ "documentSymbolProvider" ] = true;
			caps[ "foldingRangeProvider" ] = true;
			caps[ "positionEncoding" ] = this->positionEncoding;

			llvm::json::Object semanticTokens;
			llvm::json::Array tokenTypes;
			static const char* legend[] = { "namespace", "type", "class", "enum", "interface",
				"struct", "typeParameter", "parameter", "variable", "property", "enumMember",
				"function", "method", "keyword", "string", "number", "operator", "comment" };
			for( const char* token : legend ) tokenTypes.push_back( token );
			semanticTokens[ "tokenTypes" ] = std::move( tokenTypes );
			semanticTokens[ "tokenModifiers" ] = llvm::json::Array{};
			semanticTokens[ "full" ] = true;
			caps[ "semanticTokensProvider" ] = std::move( semanticTokens );

			llvm::json::Object signatureHelp;
			signatureHelp[ "triggerCharacters" ] = llvm::json::Array{ "(", "," };
			caps[ "signatureHelpProvider" ] = std::move( signatureHelp );
			caps[ "completionProvider" ] = llvm::json::Object{
				{ "triggerCharacters", llvm::json::Array{ ".", ":", "<", "(", "," } },
				{ "resolveProvider", true } };
			caps[ "renameProvider" ] = llvm::json::Object{ { "prepareProvider", true } };
			caps[ "workspaceSymbolProvider" ] = true;

			llvm::json::Object result;
			result[ "capabilities" ] = std::move( caps );
			llvm::json::Object serverInfo;
			serverInfo[ "name" ] = "uranite-lsp";
			serverInfo[ "version" ] = "2.0.0";
			result[ "serverInfo" ] = std::move( serverInfo );
			if( msg.id.has_value() ) {
				jsonrpc::writeResponse( this->output, *msg.id, llvm::json::Value( std::move( result ) ) );
			}

			// Module graph root from client: workspaceFolders take precedence,
			// then rootUri, then rootPath (G1: VS Code sends workspaceFolders).
			if( msg.params.getAsObject() != nullptr ) {
				const llvm::json::Object* params = msg.params.getAsObject();
				const llvm::json::Object* initOptions = params->getObject( "initializationOptions" );
				if( initOptions != nullptr ) {
					const auto modulesPath = initOptions->getString( "modulesPath" );
					if( modulesPath.has_value() && modulesPath->empty() == false ) {
						this->moduleGraph->setModulesPathOverride( modulesPath->str() );
					}
				}
				bool rootResolved = false;
				const llvm::json::Array* workspaceFolders = params->getArray( "workspaceFolders" );
				if( workspaceFolders != nullptr && workspaceFolders->empty() == false ) {
					const llvm::json::Object* primaryFolder = ( *workspaceFolders )[ 0 ].getAsObject();
					if( primaryFolder != nullptr ) {
						const auto folderUri = primaryFolder->getString( "uri" );
						if( folderUri.has_value() && folderUri->empty() == false ) {
							this->moduleGraph->setWorkspaceRoot( text::uriToPath( folderUri->str() ) );
							rootResolved = true;
						}
					}
				}
				if( rootResolved == false ) {
					const auto rootUri = params->getString( "rootUri" );
					if( rootUri.has_value() && rootUri->empty() == false ) {
						this->moduleGraph->setWorkspaceRoot( text::uriToPath( rootUri->str() ) );
					}
					else {
						const auto rootPath = params->getString( "rootPath" );
						if( rootPath.has_value() && rootPath->empty() == false ) {
							this->moduleGraph->setWorkspaceRoot( rootPath->str() );
						}
					}
				}
			}
			this->initialized = true;
			return;
		}

		if( msg.method == "initialized" ) return;

		if( msg.method == "shutdown" ) {
			this->shutdownRequested = true;
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, nullptr );
			return;
		}

		if( msg.method == "exit" ) return;

		if( this->initialized == false ) {
			if( msg.id.has_value() ) {
				jsonrpc::writeError( this->output, *msg.id,
					static_cast<int>( jsonrpc::ErrorCode::ServerNotInitialized ),
					"Server not initialized" );
			}
			return;
		}

		if( msg.method == "textDocument/didOpen" ) {
			const llvm::json::Object* params = msg.params.getAsObject();
			if( params != nullptr ) {
				const llvm::json::Object* document = params->getObject( "textDocument" );
				if( document != nullptr ) {
					const std::string uri = document->getString( "uri" ).value_or( "" ).str();
					const std::string text = document->getString( "text" ).value_or( "" ).str();
					const int version = static_cast<int>( document->getInteger( "version" ).value_or( 0 ) );
					this->documents.open( uri, text, version );
					this->overlayStore->put( text::uriToPath( uri ), text );
					this->runAnalysis( uri );
				}
			}
			return;
		}

		if( msg.method == "textDocument/didChange" ) {
			const llvm::json::Object* params = msg.params.getAsObject();
			if( params != nullptr ) {
				const llvm::json::Object* document = params->getObject( "textDocument" );
				std::string uri;
				int version = 0;
				if( document != nullptr ) {
					uri = document->getString( "uri" ).value_or( "" ).str();
					version = static_cast<int>( document->getInteger( "version" ).value_or( 0 ) );
				}
				const llvm::json::Array* changes = params->getArray( "contentChanges" );
				if( changes != nullptr && changes->empty() == false && uri.empty() == false ) {
					// G2: honor both full-text and range-based incremental change events.
					std::string text = documentTextFor( this->documents, uri );
					for( const llvm::json::Value& value : *changes ) {
						const llvm::json::Object* change = value.getAsObject();
						if( change == nullptr ) continue;
						const std::string changeText = change->getString( "text" ).value_or( "" ).str();
						const llvm::json::Object* range = change->getObject( "range" );
						if( range == nullptr ) {
							// Full-document replacement.
							text = changeText;
							continue;
						}
						text = applyRangeEdit( text, *range, changeText, toEncoding( this->positionEncoding ) );
					}
					this->documents.change( uri, text, version );
					this->overlayStore->put( text::uriToPath( uri ), text );
					this->runAnalysis( uri );
				}
			}
			return;
		}

		if( msg.method == "workspace/didChangeConfiguration" ) {
			const llvm::json::Object* params = msg.params.getAsObject();
			if( params != nullptr ) {
				const llvm::json::Object* settings = params->getObject( "settings" );
				const llvm::json::Object* uranite = ( settings != nullptr ) ? settings->getObject( "uranite" ) : params->getObject( "uranite" );
				if( uranite != nullptr ) {
					const auto modulesPath = uranite->getString( "modulesPath" );
					if( modulesPath.has_value() ) {
						if( modulesPath->empty() ) this->moduleGraph->setModulesPathOverride( "" );
						else this->moduleGraph->setModulesPathOverride( modulesPath->str() );
					}
				}
			}
			// Re-analyze open documents.
			std::vector<std::string> openUris;
			{
				std::lock_guard<std::mutex> lock( this->resultsMutex );
				for( const auto& entry : this->analysisResults ) openUris.push_back( entry.first );
			}
			for( const std::string& uri : openUris ) this->runAnalysis( uri );
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, nullptr );
			return;
		}

		if( msg.method == "textDocument/didClose" ) {
			const llvm::json::Object* params = msg.params.getAsObject();
			if( params != nullptr ) {
				const llvm::json::Object* document = params->getObject( "textDocument" );
				if( document != nullptr ) {
					const std::string uri = document->getString( "uri" ).value_or( "" ).str();
					this->scheduler.cancel( uri );
					this->documents.close( uri );
					this->overlayStore->remove( text::uriToPath( uri ) );
					{
						std::lock_guard<std::mutex> lock( this->resultsMutex );
						this->analysisResults.erase( uri );
					}
				}
			}
			return;
		}

		if( msg.method == "textDocument/documentSymbol" ) {
			const std::string uri = requestUri( msg.params );
			llvm::json::Array symbols;
			const workspace::DocumentEntry* entry = this->documents.get( uri );
			if( entry != nullptr ) {
				const std::vector<cst::CstSymbol> tagged = cst::SymbolProvider().provide(
					entry->document.text(), entry->parseSession.rootNode(), this->queries );
				symbols = cst::toDocumentSymbols( tagged );
			}
			if( msg.id.has_value() ) {
				jsonrpc::writeResponse( this->output, *msg.id, llvm::json::Value( std::move( symbols ) ) );
			}
			return;
		}

		if( msg.method == "textDocument/foldingRange" ) {
			const std::string uri = requestUri( msg.params );
			llvm::json::Array folds;
			const workspace::DocumentEntry* entry = this->documents.get( uri );
			if( entry != nullptr ) {
				for( llvm::json::Value& range : cst::FoldProvider().provide(
					entry->parseSession.rootNode(), this->queries ) ) {
					folds.push_back( std::move( range ) );
				}
			}
			if( msg.id.has_value() ) {
				jsonrpc::writeResponse( this->output, *msg.id, llvm::json::Value( std::move( folds ) ) );
			}
			return;
		}

		if( msg.method == "textDocument/semanticTokens/full" ) {
			const std::string uri = requestUri( msg.params );
			llvm::json::Array data;
			const workspace::DocumentEntry* entry = this->documents.get( uri );
			if( entry != nullptr ) {
				const std::vector<int> tokens = cst::HighlightProvider().provide(
					entry->parseSession.tree(), entry->document.text(), this->queries.highlights() );
				for( int token : tokens ) data.push_back( token );
			}
			if( msg.id.has_value() ) {
				jsonrpc::writeResponse( this->output, *msg.id,
					llvm::json::Value( llvm::json::Object{ { "data", std::move( data ) } } ) );
			}
			return;
		}

		if( msg.method == "textDocument/hover" ) {
			llvm::json::Value response = nullptr;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			if( requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				const std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service != nullptr ) {
					const semantic::QueryHover info = service->hover( line, col );
					if( info.valid ) {
						response = llvm::json::Value( llvm::json::Object{
							{ "contents", llvm::json::Object{ { "kind", "markdown" }, { "value", info.contents } } },
						} );
					}
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, response );
			return;
		}

		if( msg.method == "textDocument/definition" ) {
			llvm::json::Value response = nullptr;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			if( requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				const std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service != nullptr ) {
					const QueryLocation location = service->definition( line, col );
					if( location.valid ) response = llvm::json::Value( locationObject( location, toEncoding( this->positionEncoding ), this->documents ) );
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, response );
			return;
		}

		if( msg.method == "textDocument/references" ) {
			llvm::json::Array references;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			bool includeDeclaration = false;
			if( requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				const llvm::json::Object* object = msg.params.getAsObject();
				const llvm::json::Object* context = object == nullptr ? nullptr : object->getObject( "context" );
				if( context != nullptr ) {
					includeDeclaration = context->getBoolean( "includeDeclaration" ).value_or( false );
				}
				const std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service != nullptr ) references = locationsArray( service->references( line, col, includeDeclaration ), toEncoding( this->positionEncoding ), this->documents );
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, llvm::json::Value( std::move( references ) ) );
			return;
		}

		if( msg.method == "completionItem/resolve" ) {
			llvm::json::Value result = msg.params;
			llvm::json::Object* item = result.getAsObject();
			if( item != nullptr ) {
				const llvm::json::Object* data = item->getObject( "data" );
				std::string module;
				std::string symbol;
				std::string uri;
				if( data != nullptr ) {
					module = data->getString( "module" ).value_or( "" ).str();
					symbol = data->getString( "symbol" ).value_or( "" ).str();
					uri = data->getString( "uri" ).value_or( "" ).str();
				}
				if( symbol.empty() ) {
					if( const auto label = item->getString( "label" ) ) symbol = label->str();
				}
				if( module.empty() ) {
					if( const auto detail = item->getString( "detail" ) ) module = detail->str();
				}

				if( module.empty() == false && symbol.empty() == false && this->moduleGraph != nullptr ) {
					std::shared_ptr<semantic::QueryService> service;
					if( uri.empty() == false ) service = this->queryServiceFor( uri );
					if( service == nullptr ) {
						std::string text;
						if( uri.empty() == false ) {
							const workspace::DocumentEntry* entry = this->documents.get( uri );
							if( entry != nullptr ) text = entry->document.text();
						}
						service = std::make_shared<semantic::QueryService>( nullptr, nullptr, this->overlayStore,
							this->modulesPath, uri, text, this->moduleGraph );
					}
					const std::string canonicalDocumentation = service->entityHoverDocumentation( module, symbol );
					if( canonicalDocumentation.empty() == false ) ( *item )[ "documentation" ] = canonicalDocumentation;
					for( const auto& info : this->moduleGraph->exportedSymbolInfos( module ) ) {
						if( info.name != symbol ) continue;
						if( canonicalDocumentation.empty() ) {
							std::string doc = info.documentation;
							if( info.detail.empty() == false ) {
								std::string md = "```uranite\n" + info.detail + "\n```";
								if( doc.empty() == false ) md += "\n\n" + doc;
								doc = md;
							}
							if( doc.empty() == false ) ( *item )[ "documentation" ] = doc;
						}
						if( info.detail.empty() == false ) ( *item )[ "detail" ] = info.detail;
						break;
					}
				}

				if( module.empty() == false && symbol.empty() == false ) {
					bool hasEdits = item->getArray( "additionalTextEdits" ) != nullptr;
					if( hasEdits == false ) {
						std::string text;
						if( uri.empty() == false ) {
							const workspace::DocumentEntry* entry = this->documents.get( uri );
							if( entry != nullptr ) text = entry->document.text();
						}
						if( text.empty() == false ) {
							// AST-backed check: if symbol already imported, skip.
							bool alreadyImported = false;
							bool moduleImported = false;
							uint32_t moduleImportLine = 0;
							std::string moduleImportText;
							{
								std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
								(void) service;
							}
							std::istringstream stream( text );
							std::string line;
							uint32_t idx = 0;
							while( std::getline( stream, line ) ) {
								std::string t = trim( line );
								if( t.rfind( "from ", 0 ) == 0 ) {
									std::size_t importPos = t.find( " import" );
									if( importPos != std::string::npos ) {
										std::string mod = trim( t.substr( 5, importPos - 5 ) );
										std::string syms = t.substr( importPos + 7 );
										if( syms.find( symbol ) != std::string::npos ) alreadyImported = true;
										if( mod == module ) { moduleImported = true; moduleImportLine = idx; moduleImportText = line; }
									}
								}
								idx++;
							}
							if( alreadyImported == false ) {
								llvm::json::Array edits;
								if( moduleImported ) {
									std::string newLine = moduleImportText;
									while( newLine.empty() == false && ( newLine.back() == '\r' || newLine.back() == ' ' || newLine.back() == '\t' ) ) newLine.pop_back();
									if( newLine.find( symbol ) == std::string::npos ) {
										// G4: brace-list imports merge before the closing brace.
										const std::size_t importKeyword = newLine.find( " import " );
										const std::size_t closeBrace = newLine.rfind( '}' );
										if( importKeyword != std::string::npos && closeBrace != std::string::npos &&
										    closeBrace > importKeyword &&
										    newLine.compare( importKeyword + 8, 1, "{" ) == 0 ) {
											newLine = newLine.substr( 0, closeBrace ) + ", " + symbol + newLine.substr( closeBrace );
										}
										else {
											newLine += ", " + symbol;
										}
									}
									edits.push_back( llvm::json::Object{
										{ "range", llvm::json::Object{
											{ "start", llvm::json::Object{ { "line", moduleImportLine }, { "character", 0 } } },
											{ "end", llvm::json::Object{ { "line", moduleImportLine }, { "character", static_cast<uint32_t>( moduleImportText.size() ) } } } } },
										{ "newText", newLine } } );
								}
								else {
									// insert after package and leading doccomments
									uint32_t insertLine = 0;
									std::istringstream s2( text );
									std::string l;
									uint32_t li = 0;
									bool inBlock = false;
									bool seenPackage = false;
									while( std::getline( s2, l ) ) {
										std::string t = trim( l );
										if( inBlock ) { if( t.find( "*/" ) != std::string::npos ) inBlock = false; li++; if( seenPackage ) { insertLine = li; break; } continue; }
										if( t.rfind( "/*", 0 ) == 0 ) { if( t.find( "*/" ) == std::string::npos ) inBlock = true; li++; if( seenPackage ) { insertLine = li; break; } continue; }
										if( t.empty() || t.rfind( "#", 0 ) == 0 || t.rfind( "//", 0 ) == 0 ) { li++; if( seenPackage ) insertLine = li; continue; }
										if( t.rfind( "package ", 0 ) == 0 ) { seenPackage = true; insertLine = li + 1; li++; continue; }
										if( seenPackage ) { insertLine = li; break; }
										break;
									}
									(void) seenPackage;
									edits.push_back( llvm::json::Object{
										{ "range", llvm::json::Object{
											{ "start", llvm::json::Object{ { "line", insertLine }, { "character", 0 } } },
											{ "end", llvm::json::Object{ { "line", insertLine }, { "character", 0 } } } } },
										{ "newText", "from " + module + " import " + symbol + "\n" } } );
								}
								if( edits.empty() == false ) ( *item )[ "additionalTextEdits" ] = std::move( edits );
							}
						}
					}
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, result );
			return;
		}

		if( msg.method == "textDocument/completion" ) {
			llvm::json::Array items;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			if( requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service == nullptr ) {
					// Mid-typing buffers whose parse failed still deserve import,
					// keyword and auto-import completions: answer through a
					// lightweight service that never touches the symbol index.
					std::string text;
					const workspace::DocumentEntry* entry = this->documents.get( uri );
					if( entry != nullptr ) text = entry->document.text();
					if( text.empty() == false ) {
						service = std::make_shared<semantic::QueryService>( nullptr, nullptr, this->overlayStore,
							this->modulesPath, uri, text, this->moduleGraph );
					}
				}
				if( service != nullptr ) {
					for( const semantic::QueryCompletion& item : service->completion( line, col ) ) {
						if( item.valid == false ) continue;
						llvm::json::Object object;
						object[ "label" ] = item.label;
						object[ "kind" ] = item.kind;
						object[ "detail" ] = item.detail;
						if( item.documentation.empty() == false ) object[ "documentation" ] = item.documentation;
						object[ "insertText" ] = item.insertText;
						object[ "insertTextFormat" ] = item.insertTextFormat;
						object[ "sortText" ] = fmt::format( "{:04d}", item.sortPriority );
						if( item.resolvable ) {
							object[ "data" ] = llvm::json::Object{
								{ "module", item.dataModule },
								{ "symbol", item.dataSymbol },
								{ "uri", item.dataUri },
							};
						}
						if( item.additionalTextEdits.empty() == false ) {
							llvm::json::Array edits;
							for( const semantic::QueryCompletion::TextEdit& edit : item.additionalTextEdits ) {
								edits.push_back( llvm::json::Object{
									{ "range", llvm::json::Object{
										{ "start", llvm::json::Object{ { "line", edit.startLine }, { "character", edit.startCol } } },
										{ "end", llvm::json::Object{ { "line", edit.endLine }, { "character", edit.endCol } } } } },
									{ "newText", edit.newText } } );
							}
							object[ "additionalTextEdits" ] = std::move( edits );
						}
						items.push_back( std::move( object ) );
					}
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, llvm::json::Value( std::move( items ) ) );
			return;
		}

		if( msg.method == "textDocument/signatureHelp" ) {
			llvm::json::Value response = nullptr;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			if( requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				const std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service != nullptr ) {
					const semantic::QuerySignature info = service->signatureHelp( line, col );
					if( info.valid ) {
						llvm::json::Array parameters;
						llvm::json::Object signature;
						signature[ "label" ] = info.label;
						signature[ "documentation" ] = info.documentation;
						signature[ "parameters" ] = std::move( parameters );
						response = llvm::json::Value( llvm::json::Object{
							{ "signatures", llvm::json::Array{ std::move( signature ) } },
							{ "activeSignature", 0 },
							{ "activeParameter", info.activeParameter },
						} );
					}
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, response );
			return;
		}

		if( msg.method == "textDocument/prepareRename" ) {
			llvm::json::Value response = nullptr;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			if( requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				const std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service != nullptr ) {
					const QueryLocation location = service->prepareRename( line, col );
					if( location.valid ) {
						response = llvm::json::Value( llvm::json::Object{ { "range", rangeObject( location, toEncoding( this->positionEncoding ), this->documents ) } } );
					}
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, response );
			return;
		}

		if( msg.method == "textDocument/rename" ) {
			llvm::json::Value response = nullptr;
			std::string uri;
			uint32_t line = 0;
			uint32_t col = 0;
			const llvm::json::Object* object = msg.params.getAsObject();
			const std::string newName = object == nullptr ? "" : object->getString( "newName" ).value_or( "" ).str();
			if( newName.empty() == false && requestPosition( msg.params, uri, line, col, toEncoding( this->positionEncoding ), this->documents ) ) {
				const std::shared_ptr<semantic::QueryService> service = this->queryServiceFor( uri );
				if( service != nullptr ) {
					std::map<std::string, llvm::json::Array> grouped;
					for( const QueryLocation& location : service->rename( line, col ) ) {
						if( location.valid == false ) continue;
						grouped[ toDocumentUri( location.path ) ].push_back( llvm::json::Object{
							{ "range", rangeObject( location, toEncoding( this->positionEncoding ), this->documents ) },
							{ "newText", newName },
						} );
					}
					llvm::json::Object changes;
					for( auto& entry : grouped ) changes[ entry.first ] = llvm::json::Value( std::move( entry.second ) );
					response = llvm::json::Value( llvm::json::Object{ { "changes", std::move( changes ) } } );
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, response );
			return;
		}

		if( msg.method == "workspace/symbol" ) {
			llvm::json::Array symbols;
			const llvm::json::Object* object = msg.params.getAsObject();
			const std::string query = object == nullptr ? "" : object->getString( "query" ).value_or( "" ).str();
			std::vector<std::shared_ptr<semantic::FrontendResult>> results;
			{
				std::lock_guard<std::mutex> lock( this->resultsMutex );
				for( const auto& entry : this->analysisResults ) results.push_back( entry.second );
			}
			for( const std::shared_ptr<semantic::FrontendResult>& result : results ) {
				if( result == nullptr || result->symbolIndex == nullptr ) continue;
				const semantic::QueryService service( result, result->symbolIndex );
				for( const semantic::QueryWorkspaceSymbol& symbol : service.workspaceSymbols( query ) ) {
					if( symbol.location.valid == false ) continue;
					symbols.push_back( llvm::json::Object{
						{ "name", symbol.name },
						{ "location", locationObject( symbol.location, toEncoding( this->positionEncoding ), this->documents ) },
					} );
				}
			}
			if( msg.id.has_value() ) jsonrpc::writeResponse( this->output, *msg.id, llvm::json::Value( std::move( symbols ) ) );
			return;
		}

		if( msg.id.has_value() ) {
			jsonrpc::writeError( this->output, *msg.id,
				static_cast<int>( jsonrpc::ErrorCode::MethodNotFound ), "Method not found: " + msg.method );
		}
	}

} // namespace uranite::lsp::server
