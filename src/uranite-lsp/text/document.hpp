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

#ifndef _URANITE_LSP_TEXT_DOCUMENT_HPP_
#define _URANITE_LSP_TEXT_DOCUMENT_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "uranite-lsp/text/position.hpp"

namespace uranite::lsp::text {

	class Document {
		public:
			Document();
			Document( const std::string& uri, const std::string& text, int version );

			const std::string& uri() const { return this->documentUri; }
			const std::string& text() const { return this->content; }
			int version() const { return this->documentVersion; }

			void update( const std::string& fullText, int version );

			std::vector<std::string> lines() const;

			// apply an incremental change (start/end positions are UTF-16)
			void applyChange( const Range& range, const std::string& newText );

		private:
			std::string documentUri;
			std::string content;
			int documentVersion;
	};

} // namespace uranite::lsp::text

#endif // end _URANITE_LSP_TEXT_DOCUMENT_HPP_
