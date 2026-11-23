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

#ifndef _URANITE_LSP_SEMANTIC_QUERY_SERVICE_HPP_
#define _URANITE_LSP_SEMANTIC_QUERY_SERVICE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "uranite-lsp/semantic/frontend.hpp"
#include "uranite-lsp/semantic/module-graph.hpp"
#include "uranite-lsp/semantic/overlay-store.hpp"
#include "uranite-lsp/semantic/symbol-index.hpp"

namespace uranite::lsp::semantic {

	struct QueryLocation {
		std::string path;
		uint32_t line = 0;
		uint32_t col = 0;
		uint32_t endLine = 0;
		uint32_t endCol = 0;
		bool valid = false;
	};

	struct QueryHover {
		std::string contents;
		bool valid = false;
	};

	struct QueryCompletion {
		struct TextEdit {
			uint32_t startLine = 0;
			uint32_t startCol = 0;
			uint32_t endLine = 0;
			uint32_t endCol = 0;
			std::string newText;
		};
		std::string label;
		std::string detail;
		std::string documentation;
		std::string insertText;
		int insertTextFormat = 1;
		int kind = 0;
		int sortPriority = 3;
		bool valid = false;
		bool resolvable = false;
		std::string dataModule;
		std::string dataSymbol;
		std::string dataUri;
		std::vector<TextEdit> additionalTextEdits;
	};

	struct QuerySignature {
		std::string label;
		std::string documentation;
		uint32_t activeParameter = 0;
		bool valid = false;
	};

	struct QueryWorkspaceSymbol {
		std::string name;
		QueryLocation location;
	};

	/** @brief Answers semantic LSP questions from the compiler-backed index. */
	class QueryService {
		public:
			QueryService( std::shared_ptr<FrontendResult> result, std::shared_ptr<SymbolIndex> index,
			              std::shared_ptr<OverlayStore> overlayStore = nullptr,
			              const std::string& modulesPath = "", const std::string& uri = "",
			              const std::string& text = "",
			              std::shared_ptr<ModuleGraph> moduleGraph = nullptr );

			QueryHover hover( uint32_t line, uint32_t col ) const;
			QueryLocation definition( uint32_t line, uint32_t col ) const;
			std::vector<QueryLocation> references( uint32_t line, uint32_t col, bool includeDeclaration ) const;
			std::vector<QueryCompletion> completion( uint32_t line, uint32_t col ) const;
			QuerySignature signatureHelp( uint32_t line, uint32_t col ) const;
			QueryLocation prepareRename( uint32_t line, uint32_t col ) const;
			std::vector<QueryLocation> rename( uint32_t line, uint32_t col ) const;
			std::vector<QueryWorkspaceSymbol> workspaceSymbols( const std::string& query ) const;

		private:
			std::shared_ptr<FrontendResult> result;
			std::shared_ptr<SymbolIndex> index;
			std::shared_ptr<OverlayStore> overlayStore;
			std::string modulesPath;
			std::string uri;
			std::string text;
			std::shared_ptr<ModuleGraph> moduleGraph;

			static QueryLocation toLocation( const IndexedSymbol& symbol );
			std::vector<QueryLocation> locationsFor( uint32_t line, uint32_t col, bool includeDeclaration ) const;
	};

} // namespace uranite::lsp::semantic

#endif // end _URANITE_LSP_SEMANTIC_QUERY_SERVICE_HPP_
