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

#include "uranite-lsp/workspace/document-store.hpp"

#include <memory>

namespace uranite::lsp::workspace {

	void DocumentStore::open( const std::string& uri, const std::string& text, int version ) {
		std::string key = text::canonicalizePath( text::uriToPath( uri ) );
		if( key.empty() ) key = uri;
		auto entry = std::make_shared<DocumentEntry>();
		entry->document = text::Document( uri, text, version );
		entry->parseSession.parse( text );
		entry->clientUri = uri;
		this->entries[ key ] = entry;
	}

	void DocumentStore::change( const std::string& uri, const std::string& text, int version ) {
		std::string key = text::canonicalizePath( text::uriToPath( uri ) );
		if( key.empty() ) key = uri;
		auto it = this->entries.find( key );
		if( it == this->entries.end() ) {
			this->open( uri, text, version );
			return;
		}
		it->second->document.update( text, version );
		it->second->parseSession.parse( text );
		it->second->clientUri = uri;
	}

	void DocumentStore::close( const std::string& uri ) {
		std::string key = text::canonicalizePath( text::uriToPath( uri ) );
		if( key.empty() ) key = uri;
		this->entries.erase( key );
	}

	DocumentEntry* DocumentStore::get( const std::string& uri ) {
		std::string key = text::canonicalizePath( text::uriToPath( uri ) );
		if( key.empty() ) key = uri;
		auto it = this->entries.find( key );
		if( it == this->entries.end() ) return nullptr;
		return it->second.get();
	}

	const DocumentEntry* DocumentStore::get( const std::string& uri ) const {
		std::string key = text::canonicalizePath( text::uriToPath( uri ) );
		if( key.empty() ) key = uri;
		auto it = this->entries.find( key );
		if( it == this->entries.end() ) return nullptr;
		return it->second.get();
	}

} // namespace uranite::lsp::workspace
