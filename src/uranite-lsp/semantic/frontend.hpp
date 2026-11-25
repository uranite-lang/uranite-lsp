
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

#ifndef _URANITE_LSP_SEMANTIC_FRONTEND_HPP_
#define _URANITE_LSP_SEMANTIC_FRONTEND_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "uranite/ast/node.hpp"
#include "uranite/compiler/driver.hpp"
#include "uranite/compiler/option.hpp"
#include "uranite/diagnostic/diagnostic.hpp"
#include "uranite/semantic/analyzer.hpp"
#include "uranite/semantic/borrow.hpp"
#include "uranite/semantic/symbol.hpp"
#include "uranite/semantic/typeref.hpp"
#include "uranite/ir/hir.hpp"
#include "uranite/ir/hir/lowering.hpp"
#include "uranite/ir/hir/validator.hpp"
#include "uranite/ir/mir.hpp"
#include "uranite/ir/mir/lowering.hpp"
#include "uranite/ir/mir/borrow.hpp"
#include "uranite/ir/mir/analyzer.hpp"

#include "uranite-lsp/semantic/module-graph.hpp"
#include "uranite-lsp/semantic/overlay-store.hpp"
#include "uranite-lsp/semantic/symbol-index.hpp"

namespace uranite::lsp::semantic {

	/** @brief Optional 0-based LSP position at which to gather completions. */
	struct CompletionRequest {
		bool enabled = false;
		uint32_t line = 0;
		uint32_t col = 0;
	};

	struct FrontendResult {
		std::shared_ptr<ast::nodes::Program> program;
		std::shared_ptr<uranite::semantic::Analyzer> analyzer;
		diagnostic::Engine diagnostics;
		std::shared_ptr<SymbolIndex> symbolIndex;
		std::unordered_map<std::string, std::vector<uranite::semantic::SymbolSharedPointer>> moduleSymbols;
		std::unordered_map<std::string, uranite::semantic::TypeSharedPointer> moduleTypes;
		std::vector<uranite::semantic::CompletionItem> completions;
		std::string clientUri;
		std::string canonicalPath;
		bool success = false;
	};

	class Frontend {
		public:
			Frontend( std::shared_ptr<OverlayStore> overlayStore, std::shared_ptr<ModuleGraph> moduleGraph,
			          const std::string& modulesPathOverride = "" );

			FrontendResult analyze( const std::string& uri, const std::string& source,
			                        const CompletionRequest& completion = CompletionRequest() );

		private:
			std::shared_ptr<OverlayStore> overlayStore;
			std::shared_ptr<ModuleGraph> moduleGraph;
			std::string modulesPathOverride;
	};

} // namespace uranite::lsp::semantic

#endif // end _URANITE_LSP_SEMANTIC_FRONTEND_HPP_
