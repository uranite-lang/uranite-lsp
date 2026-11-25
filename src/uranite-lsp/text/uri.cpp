
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

#include <cctype>
#include <filesystem>
#include <string>

#include "uranite-lsp/text/uri.hpp"

namespace uranite::lsp::text {

	namespace {

		int hexValue( char character ) {
			if( character >= '0' && character <= '9' ) return character - '0';
			if( character >= 'a' && character <= 'f' ) return character - 'a' + 10;
			if( character >= 'A' && character <= 'F' ) return character - 'A' + 10;
			return -1;
		}

		bool isUnreserved( unsigned char character ) {
			return std::isalnum( character ) != 0 || character == '-' || character == '_' ||
			       character == '.' || character == '~' || character == '/';
		}

	} // namespace

	std::string uriToPath( const std::string& uri ) {
		const std::string scheme = "file://";
		std::string path = uri;
		if( path.compare( 0, scheme.size(), scheme ) == 0 ) {
			path = path.substr( scheme.size() );
			const std::string localhost = "localhost/";
			if( path.compare( 0, localhost.size(), localhost ) == 0 ) path = path.substr( localhost.size() - 1 );
		}

		std::string decoded;
		decoded.reserve( path.size() );
		for( std::size_t i = 0; i < path.size(); i++ ) {
			if( path[ i ] == '%' && i + 2 < path.size() ) {
				const int high = hexValue( path[ i + 1 ] );
				const int low = hexValue( path[ i + 2 ] );
				if( high >= 0 && low >= 0 ) {
					decoded.push_back( static_cast<char>( high * 16 + low ) );
					i += 2;
					continue;
				}
			}
			decoded.push_back( path[ i ] );
		}
		return decoded;
	}

	std::string pathToUri( const std::string& path ) {
		static const char* digits = "0123456789ABCDEF";
		std::string uri = "file://";
		for( unsigned char character : path ) {
			if( isUnreserved( character ) ) {
				uri.push_back( static_cast<char>( character ) );
			}
			else {
				uri.push_back( '%' );
				uri.push_back( digits[ character >> 4 ] );
				uri.push_back( digits[ character & 0x0F ] );
			}
		}
		return uri;
	}

	std::string canonicalizePath( const std::string& path ) {
		std::error_code error;
		std::filesystem::path result = std::filesystem::weakly_canonical( path, error );
		if( error || result.empty() ) return path;
		return result.string();
	}

} // namespace uranite::lsp::text
