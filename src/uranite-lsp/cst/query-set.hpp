
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

#ifndef _URANITE_LSP_CST_QUERY_SET_HPP_
#define _URANITE_LSP_CST_QUERY_SET_HPP_

#include <map>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace uranite::lsp::cst {

	/** @brief Owns the compiled tree-sitter queries used by every CST provider. */
	class QuerySet {
		public:
			QuerySet();
			~QuerySet();

			QuerySet( const QuerySet& ) = delete;
			QuerySet& operator=( const QuerySet& ) = delete;

			/**
			 * @brief Compiles the embedded queries, optionally overridden by files.
			 *
			 * An explicit directory (or URANITE_TSITTER_QUERIES when the argument is
			 * empty) may override individual .scm files; missing files fall back to
			 * the embedded copies so an installed server is self-contained.
			 */
			bool load( const std::string& queriesDir = "" );

			TSQuery* highlights() const { return this->highlightsQuery; }
			TSQuery* folds() const { return this->foldsQuery; }
			TSQuery* tags() const { return this->tagsQuery; }
			TSQuery* locals() const { return this->localsQuery; }

			const std::vector<std::string>& captureNames( TSQuery* query ) const;
			std::string captureName( TSQuery* query, uint32_t captureId ) const;

			const std::string& lastError() const { return this->error; }

		private:
			TSQuery* highlightsQuery = nullptr;
			TSQuery* foldsQuery = nullptr;
			TSQuery* tagsQuery = nullptr;
			TSQuery* localsQuery = nullptr;

			std::map<TSQuery*, std::vector<std::string>> captureNameMap;
			std::string error;

			void release();
			bool compileAll( const std::string& queriesDir );
	};

} // namespace uranite::lsp::cst

#endif // end _URANITE_LSP_CST_QUERY_SET_HPP_
