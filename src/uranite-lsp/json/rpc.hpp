
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

#ifndef _URANITE_LSP_JSON_RPC_HPP_
#define _URANITE_LSP_JSON_RPC_HPP_

#include <cstdint>
#include <functional>
#include <llvm/Support/JSON.h>
#include <string>

namespace uranite::lsp {

	struct JsonRpcMessage {
		std::string method;
		llvm::json::Value params = nullptr;
		llvm::json::Value id = nullptr;
		bool isNotification = false;
	};

	class JsonRpcTransport {

		private:

			std::FILE* inputStream;
			std::FILE* outputStream;

			bool readHeader( std::string& headerLine );
			bool readContent( uint64_t contentLength, std::string& contentBody );

		public:

			JsonRpcTransport( std::FILE* inputStream, std::FILE* outputStream );

			bool receiveMessage( JsonRpcMessage& receivedMessage );

			void sendResponse( const llvm::json::Value& responseId, llvm::json::Value resultPayload );

			void sendError( const llvm::json::Value& responseId, int32_t errorCode, const std::string& errorMessage );

			void sendNotification( const std::string& notificationMethod, llvm::json::Value notificationParams );

	};

} // namespace uranite::lsp

#endif // end _URANITE_LSP_JSON_RPC_HPP_
