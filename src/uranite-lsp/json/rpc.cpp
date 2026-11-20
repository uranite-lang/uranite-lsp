
// 
// @author hxAri (hxari)
// @create 2026-06-18 15:30
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "llvm/Support/raw_ostream.h"

#include "uranite-lsp/json/rpc.hpp"

namespace uranite::lsp {

	JsonRpcTransport::JsonRpcTransport( std::FILE* inputStream, std::FILE* outputStream )
		: inputStream( inputStream ), outputStream( outputStream ) {
	}

	bool JsonRpcTransport::readHeader( std::string& headerLine ) {
		headerLine.clear();
		while( true ) {
			int readCharacter = std::fgetc( this->inputStream );
			if( readCharacter == EOF ) {
				return false;
			}
			headerLine += static_cast<char>( readCharacter );
			if( headerLine.size() >= 2 &&
				headerLine[headerLine.size() - 2] == '\r' &&
				headerLine[headerLine.size() - 1] == '\n' ) {
				headerLine.resize( headerLine.size() - 2 );
				return true;
			}
		}
	}

	bool JsonRpcTransport::readContent( uint64_t contentLength, std::string& contentBody ) {
		contentBody.resize( contentLength );
		uint64_t totalBytesRead = 0;
		while( totalBytesRead < contentLength ) {
			uint64_t bytesRead = std::fread(
				&contentBody[totalBytesRead], 1,
				contentLength - totalBytesRead,
				this->inputStream
			);
			if( bytesRead == 0 ) {
				return false;
			}
			totalBytesRead += bytesRead;
		}
		return true;
	}

	bool JsonRpcTransport::receiveMessage( JsonRpcMessage& receivedMessage ) {
		uint64_t contentLength = 0;
		bool foundContentLength = false;

		while( true ) {
			std::string headerLine;
			if( this->readHeader( headerLine ) == false ) {
				return false;
			}
			if( headerLine.empty() ) {
				break;
			}
			if( headerLine.find( "Content-Length:" ) == 0 ) {
				std::string lengthValue = headerLine.substr( 15 );
				while( lengthValue.empty() == false && lengthValue[0] == ' ' ) {
					lengthValue = lengthValue.substr( 1 );
				}
				contentLength = std::stoull( lengthValue );
				foundContentLength = true;
			}
		}

		if( foundContentLength == false || contentLength == 0 ) {
			return false;
		}

		std::string contentBody;
		if( this->readContent( contentLength, contentBody ) == false ) {
			return false;
		}

		llvm::Expected<llvm::json::Value> parsedJson = llvm::json::parse( contentBody );
		if( !parsedJson ) {
			llvm::consumeError( parsedJson.takeError() );
			return false;
		}

		llvm::json::Object* rootObject = parsedJson->getAsObject();
		if( rootObject == nullptr ) {
			return false;
		}

		if( llvm::json::Value* methodValue = rootObject->get( "method" ) ) {
			if( std::optional<llvm::StringRef> methodString = methodValue->getAsString() ) {
				receivedMessage.method = methodString->str();
			}
		}

		if( llvm::json::Value* paramsValue = rootObject->get( "params" ) ) {
			receivedMessage.params = std::move( *paramsValue );
		}
		else {
			receivedMessage.params = nullptr;
		}

		if( llvm::json::Value* idValue = rootObject->get( "id" ) ) {
			receivedMessage.id = std::move( *idValue );
			receivedMessage.isNotification = false;
		}
		else {
			receivedMessage.id = nullptr;
			receivedMessage.isNotification = true;
		}

		return true;
	}

	void JsonRpcTransport::sendResponse( const llvm::json::Value& responseId, llvm::json::Value resultPayload ) {
		llvm::json::Object responseObject;
		responseObject["jsonrpc"] = "2.0";
		responseObject["id"] = llvm::json::Value( responseId );
		responseObject["result"] = std::move( resultPayload );

		std::string serializedResponse;
		llvm::raw_string_ostream outputBuffer( serializedResponse );
		outputBuffer << llvm::json::Value( std::move( responseObject ) );
		outputBuffer.flush();

		std::fprintf( this->outputStream, "Content-Length: %zu\r\n\r\n%s",
			serializedResponse.size(), serializedResponse.c_str() );
		std::fflush( this->outputStream );
	}

	void JsonRpcTransport::sendError( const llvm::json::Value& responseId, int32_t errorCode, const std::string& errorMessage ) {
		llvm::json::Object errorDetail;
		errorDetail["code"] = errorCode;
		errorDetail["message"] = errorMessage;

		llvm::json::Object responseObject;
		responseObject["jsonrpc"] = "2.0";
		responseObject["id"] = llvm::json::Value( responseId );
		responseObject["error"] = llvm::json::Value( std::move( errorDetail ) );

		std::string serializedResponse;
		llvm::raw_string_ostream outputBuffer( serializedResponse );
		outputBuffer << llvm::json::Value( std::move( responseObject ) );
		outputBuffer.flush();

		std::fprintf( this->outputStream, "Content-Length: %zu\r\n\r\n%s",
			serializedResponse.size(), serializedResponse.c_str() );
		std::fflush( this->outputStream );
	}

	void JsonRpcTransport::sendNotification( const std::string& notificationMethod, llvm::json::Value notificationParams ) {
		llvm::json::Object notificationObject;
		notificationObject["jsonrpc"] = "2.0";
		notificationObject["method"] = notificationMethod;
		notificationObject["params"] = std::move( notificationParams );

		std::string serializedNotification;
		llvm::raw_string_ostream outputBuffer( serializedNotification );
		outputBuffer << llvm::json::Value( std::move( notificationObject ) );
		outputBuffer.flush();

		std::fprintf( this->outputStream, "Content-Length: %zu\r\n\r\n%s",
			serializedNotification.size(), serializedNotification.c_str() );
		std::fflush( this->outputStream );
	}

} // namespace uranite::lsp
