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

#ifndef _URANITE_LSP_TEXT_URI_HPP_
#define _URANITE_LSP_TEXT_URI_HPP_

#include <string>

namespace uranite::lsp::text {

	/** @brief Converts a file: URI to a filesystem path, decoding valid %XX escapes. */
	std::string uriToPath( const std::string& uri );

	/** @brief Converts a filesystem path to a file: URI with percent-encoding. */
	std::string pathToUri( const std::string& path );

	/** @brief Canonicalizes a filesystem path (weakly) for symlink-safe lookups. */
	std::string canonicalizePath( const std::string& path );

} // namespace uranite::lsp::text

#endif // end _URANITE_LSP_TEXT_URI_HPP_
