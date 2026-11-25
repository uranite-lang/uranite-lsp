
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

#ifndef _URANITE_LSP_SERVER_SERVER_HPP_
#define _URANITE_LSP_SERVER_SERVER_HPP_

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "uranite-lsp/cst/query-set.hpp"
#include "uranite-lsp/jsonrpc/message.hpp"
#include "uranite-lsp/semantic/frontend.hpp"
#include "uranite-lsp/semantic/module-graph.hpp"
#include "uranite-lsp/semantic/overlay-store.hpp"
#include "uranite-lsp/semantic/query-service.hpp"
#include "uranite-lsp/workspace/analysis-scheduler.hpp"
#include "uranite-lsp/workspace/document-store.hpp"

namespace uranite::lsp::server {

	class Server {
		public:
			Server( std::FILE* in, std::FILE* out );
			~Server();
			int run();

			/** @brief Waits for queued analysis work; useful for tests and shutdown. */
			void flushAnalysis();

			/** @brief Reads, dispatches one framed message and flushes analysis. Returns false at EOF or exit. */
			bool step();

		private:
			std::FILE* input;
			std::FILE* output;

			bool initialized = false;
			bool shutdownRequested = false;

			workspace::DocumentStore documents;
			workspace::AnalysisScheduler scheduler;
			cst::QuerySet queries;

			mutable std::mutex resultsMutex;
			std::shared_ptr<semantic::OverlayStore> overlayStore;
			std::string modulesPath;
			std::shared_ptr<semantic::ModuleGraph> moduleGraph;
			std::string positionEncoding = "utf-16";
			std::map<std::string, std::shared_ptr<semantic::FrontendResult>> analysisResults;

			bool readAndDispatch( bool flush );
			void handle( const jsonrpc::Message& msg );
			std::shared_ptr<semantic::QueryService> queryServiceFor( const std::string& uri ) const;
			void runAnalysis( const std::string& uri );
			void analyzeAndPublish( const std::string& uri, const std::string& text, uint64_t generation );
	};

} // namespace uranite::lsp::server

#endif // end _URANITE_LSP_SERVER_SERVER_HPP_
