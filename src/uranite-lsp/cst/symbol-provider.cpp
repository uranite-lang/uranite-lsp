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

#include "uranite-lsp/cst/symbol-provider.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <llvm/Support/JSON.h>
#include <tree_sitter/api.h>

namespace uranite::lsp::cst {

	namespace {

		struct KindMapping {
			const char* capture;
			int kind;
		};

		// LSP SymbolKind values. Only tags.scm capture names appear here: no
		// grammar node names and no source-text matching are involved.
		static constexpr KindMapping kindMappings[] = {
			{ "definition.function", 12 },
			{ "definition.class", 5 },
			{ "definition.interface", 11 },
			{ "definition.type", 23 },
			{ "definition.variable", 13 },
			{ "definition.constant", 14 },
			{ "reference.call", 12 },
			{ "reference.class", 5 },
		};

		int lspKindForCapture( const std::string& capture ) {
			for( const KindMapping& mapping : kindMappings ) {
				if( capture == mapping.capture ) return mapping.kind;
			}
			return 0;
		}

		bool isDefinitionCapture( const std::string& capture ) {
			return capture.compare( 0, 11, "definition." ) == 0;
		}

		bool isReferenceCapture( const std::string& capture ) {
			return capture.compare( 0, 10, "reference." ) == 0;
		}

		std::string nodeText( const std::string& source, TSNode node ) {
			const uint32_t start = ts_node_start_byte( node );
			const uint32_t end = ts_node_end_byte( node );
			if( start >= end || end > source.size() ) return "";
			return source.substr( start, end - start );
		}

		bool symbolLess( const CstSymbol& left, const CstSymbol& right ) {
			return std::tie( left.startByte, left.endByte, left.capture, left.name ) <
			       std::tie( right.startByte, right.endByte, right.capture, right.name );
		}

		llvm::json::Object positionObject( uint32_t line, uint32_t character ) {
			llvm::json::Object object;
			object[ "line" ] = line;
			object[ "character" ] = character;
			return object;
		}

		llvm::json::Object rangeObject( const CstSymbol& symbol ) {
			llvm::json::Object object;
			object[ "start" ] = positionObject( symbol.startLine, symbol.startCharacter );
			object[ "end" ] = positionObject( symbol.endLine, symbol.endCharacter );
			return object;
		}

		llvm::json::Object selectionRangeObject( const CstSymbol& symbol ) {
			llvm::json::Object object;
			object[ "start" ] = positionObject( symbol.nameStartLine, symbol.nameStartCharacter );
			object[ "end" ] = positionObject( symbol.nameEndLine, symbol.nameEndCharacter );
			return object;
		}

		llvm::json::Value buildSymbol( std::size_t index,
			const std::vector<CstSymbol>& symbols,
			const std::vector<std::vector<std::size_t>>& children ) {
			const CstSymbol& symbol = symbols[ index ];
			llvm::json::Object object;
			object[ "name" ] = symbol.name;
			object[ "kind" ] = symbol.lspKind;
			object[ "range" ] = rangeObject( symbol );
			object[ "selectionRange" ] = selectionRangeObject( symbol );

			llvm::json::Array nested;
			for( std::size_t child : children[ index ] ) {
				nested.push_back( buildSymbol( child, symbols, children ) );
			}
			if( nested.empty() == false ) object[ "children" ] = std::move( nested );
			return llvm::json::Value( std::move( object ) );
		}

	} // namespace

