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

#include "uranite-lsp/protocol/convert.hpp"

#include <vector>

#include "uranite-lsp/text/position.hpp"

namespace uranite::lsp::protocol {

	namespace {

		std::string lineAt( const std::string& text, int line ) {
			if( line < 0 ) return "";
			int current = 0;
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

	} // namespace

	void sourceToLspRange( const uranite::lookup::SourceSharedPointer& src, int& startLine, int& startCol, int& endLine, int& endCol,
	                       text::PositionEncoding encoding, const std::string& documentText ) {
		startLine = 0; startCol = 0; endLine = 0; endCol = 0;
		if( !src ) return;
		if( src->location ) {
			startLine = src->location->line > 0 ? static_cast<int>( src->location->line - 1 ) : 0;
			uint32_t byteCol = src->location->column > 0 ? static_cast<uint32_t>( src->location->column - 1 ) : 0;
			startCol = static_cast<int>( text::toLspCharacter( lineAt( documentText, startLine ), byteCol, encoding ) );
		}
		if( src->range ) {
			endLine = src->range->end->line > 0 ? static_cast<int>( src->range->end->line - 1 ) : startLine;
			uint32_t byteCol = src->range->end->column > 0 ? static_cast<uint32_t>( src->range->end->column - 1 ) : 0;
			endCol = static_cast<int>( text::toLspCharacter( lineAt( documentText, endLine ), byteCol, encoding ) );
		}
		else {
			endLine = startLine;
			endCol = startCol;
		}
	}

} // namespace uranite::lsp::protocol
