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

#include "uranite-lsp/jsonrpc/message.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

namespace uranite::lsp::jsonrpc {

	namespace {

		constexpr std::size_t maximumHeaderBytes = 1024 * 1024;
		constexpr std::size_t maximumBodyBytes = 64 * 1024 * 1024;

		std::mutex& outputMutex() {
			static std::mutex mutex;
			return mutex;
		}

		std::optional<std::size_t> parseContentLength( const std::string& headers ) {
			std::string lower = headers;
			std::transform( lower.begin(), lower.end(), lower.begin(), []( unsigned char character ) {
				return static_cast<char>( std::tolower( character ) );
			} );
			std::size_t position = lower.find( "content-length:" );
			if( position == std::string::npos ) return std::nullopt;
			position += 15;
			while( position < lower.size() && ( lower[ position ] == ' ' || lower[ position ] == '\t' ) ) position++;

			std::size_t end = lower.find( "\r\n", position );
			if( end == std::string::npos ) end = lower.find( "\n", position );
			if( end == std::string::npos ) end = lower.size();

			const std::string digits = lower.substr( position, end - position );
			if( digits.empty() ) return std::nullopt;
			for( char character : digits ) {
				if( std::isdigit( static_cast<unsigned char>( character ) ) == 0 ) return std::nullopt;
			}
			try {
				return static_cast<std::size_t>( std::stoull( digits ) );
			}
			catch( ... ) {
				return std::nullopt;
			}
		}

		ReadResult malformed( const std::string& error ) {
			ReadResult result;
			result.status = ReadStatus::Malformed;
			result.error = error;
			return result;
		}

		void writeFramed( std::FILE* out, const std::string& body ) {
			if( out == nullptr ) return;
			const std::string header = "Content-Length: " + std::to_string( body.size() ) + "\r\n\r\n";
			std::lock_guard<std::mutex> lock( outputMutex() );
			std::fwrite( header.c_str(), 1, header.size(), out );
			std::fwrite( body.c_str(), 1, body.size(), out );
			std::fflush( out );
		}

	} // namespace

	ReadResult readMessageResult( std::FILE* in ) {
		if( in == nullptr ) return malformed( "null input stream" );

		std::string headers;
		char line[ 4096 ];
		bool sawBlank = false;
		while( true ) {
			if( std::fgets( line, sizeof( line ), in ) == nullptr ) {
				if( headers.empty() ) {
					ReadResult result;
					result.status = ReadStatus::EndOfFile;
					return result;
				}
				return malformed( "unterminated headers" );
			}
			headers += line;
			if( headers.size() > maximumHeaderBytes ) return malformed( "header block too large" );
			if( std::string( line ) == "\r\n" || std::string( line ) == "\n" ) {
				sawBlank = true;
				break;
			}
		}
		if( sawBlank == false ) return malformed( "unterminated headers" );

		const std::optional<std::size_t> length = parseContentLength( headers );
		if( length.has_value() == false ) return malformed( "missing or invalid Content-Length" );
		if( length.value() > maximumBodyBytes ) return malformed( "message body too large" );

		std::string body;
		body.resize( length.value() );
		if( length.value() > 0 ) {
			const std::size_t read = std::fread( body.data(), 1, length.value(), in );
			if( read != length.value() ) return malformed( "truncated message body" );
		}

		llvm::Expected<llvm::json::Value> parsed = llvm::json::parse( body );
		if( !parsed ) return malformed( "invalid JSON: " + llvm::toString( parsed.takeError() ) );
		const llvm::json::Object* object = parsed->getAsObject();
		if( object == nullptr ) return malformed( "JSON-RPC message must be an object" );

		Message message;
		if( const llvm::json::Value* method = object->get( "method" ) ) {
			if( const auto text = method->getAsString() ) message.method = text->str();
		}
		if( const llvm::json::Value* params = object->get( "params" ) ) message.params = *params;
		if( const llvm::json::Value* id = object->get( "id" ) ) message.id = *id;
		message.isNotification = message.id.has_value() == false;
		message.isResponse = object->get( "result" ) != nullptr || object->get( "error" ) != nullptr;
		if( message.method.empty() && message.isResponse == false ) return malformed( "message has neither method nor response" );

		ReadResult result;
		result.status = ReadStatus::Ok;
		result.message = std::move( message );
		return result;
	}

	std::optional<Message> readMessage( std::FILE* in ) {
		const ReadResult result = readMessageResult( in );
		return result.message;
	}

	void writeResponse( std::FILE* out, const llvm::json::Value& id, const llvm::json::Value& result ) {
		llvm::json::Object object;
		object[ "jsonrpc" ] = "2.0";
		object[ "id" ] = id;
		object[ "result" ] = result;
		std::string body;
		llvm::raw_string_ostream stream( body );
		stream << llvm::json::Value( std::move( object ) );
		stream.flush();
		writeFramed( out, body );
	}

	void writeError( std::FILE* out, const llvm::json::Value& id, int code, const std::string& message ) {
		llvm::json::Object error;
		error[ "code" ] = code;
		error[ "message" ] = message;
		llvm::json::Object object;
		object[ "jsonrpc" ] = "2.0";
		object[ "id" ] = id;
		object[ "error" ] = std::move( error );
		std::string body;
		llvm::raw_string_ostream stream( body );
		stream << llvm::json::Value( std::move( object ) );
		stream.flush();
		writeFramed( out, body );
	}

	void writeNotification( std::FILE* out, const std::string& method, const llvm::json::Value& params ) {
		llvm::json::Object object;
		object[ "jsonrpc" ] = "2.0";
		object[ "method" ] = method;
		object[ "params" ] = params;
		std::string body;
		llvm::raw_string_ostream stream( body );
		stream << llvm::json::Value( std::move( object ) );
		stream.flush();
		writeFramed( out, body );
	}

} // namespace uranite::lsp::jsonrpc