	std::vector<CstSymbol> SymbolProvider::provide( const std::string& source, TSNode root, const QuerySet& queries ) const {
		std::vector<CstSymbol> result;
		TSQuery* query = queries.tags();
		if( query == nullptr || ts_node_is_null( root ) ) return result;

		TSQueryCursor* cursor = ts_query_cursor_new();
		ts_query_cursor_exec( cursor, query, root );

		TSQueryMatch match;
		while( ts_query_cursor_next_match( cursor, &match ) ) {
			TSNode nameNode;
			TSNode ownerNode;
			std::string ownerCapture;
			bool hasName = false;
			bool hasOwner = false;

			for( uint32_t i = 0; i < match.capture_count; i++ ) {
				const TSQueryCapture capture = match.captures[ i ];
				const std::string captureName = queries.captureName( query, capture.index );
				if( captureName == "name" ) {
					nameNode = capture.node;
					hasName = true;
				}
				else if( isDefinitionCapture( captureName ) || isReferenceCapture( captureName ) ) {
					ownerNode = capture.node;
					ownerCapture = captureName;
					hasOwner = true;
				}
			}

			if( hasName == false || hasOwner == false ) continue;
			if( ts_node_is_null( nameNode ) || ts_node_is_null( ownerNode ) ) continue;

			CstSymbol symbol;
			symbol.name = nodeText( source, nameNode );
			if( symbol.name.empty() ) continue;
			symbol.capture = ownerCapture;
			symbol.lspKind = lspKindForCapture( ownerCapture );
			symbol.role = isDefinitionCapture( ownerCapture ) ? SymbolRole::Definition : SymbolRole::Reference;
			symbol.startByte = ts_node_start_byte( ownerNode );
			symbol.endByte = ts_node_end_byte( ownerNode );

			const TSPoint ownerStart = ts_node_start_point( ownerNode );
			const TSPoint ownerEnd = ts_node_end_point( ownerNode );
			symbol.startLine = ownerStart.row;
			symbol.startCharacter = ownerStart.column;
			symbol.endLine = ownerEnd.row;
			symbol.endCharacter = ownerEnd.column;

			const TSPoint nameStart = ts_node_start_point( nameNode );
			const TSPoint nameEnd = ts_node_end_point( nameNode );
			symbol.nameStartLine = nameStart.row;
			symbol.nameStartCharacter = nameStart.column;
			symbol.nameEndLine = nameEnd.row;
			symbol.nameEndCharacter = nameEnd.column;

			result.push_back( std::move( symbol ) );
		}
		ts_query_cursor_delete( cursor );

		std::sort( result.begin(), result.end(), symbolLess );
		result.erase( std::unique( result.begin(), result.end(),
			[]( const CstSymbol& left, const CstSymbol& right ) {
				return left.startByte == right.startByte && left.endByte == right.endByte &&
				       left.capture == right.capture && left.name == right.name;
			} ), result.end() );
		return result;
	}

	llvm::json::Array toDocumentSymbols( const std::vector<CstSymbol>& symbols ) {
		llvm::json::Array output;
		const std::size_t count = symbols.size();
		if( count == 0 ) return output;

		const std::size_t none = std::numeric_limits<std::size_t>::max();
		std::vector<std::size_t> parent( count, none );
		std::vector<std::vector<std::size_t>> children( count );

		for( std::size_t i = 0; i < count; i++ ) {
			if( symbols[ i ].role != SymbolRole::Definition ) continue;
			for( std::size_t j = 0; j < count; j++ ) {
				if( i == j || symbols[ j ].role != SymbolRole::Definition ) continue;
				if( symbols[ j ].startByte >= symbols[ i ].startByte ) continue;
				// A trailing nested definition shares its parent's end byte.
				if( symbols[ j ].endByte < symbols[ i ].endByte ) continue;
				if( parent[ i ] != none && symbols[ j ].startByte <= symbols[ parent[ i ] ].startByte ) continue;
				parent[ i ] = j;
			}
		}

		for( std::size_t i = 0; i < count; i++ ) {
			if( symbols[ i ].role != SymbolRole::Definition ) continue;
			if( parent[ i ] != none ) children[ parent[ i ] ].push_back( i );
		}

		for( std::size_t i = 0; i < count; i++ ) {
			if( symbols[ i ].role != SymbolRole::Definition ) continue;
			if( parent[ i ] == none ) output.push_back( buildSymbol( i, symbols, children ) );
		}
		return output;
	}

} // namespace uranite::lsp::cst
