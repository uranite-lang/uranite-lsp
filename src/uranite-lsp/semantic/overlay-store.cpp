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

#include "uranite-lsp/semantic/overlay-store.hpp"

#include "uranite-lsp/text/uri.hpp"

#include <filesystem>

namespace uranite::lsp::semantic {

	namespace {

		std::string canonical( const std::string& path ) {
			std::error_code error;
			std::filesystem::path result = std::filesystem::weakly_canonical( path, error );
			if( error || result.empty() ) return path;
			return result.string();
		}

	} // namespace

	void OverlayStore::put( const std::string& path, const std::string& text ) {
		std::lock_guard<std::mutex> lock( this->mutex );
		this->overlays[ path ] = text;
		std::string c = canonical( path );
		if( c != path ) this->overlays[ c ] = text;
	}

	void OverlayStore::remove( const std::string& path ) {
		std::lock_guard<std::mutex> lock( this->mutex );
		this->overlays.erase( path );
		std::string c = canonical( path );
		if( c != path ) this->overlays.erase( c );
	}

	std::optional<std::string> OverlayStore::get( const std::string& path ) const {
		std::lock_guard<std::mutex> lock( this->mutex );
		auto it = this->overlays.find( path );
		if( it != this->overlays.end() ) return it->second;
		std::string c = canonical( path );
		if( c != path ) {
			auto it2 = this->overlays.find( c );
			if( it2 != this->overlays.end() ) return it2->second;
		}
		return std::nullopt;
	}

} // namespace uranite::lsp::semantic
