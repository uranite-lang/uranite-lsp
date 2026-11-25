
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

#include <tree_sitter/api.h>
#include <vector>

#include "uranite-lsp/cst/language.hpp"
#include "uranite-lsp/cst/parse-session.hpp"

namespace uranite::lsp::cst {

	ParseSession::ParseSession() {
		this->parser = ts_parser_new();
		ts_parser_set_language( this->parser, language() );
		this->parsedTree = nullptr;
	}

	ParseSession::~ParseSession() {
		if( this->parsedTree != nullptr ) {
			ts_tree_delete( this->parsedTree );
		}
		ts_parser_delete( this->parser );
	}

	void ParseSession::parse( const std::string& text ) {
		if( this->parsedTree != nullptr ) {
			ts_tree_delete( this->parsedTree );
			this->parsedTree = nullptr;
		}
		// tree-sitter 0.22 uses ts_parser_parse_string
		this->parsedTree = ts_parser_parse_string( this->parser, nullptr, text.c_str(), static_cast<uint32_t>( text.size() ) );
	}

	void ParseSession::edit( const text::Range& range, const std::string& oldText, const std::string& newText ) {
		if( this->parsedTree == nullptr ) {
			return;
		}
		TSInputEdit inputEdit;
		// convert range (UTF-16 positions) to byte offsets approximately using utf16ToByteOffset on the old text line
		// For simplicity, treat character offsets as byte offsets (UTF-8 negotiated). Here we assume UTF-8 positions.
		uint32_t startByte = 0, oldEndByte = 0;
		// compute start byte by walking lines of oldText
		{
			std::vector<std::string> ls;
			std::string cur;
			for( char c : oldText ) { if( c == '\n' ) { ls.push_back( cur ); cur.clear(); } else cur.push_back( c ); }
			ls.push_back( cur );
			for( uint32_t i = 0; i < range.start.line && i < ls.size(); i++ ) startByte += static_cast<uint32_t>( ls[ i ].size() ) + 1;
			startByte += range.start.character;
		}
		{
			std::vector<std::string> ls;
			std::string cur;
			for( char c : oldText ) { if( c == '\n' ) { ls.push_back( cur ); cur.clear(); } else cur.push_back( c ); }
			ls.push_back( cur );
			for( uint32_t i = 0; i < range.end.line && i < ls.size(); i++ ) oldEndByte += static_cast<uint32_t>( ls[ i ].size() ) + 1;
			oldEndByte += range.end.character;
		}
		inputEdit.start_byte = startByte;
		inputEdit.old_end_byte = oldEndByte;
		inputEdit.new_end_byte = startByte + static_cast<uint32_t>( newText.size() );
		inputEdit.start_point = { range.start.line, range.start.character };
		inputEdit.old_end_point = { range.end.line, range.end.character };
		inputEdit.new_end_point = { range.start.line, range.start.character + static_cast<uint32_t>( newText.size() ) };
		ts_tree_edit( this->parsedTree, &inputEdit );
	}

	TSNode ParseSession::rootNode() const {
		if( this->parsedTree == nullptr ) {
			return TSNode{};
		}
		return ts_tree_root_node( this->parsedTree );
	}

} // namespace uranite::lsp::cst
