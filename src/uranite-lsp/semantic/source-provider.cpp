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

#include "uranite-lsp/semantic/source-provider.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace uranite::lsp::semantic {

	namespace {

		std::string canonicalize( const std::string& path ) {
			std::error_code error;
			std::filesystem::path result = std::filesystem::weakly_canonical( path, error );
			if( error ) return path;
			return result.string();
		}

	} // namespace

	LspSourceProvider::LspSourceProvider( std::shared_ptr<OverlayStore> store ) : store( store ) {
	}

	std::optional<std::string> LspSourceProvider::read( const std::string& path ) const {
		if( this->store ) {
			auto o = this->store->get( path );
			if( o.has_value() ) return o;
			std::string canonical = canonicalize( path );
			if( canonical != path ) {
				o = this->store->get( canonical );
				if( o.has_value() ) return o;
			}
		}
		std::ifstream f( path );
		if( !f.is_open() ) {
			std::string canonical = canonicalize( path );
			if( canonical != path ) {
				std::ifstream cf( canonical );
				if( cf.is_open() ) {
					std::ostringstream ss;
					ss << cf.rdbuf();
					return ss.str();
				}
			}
			return std::nullopt;
		}
		std::ostringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

} // namespace uranite::lsp::semantic
