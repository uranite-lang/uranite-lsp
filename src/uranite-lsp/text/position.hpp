
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

#ifndef _URANITE_LSP_TEXT_POSITION_HPP_
#define _URANITE_LSP_TEXT_POSITION_HPP_

#include <cstdint>
#include <string>

namespace uranite::lsp::text {

	struct Position {
		uint32_t line = 0;
		uint32_t character = 0;
	};

	struct Range {
		Position start;
		Position end;
	};

	enum class PositionEncoding {
		Utf8,
		Utf16,
	};

	/** @brief Converts a UTF-8 byte offset to an LSP (UTF-16) character offset on a line. */
	uint32_t byteOffsetToUtf16( const std::string& lineText, uint32_t byteOffset );

	/** @brief Converts an LSP (UTF-16) character offset to a UTF-8 byte offset on a line. */
	uint32_t utf16ToByteOffset( const std::string& lineText, uint32_t utf16Offset );

	/** @brief Converts a UTF-8 byte offset to an LSP character offset for the negotiated encoding. */
	uint32_t toLspCharacter( const std::string& lineText, uint32_t byteOffset, PositionEncoding encoding );

	/** @brief Converts an LSP character offset for the negotiated encoding to a UTF-8 byte offset. */
	uint32_t toByteOffset( const std::string& lineText, uint32_t character, PositionEncoding encoding );

} // namespace uranite::lsp::text

#endif // end _URANITE_LSP_TEXT_POSITION_HPP_
