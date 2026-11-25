
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

#ifndef _URANITE_LSP_SEMANTIC_SYMBOL_INDEX_HPP_
#define _URANITE_LSP_SEMANTIC_SYMBOL_INDEX_HPP_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "uranite/ast/node.hpp"
#include "uranite/semantic/scope.hpp"
#include "uranite/semantic/symbol.hpp"
#include "uranite/semantic/typeref.hpp"

namespace uranite::lsp::semantic {

	/** @brief One definition or resolved identifier usage in the workspace. */
	struct IndexedSymbol {
		std::string name;
		std::string qualifiedName;
		uranite::semantic::Symbol::Kind kind = uranite::semantic::Symbol::Kind::Variable;
		uranite::semantic::SymbolSharedPointer symbol;
		uranite::semantic::TypeSharedPointer type;
		std::string container;
		uint32_t line = 0;
		uint32_t col = 0;
		uint32_t endLine = 0;
		uint32_t endCol = 0;
		std::string pathname;
		bool isDefinition = false;
	};

	/** @brief A call expression with the resolved callee and argument positions. */
	struct IndexedCall {
		std::string name;
		std::string qualifiedName;
		uranite::semantic::SymbolSharedPointer symbol;
		uranite::semantic::TypeSharedPointer type;
		std::vector<uint32_t> argumentLines;
		std::vector<uint32_t> argumentCols;
		uint32_t line = 0;
		uint32_t col = 0;
		uint32_t endLine = 0;
		uint32_t endCol = 0;
		std::string pathname;
	};

	/** @brief Indexes compiler symbols, resolved identifier usages and call sites. */
	class SymbolIndex {
		public:
			SymbolIndex();

			void index( const ast::nodes::Program& program,
			            const std::unordered_map<std::string, std::vector<uranite::semantic::SymbolSharedPointer>>& accumulatedSymbols,
			            const std::unordered_map<std::string, uranite::semantic::TypeSharedPointer>& accumulatedTypes,
			            const uranite::semantic::ScopeSharedPointer& globalScope,
			            const std::string& sourceText = "",
			            const std::string& documentPath = "" );

			// Position is 0-based LSP.
			const IndexedSymbol* symbolAt( uint32_t line, uint32_t col ) const;
			const IndexedCall* callAt( uint32_t line, uint32_t col ) const;
			std::vector<const IndexedSymbol*> definitionsFor( const std::string& qualifiedName ) const;
			std::vector<const IndexedSymbol*> referencesFor( const std::string& qualifiedName ) const;
			std::vector<const IndexedSymbol*> allDefinitions() const;
			std::vector<const IndexedSymbol*> allSymbols() const;

			const std::vector<IndexedSymbol>& definitions() const { return this->defs; }
			const std::vector<IndexedSymbol>& references() const { return this->refs; }
			const std::vector<IndexedCall>& calls() const { return this->callSites; }

			void addDefinition( const IndexedSymbol& symbol );
			void addReference( const IndexedSymbol& symbol );
			void addCall( const IndexedCall& call );

		private:
			std::vector<IndexedSymbol> defs;
			std::vector<IndexedSymbol> refs;
			std::vector<IndexedCall> callSites;
			std::map<std::string, std::vector<std::size_t>> definitionsByName;
			std::map<std::string, std::vector<std::size_t>> referencesByName;
			std::string documentPath;
	};

} // namespace uranite::lsp::semantic

#endif // end _URANITE_LSP_SEMANTIC_SYMBOL_INDEX_HPP_
