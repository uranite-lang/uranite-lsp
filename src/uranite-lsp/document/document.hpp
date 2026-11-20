
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

#ifndef _URANITE_LSP_DOCUMENT_HPP_
#define _URANITE_LSP_DOCUMENT_HPP_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "tree_sitter/api.h"

#include "uranite-lsp/protocol/protocol.hpp"
#include "uranite-lsp/semantic/analyzer.hpp"

extern "C" const TSLanguage* tree_sitter_uranite();

namespace uranite::lsp {

	class Document {

		private:

			std::string documentUri;
			std::string documentContent;
			int64_t documentVersion = 0;

			TSParser* syntaxParser = nullptr;
			TSTree* parsedTree = nullptr;

			std::vector<std::string> cachedSourceLines;
			mutable std::mutex documentMutex;

			SemanticAnalysisResult cachedSemanticResult;
			bool semanticAnalysisStale = true;

			void rebuildLineCache();
			uint32_t computeByteOffset( const Position& position ) const;
			Position computePositionFromByte( uint32_t byteOffset ) const;

		public:

			Document( const std::string& documentUri, const std::string& initialContent, int64_t initialVersion );
			~Document();

			Document( const Document& ) = delete;
			Document& operator=( const Document& ) = delete;

			void applyContentChanges( const std::vector<ContentChangeEvent>& contentChanges, int64_t newVersion );

			void refreshSemanticAnalysis( const std::string& documentFilePath, const std::string& workspaceRootPath );

			const SemanticAnalysisResult& semanticResult() const { return this->cachedSemanticResult; }
			bool isSemanticStale() const { return this->semanticAnalysisStale; }

			TSNode rootNode() const;

			const std::string& uri() const { return this->documentUri; }
			const std::string& content() const { return this->documentContent; }
			int64_t version() const { return this->documentVersion; }
			const TSTree* tree() const { return this->parsedTree; }
			const std::vector<std::string>& sourceLines() const { return this->cachedSourceLines; }

			std::vector<Diagnostic> collectTreeErrors() const;
			std::vector<FoldingRange> collectFoldingRanges() const;
			std::vector<DocumentSymbol> collectDocumentSymbols() const;
			std::vector<SemanticToken> collectSemanticTokens() const;

			TSNode findNodeAtPosition( const Position& targetPosition ) const;
			std::string nodeText( TSNode targetNode ) const;

	};

} // namespace uranite::lsp

#endif // end _URANITE_LSP_DOCUMENT_HPP_
