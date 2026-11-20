
// 
// @author hxAri (hxari)
// @create 2026-06-18 15:30
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

#ifndef _URANITE_LSP_WORKSPACE_HPP_
#define _URANITE_LSP_WORKSPACE_HPP_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "uranite-lsp/document/document.hpp"
#include "uranite-lsp/protocol/protocol.hpp"

namespace uranite::lsp {

	struct WorkspaceSymbol {
		std::string symbolName;
		int32_t symbolKind = 0;
		Location symbolLocation;
	};

	class Workspace {

		private:

			std::string rootUri;
			std::string rootFilePath;
			std::unordered_map<std::string, std::unique_ptr<Document>> openDocuments;

		public:

			explicit Workspace( const std::string& rootUri );

			Document* openDocument( const std::string& documentUri, const std::string& content, int64_t version );
			Document* getDocument( const std::string& documentUri );
			void closeDocument( const std::string& documentUri );

			std::vector<WorkspaceSymbol> searchSymbols( const std::string& queryText ) const;

			const std::string& getRootFilePath() const { return this->rootFilePath; }

			std::string uriToFilePath( const std::string& documentUri ) const;
			std::string filePathToUri( const std::string& filePath ) const;

	};

} // namespace uranite::lsp

#endif // end _URANITE_LSP_WORKSPACE_HPP_
