
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

#ifndef _URANITE_LSP_JSONRPC_MESSAGE_HPP_
#define _URANITE_LSP_JSONRPC_MESSAGE_HPP_

#include <cstdint>
#include <cstdio>
#include <llvm/Support/JSON.h>
#include <optional>
#include <string>

namespace uranite::lsp::jsonrpc {

	struct Message {
		std::string method;
		llvm::json::Value params = nullptr;
		std::optional<llvm::json::Value> id;
		bool isNotification = false;
		bool isResponse = false;
	};

	enum class ReadStatus : uint8_t {
		Ok,
		EndOfFile,
		Malformed
	};

	struct ReadResult {
		ReadStatus status = ReadStatus::EndOfFile;
		std::optional<Message> message;
		std::string error;
	};

	/** @brief Reads one framed JSON-RPC message, distinguishing EOF from bad framing. */
	ReadResult readMessageResult( std::FILE* in );

	/** @brief Compatibility wrapper: returns nullopt on EOF and on malformed input. */
	std::optional<Message> readMessage( std::FILE* in );

	/** @brief Writes a framed JSON-RPC response. */
	void writeResponse( std::FILE* out, const llvm::json::Value& id, const llvm::json::Value& result );

	/** @brief Writes a framed JSON-RPC error response. */
	void writeError( std::FILE* out, const llvm::json::Value& id, int code, const std::string& message );

	/** @brief Writes a framed JSON-RPC notification (no id). */
	void writeNotification( std::FILE* out, const std::string& method, const llvm::json::Value& params );

} // namespace uranite::lsp::jsonrpc

#endif // end _URANITE_LSP_JSONRPC_MESSAGE_HPP_
