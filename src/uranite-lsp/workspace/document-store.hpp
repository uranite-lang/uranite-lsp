
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

#ifndef _URANITE_LSP_WORKSPACE_DOCUMENT_STORE_HPP_
#define _URANITE_LSP_WORKSPACE_DOCUMENT_STORE_HPP_

#include <map>
#include <memory>
#include <string>

#include "uranite-lsp/cst/parse-session.hpp"
#include "uranite-lsp/cst/query-set.hpp"
#include "uranite-lsp/text/document.hpp"
#include "uranite-lsp/text/uri.hpp"

namespace uranite::lsp::workspace {

	struct DocumentEntry {
		text::Document document;
		cst::ParseSession parseSession;
		std::string clientUri;
	};

	class DocumentStore {
		public:
			void open( const std::string& uri, const std::string& text, int version );
			void change( const std::string& uri, const std::string& text, int version );
			void close( const std::string& uri );

			DocumentEntry* get( const std::string& uri );
			const DocumentEntry* get( const std::string& uri ) const;

		private:
			std::map<std::string, std::shared_ptr<DocumentEntry>> entries;
	};

} // namespace uranite::lsp::workspace

#endif // end _URANITE_LSP_WORKSPACE_DOCUMENT_STORE_HPP_
