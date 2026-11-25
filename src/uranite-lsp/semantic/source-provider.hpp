
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

#ifndef _URANITE_LSP_SEMANTIC_SOURCE_PROVIDER_HPP_
#define _URANITE_LSP_SEMANTIC_SOURCE_PROVIDER_HPP_

#include <memory>
#include <optional>
#include <string>

#include "uranite/lookup/source-provider.hpp"

#include "uranite-lsp/semantic/overlay-store.hpp"

namespace uranite::lsp::semantic {

	class LspSourceProvider : public uranite::lookup::SourceProvider {
		public:
			explicit LspSourceProvider( std::shared_ptr<OverlayStore> store );
			std::optional<std::string> read( const std::string& path ) const override;

		private:
			std::shared_ptr<OverlayStore> store;
	};

} // namespace uranite::lsp::semantic

#endif // end _URANITE_LSP_SEMANTIC_SOURCE_PROVIDER_HPP_
