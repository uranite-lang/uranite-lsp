
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

#include <llvm/Support/JSON.h>
#include <set>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

#include "uranite-lsp/cst/fold-provider.hpp"

namespace uranite::lsp::cst {

	std::vector<llvm::json::Value> FoldProvider::provide( TSNode root, const QuerySet& queries ) const {
		std::vector<llvm::json::Value> output;
		TSQuery* query = queries.folds();
		if( query == nullptr || ts_node_is_null( root ) ) return output;

		TSQueryCursor* cursor = ts_query_cursor_new();
		ts_query_cursor_exec( cursor, query, root );

		std::set<std::pair<uint32_t, uint32_t>> seen;
		TSQueryMatch match;
		while( ts_query_cursor_next_match( cursor, &match ) ) {
			for( uint32_t i = 0; i < match.capture_count; i++ ) {
				const TSQueryCapture capture = match.captures[ i ];
				if( queries.captureName( query, capture.index ) != "fold" ) continue;

				const TSNode node = capture.node;
				const TSPoint start = ts_node_start_point( node );
				const TSPoint end = ts_node_end_point( node );
				if( end.row <= start.row ) continue;

				const uint32_t startByte = ts_node_start_byte( node );
				const uint32_t endByte = ts_node_end_byte( node );
				if( seen.insert( std::make_pair( startByte, endByte ) ).second == false ) continue;

				llvm::json::Object range;
				range[ "startLine" ] = start.row;
				range[ "startCharacter" ] = start.column;
				range[ "endLine" ] = end.row;
				range[ "endCharacter" ] = end.column;
				output.push_back( llvm::json::Value( std::move( range ) ) );
			}
		}
		ts_query_cursor_delete( cursor );
		return output;
	}

} // namespace uranite::lsp::cst
