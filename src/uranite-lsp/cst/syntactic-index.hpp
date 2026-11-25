
//
// @author hxAri (hxari)
// @create 2026-11-23 00:00
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

#ifndef _URANITE_LSP_CST_SYNTACTIC_INDEX_HPP_
#define _URANITE_LSP_CST_SYNTACTIC_INDEX_HPP_

#include <string>
#include <tree_sitter/api.h>

namespace uranite::lsp::cst {

	enum class CompletionContextKind {
		Unknown,
		Identifier,
		MemberAccess,
		ImportPath,
		ImportSymbol,
		TypeAnnotation,
		Keyword,
		CallArgument,
		StructInit,
		AsmBlock,
	};

	struct CompletionContext {
		CompletionContextKind kind = CompletionContextKind::Unknown;
		std::string prefix;
		bool afterDot = false;
		bool afterDoubleColon = false;
		// Module path extracted for import contexts (single or multi-line).
		std::string modulePath;
	};

	class SyntacticIndex {
		public:
			static CompletionContext completionContext( const std::string& source, TSNode root, uint32_t line, uint32_t column );
	};

} // namespace uranite::lsp::cst

#endif // end _URANITE_LSP_CST_SYNTACTIC_INDEX_HPP_
