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

#ifndef _URANITE_LSP_PROTOCOL_CONVERT_HPP_
#define _URANITE_LSP_PROTOCOL_CONVERT_HPP_

#include <string>

#include "uranite/lookup/source.hpp"

#include "uranite-lsp/text/position.hpp"

namespace uranite::lsp::protocol {

	// Convert 1-based compiler Source location to 0-based LSP Position fields.
	void sourceToLspRange( const uranite::lookup::SourceSharedPointer& src, int& startLine, int& startCol, int& endLine, int& endCol,
	                       text::PositionEncoding encoding = text::PositionEncoding::Utf16,
	                       const std::string& documentText = "" );

} // namespace uranite::lsp::protocol

#endif // end _URANITE_LSP_PROTOCOL_CONVERT_HPP_
