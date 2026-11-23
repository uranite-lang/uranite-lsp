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

#ifndef _URANITE_LSP_CST_PARSE_SESSION_HPP_
#define _URANITE_LSP_CST_PARSE_SESSION_HPP_

#include <string>
#include <tree_sitter/api.h>

#include "uranite-lsp/text/position.hpp"

namespace uranite::lsp::cst {

	class ParseSession {
		public:
			ParseSession();
			~ParseSession();

			void parse( const std::string& text );
			void edit( const text::Range& range, const std::string& oldText, const std::string& newText );

			const TSTree* tree() const { return this->parsedTree; }
			TSNode rootNode() const;

		private:
			TSParser* parser;
			TSTree* parsedTree;
	};

} // namespace uranite::lsp::cst

#endif // end _URANITE_LSP_CST_PARSE_SESSION_HPP_
