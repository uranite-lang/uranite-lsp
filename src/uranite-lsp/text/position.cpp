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

#include "uranite-lsp/text/position.hpp"

#include <string>

namespace uranite::lsp::text {

	uint32_t byteOffsetToUtf16( const std::string& lineText, uint32_t byteOffset ) {
		uint32_t utf16 = 0;
		uint32_t i = 0;
		while( i < byteOffset && i < lineText.size() ) {
			unsigned char c = static_cast<unsigned char>( lineText[ i ] );
			if( c < 0x80 ) {
				i += 1;
				utf16 += 1;
			}
			else if( ( c & 0xE0 ) == 0xC0 ) {
				i += 2;
				utf16 += 1;
			}
			else if( ( c & 0xF0 ) == 0xE0 ) {
				i += 3;
				utf16 += 1;
			}
			else if( ( c & 0xF8 ) == 0xF0 ) {
				i += 4;
				utf16 += 2;
			}
			else {
				i += 1;
				utf16 += 1;
			}
		}
		return utf16;
	}

	uint32_t utf16ToByteOffset( const std::string& lineText, uint32_t utf16Offset ) {
		uint32_t utf16 = 0;
		uint32_t i = 0;
		while( i < lineText.size() && utf16 < utf16Offset ) {
			unsigned char c = static_cast<unsigned char>( lineText[ i ] );
			if( c < 0x80 ) {
				i += 1;
				utf16 += 1;
			}
			else if( ( c & 0xE0 ) == 0xC0 ) {
				i += 2;
				utf16 += 1;
			}
			else if( ( c & 0xF0 ) == 0xE0 ) {
				i += 3;
				utf16 += 1;
			}
			else if( ( c & 0xF8 ) == 0xF0 ) {
				i += 4;
				utf16 += 2;
			}
			else {
				i += 1;
				utf16 += 1;
			}
		}
		return i;
	}

	uint32_t toLspCharacter( const std::string& lineText, uint32_t byteOffset, PositionEncoding encoding ) {
		if( encoding == PositionEncoding::Utf8 ) return byteOffset;
		if( lineText.empty() ) return byteOffset;
		return byteOffsetToUtf16( lineText, byteOffset );
	}

	uint32_t toByteOffset( const std::string& lineText, uint32_t character, PositionEncoding encoding ) {
		if( encoding == PositionEncoding::Utf8 ) return character;
		if( lineText.empty() ) return character;
		return utf16ToByteOffset( lineText, character );
	}

} // namespace uranite::lsp::text
