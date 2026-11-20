
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

#ifndef _URANITE_LSP_SERVER_HPP_
#define _URANITE_LSP_SERVER_HPP_

#include <atomic>

#include "uranite-lsp/json/rpc.hpp"
#include "uranite-lsp/protocol/protocol.hpp"
#include "uranite-lsp/workspace/workspace.hpp"

namespace uranite::lsp {

	class Server {

		private:

			JsonRpcTransport transport;
			std::unique_ptr<Workspace> workspace;
			std::atomic<bool> isShutdownRequested{ false };

			void handleInitialize( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleInitialized();
			void handleShutdown( const llvm::json::Value& requestId );
			void handleTextDocumentDidOpen( const llvm::json::Value& notificationParams );
			void handleTextDocumentDidChange( const llvm::json::Value& notificationParams );
			void handleTextDocumentDidSave( const llvm::json::Value& notificationParams );
			void handleTextDocumentDidClose( const llvm::json::Value& notificationParams );
			void handleTextDocumentCompletion( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentHover( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentDefinition( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentDocumentSymbol( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentFoldingRange( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentSemanticTokensFull( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleWorkspaceSymbol( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentSignatureHelp( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentReferences( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentRename( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );
			void handleTextDocumentPrepareRename( const llvm::json::Value& requestId, const llvm::json::Value& requestParams );

			void publishDiagnostics( Document* targetDocument );
			void runSemanticAnalysisAndPublish( Document* targetDocument );

			Position extractCursorPosition( const llvm::json::Value& requestParams ) const;

			std::string extractDocumentUri( const llvm::json::Value& requestParams ) const;

		public:

			Server( std::FILE* inputStream, std::FILE* outputStream );

			void run();

	};

} // namespace uranite::lsp

#endif // end _URANITE_LSP_SERVER_HPP_
