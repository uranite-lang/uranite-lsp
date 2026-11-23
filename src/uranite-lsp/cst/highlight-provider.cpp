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

#include "uranite-lsp/cst/highlight-provider.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <tree_sitter/api.h>

namespace uranite::lsp::cst {

	int HighlightProvider::captureToTokenType( const std::string& name ) const {
		if( name.find( "keyword" ) == 0 ) return 13;
		if( name == "type.builtin" || name == "type" || name == "type.definition" ) return 1;
		if( name == "type.parameter" ) return 6;
		if( name == "variable.builtin" || name == "variable" ) return 8;
		if( name == "variable.parameter" ) return 7;
		if( name == "constant.builtin" || name == "constant" || name == "enumMember" ) return 10;
		if( name == "function" || name == "function.call" ) return 11;
		if( name == "function.method.call" ) return 12;
		if( name == "property" ) return 9;
		if( name == "module" ) return 0;
		if( name == "number" || name == "number.float" ) return 15;
		if( name == "string" || name == "string.special" || name == "character" ) return 14;
		if( name == "comment" || name.find( "comment." ) == 0 ) return 17;
		if( name == "operator" || name == "keyword.operator" ) return 16;
		return -1; // skip (punctuation etc.)
	}

	struct Token {
		uint32_t line;
		uint32_t col;
		uint32_t length;
		int type;
	};

	std::vector<int> HighlightProvider::provide( const TSTree* tree, const std::string& source, TSQuery* query ) {
		std::vector<int> data;
		if( tree == nullptr || query == nullptr ) return data;

		TSQueryCursor* cursor = ts_query_cursor_new();
		TSNode root = ts_tree_root_node( tree );
		ts_query_cursor_exec( cursor, query, root );

		// Collect captures
		std::vector<Token> tokens;
		TSQueryMatch match;
		while( ts_query_cursor_next_match( cursor, &match ) ) {
			for( uint32_t i = 0; i < match.capture_count; i++ ) {
				TSQueryCapture cap = match.captures[ i ];
				uint32_t nameLen = 0;
				const char* namePtr = ts_query_capture_name_for_id( query, cap.index, &nameLen );
				std::string name( namePtr, nameLen );
				int type = this->captureToTokenType( name );
				if( type < 0 ) continue;
				TSNode node = cap.node;
				TSPoint sp = ts_node_start_point( node );
				TSPoint ep = ts_node_end_point( node );
				uint32_t length = 0;
				if( sp.row == ep.row ) {
					length = ep.column - sp.column;
				}
				else {
					// multi-line: use first line length
					const char* s = source.c_str() + ts_node_start_byte( node );
					const char* nl = static_cast<const char*>( std::memchr( s, '\n', ts_node_end_byte( node ) - ts_node_start_byte( node ) ) );
					length = nl ? static_cast<uint32_t>( nl - s ) : ( ts_node_end_byte( node ) - ts_node_start_byte( node ) );
				}
				if( length == 0 ) continue;
				tokens.push_back( { sp.row, sp.column, length, type } );
			}
		}
		ts_query_cursor_delete( cursor );

		// Dedup overlapping tokens by position+length (keep first in query order approx)
		std::sort( tokens.begin(), tokens.end(), []( const Token& a, const Token& b ) {
			if( a.line != b.line ) return a.line < b.line;
			if( a.col != b.col ) return a.col < b.col;
			return a.length > b.length;
		} );
		std::vector<Token> deduped;
		for( auto& t : tokens ) {
			bool overlap = false;
			for( auto& d : deduped ) {
				if( d.line == t.line && t.col < d.col + d.length && d.col < t.col + t.length ) {
					overlap = true;
					break;
				}
			}
			if( !overlap ) deduped.push_back( t );
		}
		std::sort( deduped.begin(), deduped.end(), []( const Token& a, const Token& b ) {
			if( a.line != b.line ) return a.line < b.line;
			return a.col < b.col;
		} );

		// Delta encode
		uint32_t prevLine = 0;
		uint32_t prevCol = 0;
		bool first = true;
		for( auto& t : deduped ) {
			uint32_t lineDelta = first ? t.line : ( t.line - prevLine );
			uint32_t colDelta = 0;
			if( first || t.line != prevLine ) {
				colDelta = t.col;
			}
			else {
				colDelta = t.col - prevCol;
			}
			data.push_back( static_cast<int>( lineDelta ) );
			data.push_back( static_cast<int>( colDelta ) );
			data.push_back( static_cast<int>( t.length ) );
			data.push_back( t.type );
			data.push_back( 0 ); // tokenModifiers
			prevLine = t.line;
			prevCol = t.col;
			first = false;
		}
		return data;
	}

} // namespace uranite::lsp::cst
