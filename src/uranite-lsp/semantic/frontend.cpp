
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

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "uranite/ast/node.hpp"
#include "uranite/compiler/driver.hpp"
#include "uranite/compiler/option.hpp"
#include "uranite/diagnostic/diagnostic.hpp"
#include "uranite/lexer/lexer.hpp"
#include "uranite/parser/parser.hpp"
#include "uranite/semantic/analyzer.hpp"
#include "uranite/semantic/borrow.hpp"
#include "uranite/semantic/qualnames.hpp"
#include "uranite/ir/hir.hpp"
#include "uranite/ir/hir/lowering.hpp"
#include "uranite/ir/hir/validator.hpp"
#include "uranite/ir/mir.hpp"
#include "uranite/ir/mir/lowering.hpp"
#include "uranite/ir/mir/borrow.hpp"
#include "uranite/ir/mir/analyzer.hpp"
#include "uranite/lookup/source.hpp"
#include "uranite/token/token.hpp"
#include "uranite/visitors/printer.hpp"

#include "uranite-lsp/semantic/frontend.hpp"
#include "uranite-lsp/semantic/overlay-store.hpp"
#include "uranite-lsp/semantic/source-provider.hpp"
#include "uranite-lsp/text/uri.hpp"

namespace uranite::lsp::semantic {

	Frontend::Frontend( std::shared_ptr<OverlayStore> overlayStore, std::shared_ptr<ModuleGraph> moduleGraph,
	                    const std::string& modulesPathOverride )
		: overlayStore( overlayStore ), moduleGraph( moduleGraph ), modulesPathOverride( modulesPathOverride ) {
	}

