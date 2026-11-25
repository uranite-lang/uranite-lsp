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

#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

#include "uranite-lsp/cst/language.hpp"
#include "uranite-lsp/cst/query-set.hpp"
#include "uranite-lsp/generated/query-sources.hpp"

namespace uranite::lsp::cst {

	namespace {

		struct QuerySource {
			const char* name = nullptr;
			const char* embedded = nullptr;
			TSQuery* compiled = nullptr;
		};

		std::string readFile( const std::string& path ) {
			std::ifstream stream( path );
			if( stream.is_open() == false ) return "";
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}

		std::string describeError( TSQueryError error, uint32_t offset ) {
			return fmt::format( "tree-sitter query error {} at offset {}", static_cast<int>( error ), offset );
		}

	} // namespace

	QuerySet::QuerySet() = default;

	QuerySet::~QuerySet() {
		this->release();
	}

	void QuerySet::release() {
		if( this->highlightsQuery != nullptr ) ts_query_delete( this->highlightsQuery );
		if( this->foldsQuery != nullptr ) ts_query_delete( this->foldsQuery );
		if( this->tagsQuery != nullptr ) ts_query_delete( this->tagsQuery );
		if( this->localsQuery != nullptr ) ts_query_delete( this->localsQuery );
		this->highlightsQuery = nullptr;
		this->foldsQuery = nullptr;
		this->tagsQuery = nullptr;
		this->localsQuery = nullptr;
		this->captureNameMap.clear();
	}

	bool QuerySet::load( const std::string& queriesDir ) {
		this->release();
		this->error.clear();

		std::string directory = queriesDir;
		if( directory.empty() ) {
			const char* environment = std::getenv( "URANITE_TSITTER_QUERIES" );
			if( environment != nullptr ) directory = environment;
		}

		if( this->compileAll( directory ) == false ) {
			this->release();
			return false;
		}
		return true;
	}

	bool QuerySet::compileAll( const std::string& queriesDir ) {
		QuerySource sources[] = {
			{ "highlights", embedded::highlights(), this->highlightsQuery },
			{ "folds", embedded::folds(), this->foldsQuery },
			{ "tags", embedded::tags(), this->tagsQuery },
			{ "locals", embedded::locals(), this->localsQuery },
		};

		for( QuerySource& source : sources ) {
			std::string text = source.embedded;
			if( queriesDir.empty() == false ) {
				const std::string path = queriesDir + "/" + source.name + ".scm";
				if( std::filesystem::exists( path ) ) {
					const std::string fileText = readFile( path );
					if( fileText.empty() == false ) text = fileText;
				}
			}

			TSQueryError queryError = TSQueryErrorNone;
			uint32_t errorOffset = 0;
			TSQuery* query = ts_query_new( language(), text.c_str(), static_cast<uint32_t>( text.size() ),
				&errorOffset, &queryError );
			if( query == nullptr || queryError != TSQueryErrorNone ) {
				this->error = fmt::format( "{}: {}", source.name, describeError( queryError, errorOffset ) );
				if( query != nullptr ) ts_query_delete( query );
				return false;
			}

			source.compiled = query;
			uint32_t count = ts_query_capture_count( query );
			std::vector<std::string> names;
			names.reserve( count );
			for( uint32_t i = 0; i < count; i++ ) {
				uint32_t length = 0;
				const char* name = ts_query_capture_name_for_id( query, i, &length );
				names.emplace_back( name, length );
			}
			this->captureNameMap[ query ] = std::move( names );
		}

		this->highlightsQuery = sources[ 0 ].compiled;
		this->foldsQuery = sources[ 1 ].compiled;
		this->tagsQuery = sources[ 2 ].compiled;
		this->localsQuery = sources[ 3 ].compiled;
		return true;
	}

	const std::vector<std::string>& QuerySet::captureNames( TSQuery* query ) const {
		const auto found = this->captureNameMap.find( query );
		if( found == this->captureNameMap.end() ) {
			static const std::vector<std::string> empty;
			return empty;
		}
		return found->second;
	}

	std::string QuerySet::captureName( TSQuery* query, uint32_t captureId ) const {
		const auto& names = this->captureNames( query );
		if( captureId >= names.size() ) return "";
		return names[ captureId ];
	}

} // namespace uranite::lsp::cst
