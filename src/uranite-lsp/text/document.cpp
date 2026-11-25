
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

#include <string>
#include <vector>

#include "uranite-lsp/text/document.hpp"

namespace uranite::lsp::text {

	bool looksParseable( const std::string& text ) {
		int parens = 0;
		int brackets = 0;
		int braces = 0;
		std::size_t i = 0;
		while( i < text.size() ) {
			const char c = text[ i ];
			if( c == '#' ) {
				if( text.compare( i, 2, "#{" ) == 0 ) {
					const std::size_t close = text.find( "}#", i + 2 );
					if( close == std::string::npos ) return true;
					i = close + 2;
					continue;
				}
				while( i < text.size() && text[ i ] != '\n' ) i++;
				continue;
			}
			if( c == '"' ) {
				if( text.compare( i, 3, "\"\"\"" ) == 0 ) {
					const std::size_t close = text.find( "\"\"\"", i + 3 );
					if( close == std::string::npos ) return true;
					i = close + 3;
					continue;
				}
				i++;
				while( i < text.size() ) {
					if( text[ i ] == '\\' ) { i += 2; continue; }
					if( text[ i ] == '"' ) { i++; break; }
					if( text[ i ] == '\n' ) return false;
					i++;
				}
				continue;
			}
			if( c == '\'' ) {
				i++;
				while( i < text.size() ) {
					if( text[ i ] == '\\' ) { i += 2; continue; }
					if( text[ i ] == '\'' ) { i++; break; }
					if( text[ i ] == '\n' ) return false;
					i++;
				}
				continue;
			}
			if( c == '(' ) parens++;
			else if( c == ')' ) parens--;
			else if( c == '[' ) brackets++;
			else if( c == ']' ) brackets--;
			else if( c == '{' ) braces++;
			else if( c == '}' ) braces--;
			if( parens < 0 || brackets < 0 || braces < 0 ) return false;
			i++;
		}
		return parens == 0 && brackets == 0 && braces == 0;
	}

	Document::Document() : documentUri( "" ), content( "" ), documentVersion( 0 ) {
	}

	Document::Document( const std::string& uri, const std::string& text, int version )
		: documentUri( uri ), content( text ), documentVersion( version ) {
	}

	void Document::update( const std::string& fullText, int version ) {
		this->content = fullText;
		this->documentVersion = version;
	}

	std::vector<std::string> Document::lines() const {
		std::vector<std::string> result;
		std::string current;
		for( char c : this->content ) {
			if( c == '\n' ) {
				result.push_back( current );
				current.clear();
			}
			else {
				current.push_back( c );
			}
		}
		result.push_back( current );
		return result;
	}

	void Document::applyChange( const Range& range, const std::string& newText ) {
		std::vector<std::string> ls = this->lines();
		// convert range to byte offsets in content
		// simple: rebuild using line offsets
		// compute start byte offset
		std::size_t startOffset = 0;
		for( uint32_t i = 0; i < range.start.line && i < ls.size(); i++ ) {
			startOffset += ls[ i ].size() + 1; // +1 for \n
		}
		if( range.start.line < ls.size() ) {
			startOffset += utf16ToByteOffset( ls[ range.start.line ], range.start.character );
		}
		std::size_t endOffset = 0;
		for( uint32_t i = 0; i < range.end.line && i < ls.size(); i++ ) {
			endOffset += ls[ i ].size() + 1;
		}
		if( range.end.line < ls.size() ) {
			endOffset += utf16ToByteOffset( ls[ range.end.line ], range.end.character );
		}
		if( startOffset > this->content.size() ) startOffset = this->content.size();
		if( endOffset > this->content.size() ) endOffset = this->content.size();
		if( startOffset > endOffset ) startOffset = endOffset;
		this->content.replace( startOffset, endOffset - startOffset, newText );
	}

} // namespace uranite::lsp::text