	FrontendResult Frontend::analyze( const std::string& uri, const std::string& source,
	                                  const CompletionRequest& completion ) {
		FrontendResult result;
		diagnostic::Engine engine( 100, 100 );
		engine.setConsolPrintable( false );
		result.clientUri = uri;
		const std::string rawPath = text::uriToPath( uri );
		std::error_code canonicalError;
		std::string pathname = std::filesystem::weakly_canonical( rawPath, canonicalError ).string();
		if( canonicalError || pathname.empty() ) pathname = rawPath;
		result.canonicalPath = pathname;

		// Lexer
		lexer::Lexer lexer( source, pathname, engine );
		std::vector<token::Token> tokens = lexer.tokenize();
		engine.setSourceLines( lexer.sourceLines() );
		if( engine.hasErrors() ) {
			result.diagnostics = engine;
			return result;
		}

		// Parser
		parser::Parser parser( tokens, engine );
		ast::nodes::ProgramSharedPointer programRoot = parser.parse();
		if( engine.hasErrors() ) {
			result.diagnostics = engine;
			return result;
		}

		// Driver for module loading (disk + overlays)
		compiler::Options options;
		std::string stdlibRoot;
		std::vector<std::string> projectRoots;
		if( this->moduleGraph ) {
			stdlibRoot = this->moduleGraph->stdlibRoot();
			projectRoots = this->moduleGraph->projectModuleRoots();
		}
		if( this->modulesPathOverride.empty() == false && std::filesystem::is_directory( this->modulesPathOverride ) ) {
			stdlibRoot = this->modulesPathOverride;
		}
		if( stdlibRoot.empty() == false ) {
			options.modulesPath = stdlibRoot;
		}
		for( const std::string& root : projectRoots ) {
			options.includePaths.push_back( root );
		}
		options.output.source = pathname;
		compiler::Driver driver( options );
		auto provider = std::make_shared<LspSourceProvider>( this->overlayStore );
		driver.setSourceProvider( provider );

		driver.diagnosticEngine().setMaxErrors( 100 );

		size_t userDeclCount = programRoot->declarations.size();
		driver.loadPrelude( *programRoot );
		driver.resolveImports( *programRoot );
		driver.loadStandardModules( *programRoot );
		result.moduleSymbols = driver.accumulatedModuleSymbols();
		result.moduleTypes = driver.accumulatedModuleTypes();

		// Surface the driver's import/module diagnostics through the LSP engine.
		for( const auto& diagnostic : driver.diagnosticEngine().diagnostics() ) {
			engine.emit( diagnostic.level, diagnostic.source, diagnostic.messages, diagnostic.hinting );
		}

		auto buildIndex = [ &result, &source ]() {
			if( result.program == nullptr || result.analyzer == nullptr ) return;
			for( const auto& entry : result.analyzer->getRegisteredTypes() ) {
				if( entry.second == nullptr ) continue;
				if( result.moduleTypes.find( entry.first ) == result.moduleTypes.end() ) {
					result.moduleTypes[ entry.first ] = entry.second;
				}
			}
			result.symbolIndex = std::make_shared<SymbolIndex>();
			result.symbolIndex->index( *result.program, result.moduleSymbols, result.moduleTypes,
				result.analyzer->globalScope(), source, result.canonicalPath );
		};

		// Semantic analyzer
		auto analyzer = std::make_shared<uranite::semantic::Analyzer>( engine );
		analyzer->importModuleTypes( driver.accumulatedModuleTypes() );
		analyzer->importModuleSymbols( driver.accumulatedModuleSymbols() );
		{
			std::unordered_set<std::string> userImportedIdentifiers;
			for( const ast::nodes::ImportDeclarationSharedPointer& importDecl : programRoot->imports ) {
				if( importDecl == nullptr ) continue;
				if( importDecl->isFromImport && importDecl->importAll == false ) {
					for( const ast::nodes::ImportItem& importItem : importDecl->importItems ) {
						userImportedIdentifiers.insert( importItem.name );
					}
				}
			}
			for( size_t declIndex = 0; declIndex < userDeclCount && declIndex < programRoot->declarations.size(); declIndex++ ) {
				const ast::nodes::DeclarationSharedPointer& decl = programRoot->declarations[ declIndex ];
				if( decl == nullptr ) continue;
				if( decl->kind == ast::Node::Kind::FunctionDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::FunctionDeclaration&>( *decl ).name );
				else if( decl->kind == ast::Node::Kind::ClassDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::ClassDeclaration&>( *decl ).name );
				else if( decl->kind == ast::Node::Kind::StructDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::StructDeclaration&>( *decl ).name );
				else if( decl->kind == ast::Node::Kind::EnumDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::EnumDeclaration&>( *decl ).name );
				else if( decl->kind == ast::Node::Kind::InterfaceDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::InterfaceDeclaration&>( *decl ).name );
				else if( decl->kind == ast::Node::Kind::ConstantDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::ConstantDeclaration&>( *decl ).name );
				else if( decl->kind == ast::Node::Kind::ExternDeclaration ) userImportedIdentifiers.insert( static_cast<const ast::nodes::ExternDeclaration&>( *decl ).name );
			}
			for( const std::string& builtinName : uranite::semantic::qualname::builtinIdentifiers() ) {
				userImportedIdentifiers.insert( builtinName );
			}
			analyzer->setUserImportScope( pathname, userImportedIdentifiers );
		}

		lookup::SourceSharedPointer completionSource;
		if( completion.enabled ) {
			auto position = std::make_shared<lookup::Source::Location>( completion.col + 1, completion.line + 1 );
			completionSource = std::make_shared<lookup::Source>(
				pathname, pathname, position, std::make_shared<lookup::Source::Range>( position, position ) );
			analyzer->clearCompletions();
			analyzer->setCompletionPosition( completionSource );
		}

		if( analyzer->analyze( *programRoot ) == false ) {
			result.diagnostics = engine;
			result.program = programRoot;
			result.analyzer = analyzer;
			if( completion.enabled ) {
				result.completions = analyzer->completions();
				analyzer->clearCompletions();
				analyzer->clearCompletionPosition();
			}
			buildIndex();
			return result;
		}
		if( completion.enabled ) {
			result.completions = analyzer->completions();
			analyzer->clearCompletions();
			analyzer->clearCompletionPosition();
		}

		// Borrow checker
		uranite::semantic::BorrowChecker borrowChecker( engine );
		borrowChecker.check( *programRoot );
		if( engine.hasErrors() ) {
			result.diagnostics = engine;
			result.program = programRoot;
			result.analyzer = analyzer;
			buildIndex();
			return result;
		}

		// HIR lowering + validation
		ir::hir::HIRLowering hirLowering( *analyzer, engine );
		std::shared_ptr<ir::hir::HIRModule> hirModule = hirLowering.lower( *programRoot );
		ir::hir::HIRValidator hirValidator( engine );
		hirValidator.validate( *hirModule );

		// MIR lowering + borrow check (no codegen)
		ir::mir::MIRLowering mirLowering( engine, &analyzer->types() );
		std::shared_ptr<ir::mir::MIRModuleDefinition> mirModule = mirLowering.lower( *hirModule );
		ir::mir::MIRBorrowChecker mirBorrowChecker( engine );
		for( auto& fn : mirModule->functionDefinitions ) {
			if( fn != nullptr ) mirBorrowChecker.check( *fn );
		}

		result.program = programRoot;
		result.analyzer = analyzer;
		result.diagnostics = engine;
		result.success = !engine.hasErrors();
		buildIndex();
		return result;
	}

} // namespace uranite::lsp::semantic
