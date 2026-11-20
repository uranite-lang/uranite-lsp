
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

#include <algorithm>

#include "uranite-lsp/workspace/workspace.hpp"

namespace uranite::lsp {

	Workspace::Workspace( const std::string& rootUri )
		: rootUri( rootUri ) {
		this->rootFilePath = this->uriToFilePath( rootUri );
	}

	Document* Workspace::openDocument( const std::string& documentUri, const std::string& content, int64_t version ) {
		auto existingDocument = this->openDocuments.find( documentUri );
		if( existingDocument != this->openDocuments.end() ) {
			this->openDocuments.erase( existingDocument );
		}
		auto insertedPair = this->openDocuments.emplace(
			documentUri,
			std::make_unique<Document>( documentUri, content, version )
		);
		return insertedPair.first->second.get();
	}

	Document* Workspace::getDocument( const std::string& documentUri ) {
		auto foundDocument = this->openDocuments.find( documentUri );
		if( foundDocument == this->openDocuments.end() ) {
			return nullptr;
		}
		return foundDocument->second.get();
	}

	void Workspace::closeDocument( const std::string& documentUri ) {
		this->openDocuments.erase( documentUri );
	}

	std::vector<WorkspaceSymbol> Workspace::searchSymbols( const std::string& queryText ) const {
		std::vector<WorkspaceSymbol> matchedSymbols;

		for( const auto& documentEntry : this->openDocuments ) {
			std::vector<DocumentSymbol> documentSymbols = documentEntry.second->collectDocumentSymbols();
			for( const DocumentSymbol& documentSymbol : documentSymbols ) {
				if( queryText.empty() || documentSymbol.name.find( queryText ) != std::string::npos ) {
					WorkspaceSymbol workspaceSymbol;
					workspaceSymbol.symbolName = documentSymbol.name;
					workspaceSymbol.symbolKind = documentSymbol.kind;
					workspaceSymbol.symbolLocation.uri = documentEntry.first;
					workspaceSymbol.symbolLocation.range = documentSymbol.selectionRange;
					matchedSymbols.push_back( workspaceSymbol );
				}
			}
		}

		return matchedSymbols;
	}

	std::string Workspace::uriToFilePath( const std::string& documentUri ) const {
		std::string filePath = documentUri;
		if( filePath.find( "file://" ) == 0 ) {
			filePath = filePath.substr( 7 );
		}
		std::string decodedPath;
		for( size_t charIndex = 0; charIndex < filePath.size(); ++charIndex ) {
			if( filePath[charIndex] == '%' && charIndex + 2 < filePath.size() ) {
				std::string hexDigits = filePath.substr( charIndex + 1, 2 );
				char decodedCharacter = static_cast<char>( std::stoi( hexDigits, nullptr, 16 ) );
				decodedPath += decodedCharacter;
				charIndex += 2;
			}
			else {
				decodedPath += filePath[charIndex];
			}
		}
		return decodedPath;
	}

	std::string Workspace::filePathToUri( const std::string& filePath ) const {
		return "file://" + filePath;
	}

} // namespace uranite::lsp
