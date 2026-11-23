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

#ifndef _URANITE_LSP_CST_SYMBOL_PROVIDER_HPP_
#define _URANITE_LSP_CST_SYMBOL_PROVIDER_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include <llvm/Support/JSON.h>
#include <tree_sitter/api.h>

#include "uranite-lsp/cst/query-set.hpp"

namespace uranite::lsp::cst {

	enum class SymbolRole : uint8_t {
		Definition,
		Reference
	};

	/** @brief A name occurrence extracted from a compiled tags.scm match. */
	struct CstSymbol {
		std::string name;
		std::string capture;
		int lspKind = 0;
		SymbolRole role = SymbolRole::Definition;
		uint32_t startByte = 0;
		uint32_t endByte = 0;
		uint32_t startLine = 0;
		uint32_t startCharacter = 0;
		uint32_t endLine = 0;
		uint32_t endCharacter = 0;
		uint32_t nameStartLine = 0;
		uint32_t nameStartCharacter = 0;
		uint32_t nameEndLine = 0;
		uint32_t nameEndCharacter = 0;
	};

	/** @brief Extracts definitions and references using tags.scm captures only. */
	class SymbolProvider {
		public:
			std::vector<CstSymbol> provide( const std::string& source, TSNode root, const QuerySet& queries ) const;
	};

	/** @brief Builds a hierarchical LSP DocumentSymbol[] from tagged definitions. */
	llvm::json::Array toDocumentSymbols( const std::vector<CstSymbol>& symbols );

} // namespace uranite::lsp::cst

#endif // end _URANITE_LSP_CST_SYMBOL_PROVIDER_HPP_
