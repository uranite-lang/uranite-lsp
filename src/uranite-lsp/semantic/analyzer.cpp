
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
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>

#include "uranite/lexer/lexer.hpp"
#include "uranite/parser/parser.hpp"
#include "uranite/semantic/analyzer.hpp"
#include "uranite/semantic/borrow.hpp"
#include "uranite-lsp/semantic/analyzer.hpp"

namespace uranite::lsp {

	static SemanticDiagnostic convertCompilerDiagnostic( const diagnostic::Diagnostic& compilerDiagnostic ) {
		SemanticDiagnostic convertedDiagnostic;

		if( compilerDiagnostic.source != nullptr ) {
			bool rangeHasValues = compilerDiagnostic.source->range != nullptr &&
				( compilerDiagnostic.source->range->begin->line > 0 ||
				  compilerDiagnostic.source->range->begin->column > 0 );

			if( rangeHasValues ) {
				convertedDiagnostic.range.start.line = compilerDiagnostic.source->range->begin->line - 1;
				convertedDiagnostic.range.start.character = compilerDiagnostic.source->range->begin->column > 0
					? compilerDiagnostic.source->range->begin->column - 1 : 0;
				convertedDiagnostic.range.end.line = compilerDiagnostic.source->range->end->line > 0
					? compilerDiagnostic.source->range->end->line - 1 : 0;
				convertedDiagnostic.range.end.character = compilerDiagnostic.source->range->end->column > 0
					? compilerDiagnostic.source->range->end->column - 1 : 0;
			}
			else if( compilerDiagnostic.source->location != nullptr &&
				( compilerDiagnostic.source->location->line > 0 || compilerDiagnostic.source->location->column > 0 ) ) {
				uint32_t diagnosticLine = compilerDiagnostic.source->location->line > 0
					? compilerDiagnostic.source->location->line - 1 : 0;
				uint32_t diagnosticColumn = compilerDiagnostic.source->location->column > 0
					? compilerDiagnostic.source->location->column - 1 : 0;
				convertedDiagnostic.range.start.line = diagnosticLine;
				convertedDiagnostic.range.start.character = diagnosticColumn;
				convertedDiagnostic.range.end.line = diagnosticLine;
				convertedDiagnostic.range.end.character = diagnosticColumn + 1;
			}
		}

		switch( compilerDiagnostic.level ) {
			case diagnostic::Level::Fatal:
			case diagnostic::Level::Error:
				convertedDiagnostic.severity = DiagnosticSeverity::ERROR;
				break;
			case diagnostic::Level::Warning:
				convertedDiagnostic.severity = DiagnosticSeverity::WARNING;
				break;
			case diagnostic::Level::Note:
				convertedDiagnostic.severity = DiagnosticSeverity::INFORMATION;
				break;
		}

		convertedDiagnostic.source = "uranite-semantic";
		convertedDiagnostic.message = compilerDiagnostic.messages;
		convertedDiagnostic.hinting = compilerDiagnostic.hinting;

		return convertedDiagnostic;
	}

	static std::string resolveModulesDirectory( const std::string& workspaceRootPath, const std::string& documentFilePath ) {
		auto searchUpwardsForStdlibs = []( std::filesystem::path searchDirectory ) -> std::string {
			for( int ancestorDepth = 0; ancestorDepth < 10; ++ancestorDepth ) {
				std::filesystem::path candidateStdlibsPath = searchDirectory / "stdlibs";
				if( std::filesystem::exists( candidateStdlibsPath ) && std::filesystem::is_directory( candidateStdlibsPath ) ) {
					return candidateStdlibsPath.string();
				}
				std::filesystem::path parentDirectory = searchDirectory.parent_path();
				if( parentDirectory == searchDirectory ) {
					break;
				}
				searchDirectory = parentDirectory;
			}
			return "";
		};

		if( documentFilePath.empty() == false ) {
			std::string documentBasedResult = searchUpwardsForStdlibs( std::filesystem::path( documentFilePath ).parent_path() );
			if( documentBasedResult.empty() == false ) {
				return documentBasedResult;
			}
		}

		if( workspaceRootPath.empty() == false ) {
			std::string workspaceBasedResult = searchUpwardsForStdlibs( std::filesystem::path( workspaceRootPath ) );
			if( workspaceBasedResult.empty() == false ) {
				return workspaceBasedResult;
			}
		}

		std::vector<std::string> candidateDirectories = {
			"/usr/local/lib/uranite/stdlibs",
			"/usr/lib/uranite/stdlibs"
		};

		for( const std::string& candidatePath : candidateDirectories ) {
			if( std::filesystem::exists( candidatePath ) ) {
				return candidatePath;
			}
		}

		return "";
	}

	SemanticAnalysisResult performSemanticAnalysis(
		const std::string& documentContent,
		const std::string& documentFilePath,
		const std::string& workspaceRootPath
	) {
		SemanticAnalysisResult analysisResult;

		spdlog::info( "[LSP-SEMA] performSemanticAnalysis: docPath='{}' wsRoot='{}'", documentFilePath, workspaceRootPath );

		try {
			diagnostic::Engine diagnosticEngine( 200, 200 );
			diagnosticEngine.setConsolPrintable( false );

			std::string sourceFileName = std::filesystem::path( documentFilePath ).filename().string();

			lexer::Lexer sourceTokenizer( documentContent, sourceFileName, diagnosticEngine );
			diagnosticEngine.setSourceLines( sourceTokenizer.sourceLines() );
			std::vector<token::Token> tokenStream = sourceTokenizer.tokenize();

			spdlog::info( "[LSP-SEMA] lexed {} tokens, {} lex diagnostics", tokenStream.size(), diagnosticEngine.diagnostics().size() );

			for( const diagnostic::Diagnostic& lexerDiagnostic : diagnosticEngine.diagnostics() ) {
				analysisResult.semanticDiagnostics.push_back( convertCompilerDiagnostic( lexerDiagnostic ) );
			}

			parser::Parser astParser( std::move( tokenStream ), diagnosticEngine );
			ast::nodes::ProgramSharedPointer parsedProgram = astParser.parse();

			size_t preLexParserDiagnosticCount = analysisResult.semanticDiagnostics.size();
			for( size_t diagnosticIndex = preLexParserDiagnosticCount; diagnosticIndex < diagnosticEngine.diagnostics().size(); ++diagnosticIndex ) {
				analysisResult.semanticDiagnostics.push_back( convertCompilerDiagnostic( diagnosticEngine.diagnostics()[diagnosticIndex] ) );
			}

			if( parsedProgram == nullptr ) {
				spdlog::warn( "[LSP-SEMA] parser returned null program" );
				analysisResult.analysisSucceeded = false;
				return analysisResult;
			}

			spdlog::info( "[LSP-SEMA] parsed {} declarations", parsedProgram->declarations.size() );

			analysisResult.analyzedProgram = parsedProgram;
			collectLocalVariablesFromProgram( parsedProgram, analysisResult.localVariables );

			std::string modulesDirectory = resolveModulesDirectory( workspaceRootPath, documentFilePath );

			spdlog::info( "[LSP-SEMA] modulesDirectory='{}' localVars={}", modulesDirectory, analysisResult.localVariables.size() );

			compiler::Options driverOptions;
			driverOptions.output.source = documentFilePath;
			driverOptions.maximumErrorCount = 200;
			driverOptions.modulesPath = modulesDirectory;

			compiler::Driver compilationDriver( driverOptions );

			spdlog::info( "[LSP-SEMA] loading prelude..." );
			compilationDriver.loadPrelude( *parsedProgram );

			std::string resolvedModulesDirectory = compilationDriver.findModulesDirectory();
			if( resolvedModulesDirectory.empty() == false ) {
				std::filesystem::path argsModulePath =
					std::filesystem::path( resolvedModulesDirectory ) / "collection" / "args.urn";
				if( std::filesystem::exists( argsModulePath ) ) {
					compilationDriver.loadModule( argsModulePath.string(), *parsedProgram );
				}
				std::filesystem::path kwargsModulePath =
					std::filesystem::path( resolvedModulesDirectory ) / "collection" / "kwargs.urn";
				if( std::filesystem::exists( kwargsModulePath ) ) {
					compilationDriver.loadModule( kwargsModulePath.string(), *parsedProgram );
				}
				std::filesystem::path mathErrorsPath =
					std::filesystem::path( resolvedModulesDirectory ) / "math" / "errors.urn";
				if( std::filesystem::exists( mathErrorsPath ) ) {
					compilationDriver.loadModule( mathErrorsPath.string(), *parsedProgram );
				}

				const std::vector<std::string> literalTypeModules = {
					"language/string.urn",
					"language/boolean.urn",
					"language/char.urn",
					"language/i64.urn",
					"language/f64.urn",
					"language/none.urn",
					"collection/array-list.urn",
					"collection/hash-map.urn",
					"collection/hash-set.urn",
					"collection/tuple.urn"
				};
				for( const std::string& literalModule : literalTypeModules ) {
					std::filesystem::path literalModulePath =
						std::filesystem::path( resolvedModulesDirectory ) / literalModule;
					if( std::filesystem::exists( literalModulePath ) ) {
						compilationDriver.loadModule( literalModulePath.string(), *parsedProgram );
					}
				}
			}

			spdlog::info( "[LSP-SEMA] resolving imports..." );
			compilationDriver.resolveImports( *parsedProgram );
			spdlog::info( "[LSP-SEMA] imports resolved, {} modules loaded", compilationDriver.getModules().size() );

			for( const ast::nodes::ImportDeclarationSharedPointer& importDeclaration : parsedProgram->imports ) {
				if( importDeclaration == nullptr || importDeclaration->isFromImport == false || importDeclaration->importAll ) {
					continue;
				}
				std::string resolvedImportModulePath = compilationDriver.resolveModulePath( importDeclaration->path );
				if( resolvedImportModulePath.empty() ) {
					continue;
				}
				std::string canonicalImportPath;
				try {
					canonicalImportPath = std::filesystem::canonical( resolvedImportModulePath ).string();
				}
				catch( const std::exception& ) {
					continue;
				}
				const std::unordered_map<std::string, compiler::ModuleInfo>& loadedModuleCache = compilationDriver.getModules();
				std::unordered_map<std::string, compiler::ModuleInfo>::const_iterator moduleIterator = loadedModuleCache.find( canonicalImportPath );
				if( moduleIterator == loadedModuleCache.end() || moduleIterator->second.program == nullptr ) {
					continue;
				}
				std::set<std::string> availableEntityNames;
				for( const ast::nodes::DeclarationSharedPointer& moduleDeclaration : moduleIterator->second.program->declarations ) {
					if( moduleDeclaration == nullptr ) continue;
					std::string declaredEntityName;
					if( moduleDeclaration->kind == ast::Node::Kind::FunctionDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::FunctionDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::ClassDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::ClassDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::StructDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::StructDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::EnumDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::EnumDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::InterfaceDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::InterfaceDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::ExternDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::ExternDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::TypeAliasDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::TypeAliasDeclaration&>( *moduleDeclaration ).name;
					}
					else if( moduleDeclaration->kind == ast::Node::Kind::ConstantDeclaration ) {
						declaredEntityName = static_cast<ast::nodes::ConstantDeclaration&>( *moduleDeclaration ).name;
					}
					if( declaredEntityName.empty() == false ) {
						availableEntityNames.insert( declaredEntityName );
					}
				}
				std::string formattedModulePath;
				for( size_t pathSegmentIndex = 0; pathSegmentIndex < importDeclaration->path.size(); ++pathSegmentIndex ) {
					if( pathSegmentIndex > 0 ) formattedModulePath += ".";
					formattedModulePath += importDeclaration->path[pathSegmentIndex];
				}
				for( const ast::nodes::ImportItem& importedItem : importDeclaration->importItems ) {
					if( availableEntityNames.count( importedItem.name ) == 0 ) {
						SemanticDiagnostic undefinedImportDiagnostic;
						undefinedImportDiagnostic.severity = DiagnosticSeverity::ERROR;
						undefinedImportDiagnostic.source = "uranite-lsp";
						undefinedImportDiagnostic.message = fmt::format(
							"'{}' is not defined in module '{}'", importedItem.name, formattedModulePath
						);
						if( importDeclaration->source != nullptr && importDeclaration->source->location != nullptr ) {
							undefinedImportDiagnostic.range.start.line = importDeclaration->source->location->line > 0
								? importDeclaration->source->location->line - 1 : 0;
							undefinedImportDiagnostic.range.start.character = 0;
							undefinedImportDiagnostic.range.end.line = undefinedImportDiagnostic.range.start.line;
							undefinedImportDiagnostic.range.end.character = 1000;
						}
						analysisResult.semanticDiagnostics.push_back( undefinedImportDiagnostic );
					}
				}
			}

			if( resolvedModulesDirectory.empty() == false ) {
				bool programNeedsAsyncRuntime = false;
				for( const ast::nodes::DeclarationSharedPointer& programDeclaration : parsedProgram->declarations ) {
					if( programDeclaration == nullptr ) {
						continue;
					}
					if( programDeclaration->kind == ast::Node::Kind::FunctionDeclaration ) {
						ast::nodes::FunctionDeclaration& functionDeclaration =
							static_cast<ast::nodes::FunctionDeclaration&>( *programDeclaration );
						if( functionDeclaration.isAsync ) {
							programNeedsAsyncRuntime = true;
							break;
						}
					}
					std::vector<ast::nodes::DeclarationSharedPointer>* methodDeclarations = nullptr;
					if( programDeclaration->kind == ast::Node::Kind::ClassDeclaration ) {
						methodDeclarations = &static_cast<ast::nodes::ClassDeclaration&>( *programDeclaration ).methods;
					}
					else if( programDeclaration->kind == ast::Node::Kind::StructDeclaration ) {
						methodDeclarations = &static_cast<ast::nodes::StructDeclaration&>( *programDeclaration ).methods;
					}
					else if( programDeclaration->kind == ast::Node::Kind::InterfaceDeclaration ) {
						methodDeclarations = &static_cast<ast::nodes::InterfaceDeclaration&>( *programDeclaration ).methods;
					}
					if( methodDeclarations != nullptr ) {
						for( const ast::nodes::DeclarationSharedPointer& methodDeclaration : *methodDeclarations ) {
							if( methodDeclaration != nullptr && methodDeclaration->kind == ast::Node::Kind::FunctionDeclaration ) {
								if( static_cast<ast::nodes::FunctionDeclaration&>( *methodDeclaration ).isAsync ) {
									programNeedsAsyncRuntime = true;
									break;
								}
							}
						}
						if( programNeedsAsyncRuntime ) {
							break;
						}
					}
				}
				if( programNeedsAsyncRuntime ) {
					std::filesystem::path asyncRuntimePath =
						std::filesystem::path( resolvedModulesDirectory ) / "async" / "runtime.urn";
					if( std::filesystem::exists( asyncRuntimePath ) ) {
						compilationDriver.loadModule( asyncRuntimePath.string(), *parsedProgram );
					}
				}
			}

			analysisResult.loadedModules = compilationDriver.getModules();

			diagnostic::Engine semanticDiagnosticEngine( 200, 200 );
			semanticDiagnosticEngine.setConsolPrintable( false );
			semanticDiagnosticEngine.setSourceLines( sourceTokenizer.sourceLines() );

			semantic::Analyzer semanticAnalyzer( semanticDiagnosticEngine );

			for( const auto& moduleEntry : analysisResult.loadedModules ) {
				semanticAnalyzer.importModuleTypes( moduleEntry.second.analyzedTypes );
				semanticAnalyzer.importModuleSymbols( moduleEntry.second.analyzedSymbols );
			}

			bool semanticAnalysisSucceeded = false;
			try {
				spdlog::info( "[LSP-SEMA] running semantic analysis..." );
				semanticAnalysisSucceeded = semanticAnalyzer.analyze( *parsedProgram );
				spdlog::info( "[LSP-SEMA] semantic analysis succeeded={}", semanticAnalysisSucceeded );
			}
			catch( const diagnostic::DiagnosticLimitReachedError& ) {
				spdlog::warn( "[LSP-SEMA] semantic analysis hit diagnostic limit" );
				semanticAnalysisSucceeded = false;
			}
			catch( const std::exception& analysisException ) {
				spdlog::warn( "[LSP-SEMA] semantic analysis exception: {}", analysisException.what() );
				semanticAnalysisSucceeded = false;
			}

			size_t filteredDiagnosticCount = 0;
			size_t skippedDiagnosticCount = 0;
			for( const diagnostic::Diagnostic& semanticDiagnostic : semanticDiagnosticEngine.diagnostics() ) {
				if( semanticDiagnostic.source != nullptr ) {
					bool diagnosticFromCurrentFile = (
						semanticDiagnostic.source->filename == sourceFileName ||
						semanticDiagnostic.source->filename == "<unknown>" ||
						semanticDiagnostic.source->pathname == documentFilePath ||
						semanticDiagnostic.source->pathname == "<unknown>"
					);
					if( diagnosticFromCurrentFile == false ) {
						skippedDiagnosticCount++;
						continue;
					}
				}
				filteredDiagnosticCount++;
				analysisResult.semanticDiagnostics.push_back( convertCompilerDiagnostic( semanticDiagnostic ) );
			}

			spdlog::info( "[LSP-SEMA] sema diagnostics: total={} filtered_in={} skipped={}",
				semanticDiagnosticEngine.diagnostics().size(), filteredDiagnosticCount, skippedDiagnosticCount );

			analysisResult.globalScope = semanticAnalyzer.globalScope();
			analysisResult.typeRegistry = std::make_shared<semantic::Registry>( semanticAnalyzer.types() );
			analysisResult.resolvedTypes = semanticAnalyzer.getRegisteredTypes();
			analysisResult.resolvedSymbols = semanticAnalyzer.getRegisteredSymbols();
			analysisResult.analyzerCompletions = semanticAnalyzer.completions();
			if( parsedProgram->module != nullptr ) {
				analysisResult.packageName = parsedProgram->module->name;
			}

			if( semanticAnalysisSucceeded ) {
				diagnostic::Engine borrowDiagnosticEngine( 200, 200 );
				borrowDiagnosticEngine.setConsolPrintable( false );
				borrowDiagnosticEngine.setSourceLines( sourceTokenizer.sourceLines() );

				try {
					semantic::BorrowChecker borrowChecker( borrowDiagnosticEngine );
					borrowChecker.check( *parsedProgram );
				}
				catch( const diagnostic::DiagnosticLimitReachedError& ) {
				}
				catch( const std::exception& borrowException ) {
					spdlog::warn( "Borrow checker exception: {}", borrowException.what() );
				}
				for( const diagnostic::Diagnostic& borrowDiagnostic : borrowDiagnosticEngine.diagnostics() ) {
					if( borrowDiagnostic.source != nullptr ) {
						bool diagnosticFromCurrentFile = (
							borrowDiagnostic.source->filename == sourceFileName ||
							borrowDiagnostic.source->filename == "<unknown>" ||
							borrowDiagnostic.source->pathname == documentFilePath ||
							borrowDiagnostic.source->pathname == "<unknown>"
						);
						if( diagnosticFromCurrentFile == false ) {
							continue;
						}
					}
					analysisResult.semanticDiagnostics.push_back( convertCompilerDiagnostic( borrowDiagnostic ) );
				}
			}

			analysisResult.analysisSucceeded = semanticAnalysisSucceeded;
		}
		catch( const std::exception& topLevelException ) {
			spdlog::warn( "Semantic analysis pipeline failed: {}", topLevelException.what() );
			analysisResult.analysisSucceeded = false;
		}

		return analysisResult;
	}

	static std::string loadSourceContentForDeclaration(
		const lookup::SourceSharedPointer& declarationSource,
		const std::string& currentDocumentContent,
		const SemanticAnalysisResult* analysisResult
	) {
		if( declarationSource == nullptr ) return "";

		std::string sourceFilePath = declarationSource->filename;
		if( sourceFilePath.empty() ) return currentDocumentContent;

		bool isBasenameOnly = sourceFilePath.find( '/' ) == std::string::npos &&
			sourceFilePath.find( '\\' ) == std::string::npos;

		if( isBasenameOnly && analysisResult != nullptr ) {
			for( const auto& moduleEntry : analysisResult->loadedModules ) {
				std::string moduleBasename = std::filesystem::path( moduleEntry.first ).filename().string();
				if( moduleBasename == sourceFilePath ) {
					sourceFilePath = moduleEntry.first;
					break;
				}
			}
		}

		if( std::filesystem::is_regular_file( sourceFilePath ) ) {
			std::ifstream sourceFileStream( sourceFilePath );
			if( sourceFileStream.is_open() ) {
				return std::string(
					std::istreambuf_iterator<char>( sourceFileStream ),
					std::istreambuf_iterator<char>()
				);
			}
		}

		return currentDocumentContent;
	}

	static std::string extractDoccommentForMethod(
		const std::vector<ast::nodes::DeclarationSharedPointer>& methodDeclarations,
		const std::string& methodName,
		const std::string& currentDocumentContent,
		const SemanticAnalysisResult* analysisResult
	) {
		for( const ast::nodes::DeclarationSharedPointer& methodDeclaration : methodDeclarations ) {
			if( methodDeclaration == nullptr ) continue;
			std::shared_ptr<ast::nodes::FunctionDeclaration> functionDeclaration =
				std::dynamic_pointer_cast<ast::nodes::FunctionDeclaration>( methodDeclaration );
			if( functionDeclaration == nullptr || functionDeclaration->name != methodName ) continue;

			if( functionDeclaration->source == nullptr || functionDeclaration->source->location == nullptr ) continue;

			uint32_t declarationLine = functionDeclaration->source->location->line;
			if( declarationLine == 0 ) continue;

			std::string sourceContent = loadSourceContentForDeclaration(
				functionDeclaration->source, currentDocumentContent, analysisResult
			);
			if( sourceContent.empty() ) continue;

			std::string doccomment = extractDoccommentForDeclaration( sourceContent, declarationLine - 1 );
			if( doccomment.empty() == false ) return doccomment;
		}
		return "";
	}

	std::vector<MemberCompletionEntry> collectMemberCompletions(
		const semantic::TypeSharedPointer& objectType,
		const std::string& currentDocumentContent,
		const SemanticAnalysisResult* analysisResult,
		bool isInternalAccess
	) {
		std::vector<MemberCompletionEntry> memberEntries;

		if( objectType == nullptr ) {
			return memberEntries;
		}

		semantic::OptionalTypeSharedPointer optionalType = std::dynamic_pointer_cast<semantic::OptionalType>( objectType );
		if( optionalType != nullptr && optionalType->inner != nullptr ) {
			return collectMemberCompletions( optionalType->inner, currentDocumentContent, analysisResult, isInternalAccess );
		}

		semantic::ClassTypeSharedPointer classType = std::dynamic_pointer_cast<semantic::ClassType>( objectType );
		if( classType != nullptr ) {
			for( const semantic::FieldInfo& classField : classType->fields ) {
				if( isInternalAccess == false && classField.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry fieldEntry;
				fieldEntry.memberName = classField.name;
				fieldEntry.memberDetail = classField.type != nullptr ? classField.type->toString() : "?";
				fieldEntry.completionKind = CompletionItemKind::FIELD;
				memberEntries.push_back( fieldEntry );
			}
			for( const semantic::MethodInfo& classMethod : classType->methods ) {
				if( isInternalAccess == false && classMethod.access != ast::AccessModifier::Public ) {
					continue;
				}
				if( classMethod.name == classType->name ) {
					continue;
				}
				MemberCompletionEntry methodEntry;
				methodEntry.memberName = classMethod.name;
				methodEntry.completionKind = CompletionItemKind::METHOD;

				semantic::FunctionTypeSharedPointer methodFunctionType =
					std::dynamic_pointer_cast<semantic::FunctionType>( classMethod.type );
				if( methodFunctionType != nullptr ) {
					methodEntry.memberDetail = methodFunctionType->toString();
					std::string snippetParameters;
					int snippetTabStop = 1;
					for( size_t parameterIndex = 0; parameterIndex < methodFunctionType->parameterNames.size(); ++parameterIndex ) {
						if( parameterIndex > 0 ) {
							snippetParameters += ", ";
						}
						snippetParameters += "${" + std::to_string( snippetTabStop ) + ":" +
							methodFunctionType->parameterNames[parameterIndex] + "}";
						snippetTabStop++;
					}
					methodEntry.snippetInsertText = classMethod.name + "(" + snippetParameters + ")$0";
				}
				else {
					methodEntry.memberDetail = classMethod.type != nullptr ? classMethod.type->toString() : "()";
				}

				if( classType->astDeclaration != nullptr ) {
					methodEntry.memberDocumentation = extractDoccommentForMethod(
						classType->astDeclaration->methods, classMethod.name,
						currentDocumentContent, analysisResult
					);
				}

				memberEntries.push_back( methodEntry );
			}

			if( classType->baseClass != nullptr ) {
				std::vector<MemberCompletionEntry> baseClassMembers = collectMemberCompletions(
					classType->baseClass, currentDocumentContent, analysisResult, isInternalAccess
				);
				memberEntries.insert( memberEntries.end(), baseClassMembers.begin(), baseClassMembers.end() );
			}

			return memberEntries;
		}

		semantic::StructTypeSharedPointer structType = std::dynamic_pointer_cast<semantic::StructType>( objectType );
		if( structType != nullptr ) {
			for( const semantic::FieldInfo& structField : structType->fields ) {
				if( isInternalAccess == false && structField.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry fieldEntry;
				fieldEntry.memberName = structField.name;
				fieldEntry.memberDetail = structField.type != nullptr ? structField.type->toString() : "?";
				fieldEntry.completionKind = CompletionItemKind::FIELD;
				memberEntries.push_back( fieldEntry );
			}
			for( const semantic::MethodInfo& structMethod : structType->methods ) {
				if( isInternalAccess == false && structMethod.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry methodEntry;
				methodEntry.memberName = structMethod.name;
				methodEntry.completionKind = CompletionItemKind::METHOD;
				methodEntry.memberDetail = structMethod.type != nullptr ? structMethod.type->toString() : "()";
				if( structType->astDeclaration != nullptr ) {
					methodEntry.memberDocumentation = extractDoccommentForMethod(
						structType->astDeclaration->methods, structMethod.name,
						currentDocumentContent, analysisResult
					);
				}
				memberEntries.push_back( methodEntry );
			}
			return memberEntries;
		}

		semantic::EnumTypeSharedPointer enumType = std::dynamic_pointer_cast<semantic::EnumType>( objectType );
		if( enumType != nullptr ) {
			for( const semantic::EnumVariantInfo& enumVariant : enumType->variants ) {
				MemberCompletionEntry variantEntry;
				variantEntry.memberName = enumVariant.name;
				variantEntry.memberDetail = "variant";
				variantEntry.completionKind = CompletionItemKind::ENUM_MEMBER;
				memberEntries.push_back( variantEntry );
			}
			for( const semantic::MethodInfo& enumMethod : enumType->methods ) {
				if( isInternalAccess == false && enumMethod.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry methodEntry;
				methodEntry.memberName = enumMethod.name;
				methodEntry.completionKind = CompletionItemKind::METHOD;
				methodEntry.memberDetail = enumMethod.type != nullptr ? enumMethod.type->toString() : "()";
				memberEntries.push_back( methodEntry );
			}
			return memberEntries;
		}

		semantic::InterfaceTypeSharedPointer interfaceType = std::dynamic_pointer_cast<semantic::InterfaceType>( objectType );
		if( interfaceType != nullptr ) {
			for( const semantic::MethodInfo& interfaceMethod : interfaceType->methods ) {
				if( isInternalAccess == false && interfaceMethod.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry methodEntry;
				methodEntry.memberName = interfaceMethod.name;
				methodEntry.completionKind = CompletionItemKind::METHOD;
				methodEntry.memberDetail = interfaceMethod.type != nullptr ? interfaceMethod.type->toString() : "()";
				memberEntries.push_back( methodEntry );
			}
			return memberEntries;
		}

		semantic::TraitTypeSharedPointer traitType = std::dynamic_pointer_cast<semantic::TraitType>( objectType );
		if( traitType != nullptr ) {
			for( const semantic::FieldInfo& traitField : traitType->fields ) {
				if( isInternalAccess == false && traitField.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry fieldEntry;
				fieldEntry.memberName = traitField.name;
				fieldEntry.memberDetail = traitField.type != nullptr ? traitField.type->toString() : "?";
				fieldEntry.completionKind = CompletionItemKind::FIELD;
				memberEntries.push_back( fieldEntry );
			}
			for( const semantic::MethodInfo& traitMethod : traitType->methods ) {
				if( isInternalAccess == false && traitMethod.access != ast::AccessModifier::Public ) {
					continue;
				}
				MemberCompletionEntry methodEntry;
				methodEntry.memberName = traitMethod.name;
				methodEntry.completionKind = CompletionItemKind::METHOD;
				methodEntry.memberDetail = traitMethod.type != nullptr ? traitMethod.type->toString() : "()";
				memberEntries.push_back( methodEntry );
			}
			return memberEntries;
		}

		return memberEntries;
	}

	static void scanModuleDirectory(
		const std::string& baseDirectory,
		const std::string& currentPathPrefix,
		std::vector<ImportPathSuggestion>& collectedSuggestions
	) {
		if( std::filesystem::exists( baseDirectory ) == false ) {
			return;
		}

		for( const auto& directoryEntry : std::filesystem::directory_iterator( baseDirectory ) ) {
			std::string entryName = directoryEntry.path().filename().string();

			if( directoryEntry.is_directory() ) {
				std::string subdirectoryModulePath = currentPathPrefix.empty()
					? entryName : currentPathPrefix + "." + entryName;

				ImportPathSuggestion directorySuggestion;
				directorySuggestion.modulePath = subdirectoryModulePath;
				directorySuggestion.displayLabel = entryName;
				directorySuggestion.moduleDescription = "module package";
				collectedSuggestions.push_back( directorySuggestion );

				scanModuleDirectory( directoryEntry.path().string(), subdirectoryModulePath, collectedSuggestions );
			}
			else if( directoryEntry.path().extension() == ".urn" ) {
				std::string moduleFileName = directoryEntry.path().stem().string();
				if( moduleFileName == "__mod__" ) {
					continue;
				}
				std::string fullModulePath = currentPathPrefix.empty()
					? moduleFileName : currentPathPrefix + "." + moduleFileName;

				ImportPathSuggestion moduleSuggestion;
				moduleSuggestion.modulePath = fullModulePath;
				moduleSuggestion.displayLabel = moduleFileName;
				moduleSuggestion.moduleDescription = "module";
				collectedSuggestions.push_back( moduleSuggestion );
			}
		}
	}

	std::vector<ImportPathSuggestion> collectImportPathSuggestions(
		const std::string& partialImportPath,
		const std::string& workspaceRootPath
	) {
		std::vector<ImportPathSuggestion> allSuggestions;

		std::string modulesDirectory = resolveModulesDirectory( workspaceRootPath, "" );
		if( modulesDirectory.empty() ) {
			return allSuggestions;
		}

		scanModuleDirectory( modulesDirectory, "uranite", allSuggestions );

		if( partialImportPath.empty() ) {
			return allSuggestions;
		}

		std::vector<ImportPathSuggestion> filteredSuggestions;
		for( const ImportPathSuggestion& suggestion : allSuggestions ) {
			if( suggestion.modulePath.find( partialImportPath ) == 0 ||
				suggestion.displayLabel.find( partialImportPath ) != std::string::npos ) {
				filteredSuggestions.push_back( suggestion );
			}
		}

		return filteredSuggestions;
	}

	ResolvedSymbolInfo resolveSymbolAtPosition(
		const SemanticAnalysisResult& analysisResult,
		const std::string& identifierText,
		const std::string& currentDocumentPath,
		uint32_t cursorLine
	) {
		ResolvedSymbolInfo resolvedInfo;
		resolvedInfo.symbolName = identifierText;

		if( analysisResult.globalScope != nullptr ) {
			semantic::SymbolSharedPointer foundSymbol = analysisResult.globalScope->lookup( identifierText );
			if( foundSymbol != nullptr ) {
				resolvedInfo.symbolName = foundSymbol->name;
				resolvedInfo.symbolKind = foundSymbol->kind;
				resolvedInfo.resolvedType = foundSymbol->typeref;

				if( foundSymbol->typeref != nullptr ) {
					resolvedInfo.typeName = foundSymbol->typeref->toString();
					resolvedInfo.qualifiedName = foundSymbol->typeref->qualified;
				}

				if( foundSymbol->source != nullptr ) {
					resolvedInfo.filePath = foundSymbol->source->filename;
					bool rangeHasValidPosition = (
						foundSymbol->source->range != nullptr &&
						foundSymbol->source->range->begin != nullptr &&
						foundSymbol->source->range->begin->line > 0
					);
					if( rangeHasValidPosition ) {
						resolvedInfo.definitionLine = foundSymbol->source->range->begin->line - 1;
						resolvedInfo.definitionColumn = foundSymbol->source->range->begin->column > 0
							? foundSymbol->source->range->begin->column - 1 : 0;
						resolvedInfo.definitionEndLine = foundSymbol->source->range->end->line > 0
							? foundSymbol->source->range->end->line - 1 : 0;
						resolvedInfo.definitionEndColumn = foundSymbol->source->range->end->column > 0
							? foundSymbol->source->range->end->column - 1 : 0;
					}
					else if( foundSymbol->source->location != nullptr && foundSymbol->source->location->line > 0 ) {
						resolvedInfo.definitionLine = foundSymbol->source->location->line - 1;
						resolvedInfo.definitionColumn = foundSymbol->source->location->column > 0
							? foundSymbol->source->location->column - 1 : 0;
						resolvedInfo.definitionEndLine = resolvedInfo.definitionLine;
						resolvedInfo.definitionEndColumn = resolvedInfo.definitionColumn + static_cast<uint32_t>( identifierText.size() );
					}
				}

				return resolvedInfo;
			}
		}

		semantic::TypeSharedPointer localVariableType = resolveLocalVariableType(
			analysisResult, identifierText, cursorLine
		);
		if( localVariableType != nullptr ) {
			resolvedInfo.symbolKind = semantic::Symbol::Kind::Variable;
			resolvedInfo.resolvedType = localVariableType;
			resolvedInfo.typeName = localVariableType->toString();
			resolvedInfo.qualifiedName = localVariableType->qualified;

			const LocalVariableInfo* bestLocalMatch = nullptr;
			for( const LocalVariableInfo& localVariable : analysisResult.localVariables ) {
				if( localVariable.variableName == identifierText && localVariable.declarationLine <= cursorLine ) {
					if( bestLocalMatch == nullptr || localVariable.declarationLine > bestLocalMatch->declarationLine ) {
						bestLocalMatch = &localVariable;
					}
				}
			}
			if( bestLocalMatch != nullptr ) {
				resolvedInfo.filePath = currentDocumentPath;
				resolvedInfo.definitionLine = bestLocalMatch->declarationLine;
				resolvedInfo.definitionColumn = bestLocalMatch->declarationColumn;
				resolvedInfo.definitionEndLine = bestLocalMatch->declarationLine;
				resolvedInfo.definitionEndColumn = bestLocalMatch->declarationColumn +
					static_cast<uint32_t>( identifierText.size() );
			}

			return resolvedInfo;
		}

		auto resolvedSymbolIterator = analysisResult.resolvedSymbols.find( identifierText );
		if( resolvedSymbolIterator != analysisResult.resolvedSymbols.end() && resolvedSymbolIterator->second.empty() == false ) {
			semantic::SymbolSharedPointer registeredSymbol = resolvedSymbolIterator->second.front();
			resolvedInfo.symbolKind = registeredSymbol->kind;
			resolvedInfo.resolvedType = registeredSymbol->typeref;
			if( registeredSymbol->typeref != nullptr ) {
				resolvedInfo.typeName = registeredSymbol->typeref->toString();
				resolvedInfo.qualifiedName = registeredSymbol->typeref->qualified;
			}
			if( registeredSymbol->source != nullptr ) {
				resolvedInfo.filePath = registeredSymbol->source->filename;
				bool rangeHasValidPosition = (
					registeredSymbol->source->range != nullptr &&
					registeredSymbol->source->range->begin != nullptr &&
					registeredSymbol->source->range->begin->line > 0
				);
				if( rangeHasValidPosition ) {
					resolvedInfo.definitionLine = registeredSymbol->source->range->begin->line - 1;
					resolvedInfo.definitionColumn = registeredSymbol->source->range->begin->column > 0
						? registeredSymbol->source->range->begin->column - 1 : 0;
					resolvedInfo.definitionEndLine = registeredSymbol->source->range->end->line > 0
						? registeredSymbol->source->range->end->line - 1 : 0;
					resolvedInfo.definitionEndColumn = registeredSymbol->source->range->end->column > 0
						? registeredSymbol->source->range->end->column - 1 : 0;
				}
				else if( registeredSymbol->source->location != nullptr && registeredSymbol->source->location->line > 0 ) {
					resolvedInfo.definitionLine = registeredSymbol->source->location->line - 1;
					resolvedInfo.definitionColumn = registeredSymbol->source->location->column > 0
						? registeredSymbol->source->location->column - 1 : 0;
					resolvedInfo.definitionEndLine = resolvedInfo.definitionLine;
					resolvedInfo.definitionEndColumn = resolvedInfo.definitionColumn + static_cast<uint32_t>( identifierText.size() );
				}
			}
			return resolvedInfo;
		}

		auto resolvedTypeIterator = analysisResult.resolvedTypes.find( identifierText );
		if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
			semantic::TypeSharedPointer foundType = resolvedTypeIterator->second;
			resolvedInfo.symbolKind = semantic::Symbol::Kind::Type;
			resolvedInfo.resolvedType = foundType;
			resolvedInfo.typeName = foundType->toString();
			resolvedInfo.qualifiedName = foundType->qualified;

			lookup::SourceSharedPointer typeSource = nullptr;
			if( auto classType = std::dynamic_pointer_cast<semantic::ClassType>( foundType ) ) {
				if( classType->astDeclaration != nullptr ) typeSource = classType->astDeclaration->source;
			}
			else if( auto structType = std::dynamic_pointer_cast<semantic::StructType>( foundType ) ) {
				if( structType->astDeclaration != nullptr ) typeSource = structType->astDeclaration->source;
			}
			else if( auto enumType = std::dynamic_pointer_cast<semantic::EnumType>( foundType ) ) {
				if( enumType->astDeclaration != nullptr ) typeSource = enumType->astDeclaration->source;
			}
			else if( auto interfaceType = std::dynamic_pointer_cast<semantic::InterfaceType>( foundType ) ) {
				if( interfaceType->astDeclaration != nullptr ) typeSource = interfaceType->astDeclaration->source;
			}
			else if( auto traitType = std::dynamic_pointer_cast<semantic::TraitType>( foundType ) ) {
				if( traitType->astDeclaration != nullptr ) typeSource = traitType->astDeclaration->source;
			}

			if( typeSource != nullptr ) {
				resolvedInfo.filePath = typeSource->filename;
				bool rangeHasValidPosition = (
					typeSource->range != nullptr &&
					typeSource->range->begin != nullptr &&
					typeSource->range->begin->line > 0
				);
				if( rangeHasValidPosition ) {
					resolvedInfo.definitionLine = typeSource->range->begin->line - 1;
					resolvedInfo.definitionColumn = typeSource->range->begin->column > 0
						? typeSource->range->begin->column - 1 : 0;
					resolvedInfo.definitionEndLine = typeSource->range->end->line > 0
						? typeSource->range->end->line - 1 : 0;
					resolvedInfo.definitionEndColumn = typeSource->range->end->column > 0
						? typeSource->range->end->column - 1 : 0;
				}
				else if( typeSource->location != nullptr && typeSource->location->line > 0 ) {
					resolvedInfo.definitionLine = typeSource->location->line - 1;
					resolvedInfo.definitionColumn = typeSource->location->column > 0
						? typeSource->location->column - 1 : 0;
					resolvedInfo.definitionEndLine = resolvedInfo.definitionLine;
					resolvedInfo.definitionEndColumn = resolvedInfo.definitionColumn + static_cast<uint32_t>( identifierText.size() );
				}
			}

			return resolvedInfo;
		}

		for( const auto& moduleEntry : analysisResult.loadedModules ) {
			auto moduleSymbolIterator = moduleEntry.second.analyzedSymbols.find( identifierText );
			if( moduleSymbolIterator != moduleEntry.second.analyzedSymbols.end() && moduleSymbolIterator->second.empty() == false ) {
				semantic::SymbolSharedPointer moduleSymbol = moduleSymbolIterator->second.front();
				resolvedInfo.symbolKind = moduleSymbol->kind;
				resolvedInfo.resolvedType = moduleSymbol->typeref;
				if( moduleSymbol->typeref != nullptr ) {
					resolvedInfo.typeName = moduleSymbol->typeref->toString();
					resolvedInfo.qualifiedName = moduleSymbol->typeref->qualified;
				}
				if( moduleSymbol->source != nullptr ) {
					resolvedInfo.filePath = moduleSymbol->source->filename;
					if( moduleSymbol->source->range != nullptr ) {
						resolvedInfo.definitionLine = moduleSymbol->source->range->begin->line > 0
							? moduleSymbol->source->range->begin->line - 1 : 0;
						resolvedInfo.definitionColumn = moduleSymbol->source->range->begin->column > 0
							? moduleSymbol->source->range->begin->column - 1 : 0;
						resolvedInfo.definitionEndLine = moduleSymbol->source->range->end->line > 0
							? moduleSymbol->source->range->end->line - 1 : 0;
						resolvedInfo.definitionEndColumn = moduleSymbol->source->range->end->column > 0
							? moduleSymbol->source->range->end->column - 1 : 0;
					}
				}
				return resolvedInfo;
			}

			auto moduleTypeIterator = moduleEntry.second.analyzedTypes.find( identifierText );
			if( moduleTypeIterator != moduleEntry.second.analyzedTypes.end() ) {
				semantic::TypeSharedPointer moduleType = moduleTypeIterator->second;
				resolvedInfo.symbolKind = semantic::Symbol::Kind::Type;
				resolvedInfo.resolvedType = moduleType;
				resolvedInfo.typeName = moduleType->toString();
				resolvedInfo.qualifiedName = moduleType->qualified;
				resolvedInfo.filePath = moduleEntry.second.canonicalPath;
				return resolvedInfo;
			}
		}

		return resolvedInfo;
	}

	std::string extractDoccommentForDeclaration(
		const std::string& documentContent,
		uint32_t declarationStartLine
	) {
		std::vector<std::string> sourceLines;
		std::string currentLine;
		for( char character : documentContent ) {
			if( character == '\n' ) {
				sourceLines.push_back( currentLine );
				currentLine.clear();
			}
			else {
				currentLine += character;
			}
		}
		if( currentLine.empty() == false ) {
			sourceLines.push_back( currentLine );
		}

		if( declarationStartLine >= sourceLines.size() ) {
			return "";
		}

		int32_t searchLineIndex = static_cast<int32_t>( declarationStartLine ) + 1;
		if( searchLineIndex >= static_cast<int32_t>( sourceLines.size() ) ) {
			return "";
		}

		while( searchLineIndex < static_cast<int32_t>( sourceLines.size() ) ) {
			size_t nonWhitespace = sourceLines[searchLineIndex].find_first_not_of( " \t" );
			if( nonWhitespace != std::string::npos ) {
				break;
			}
			++searchLineIndex;
		}
		if( searchLineIndex >= static_cast<int32_t>( sourceLines.size() ) ) {
			return "";
		}

		std::string lineAfterDeclaration = sourceLines[searchLineIndex];
		size_t tripleQuoteStart = lineAfterDeclaration.find( "\"\"\"" );
		if( tripleQuoteStart == std::string::npos ) {
			int32_t commentScanStart = static_cast<int32_t>( declarationStartLine ) - 1;
			while( commentScanStart >= 0 ) {
				size_t nonWs = sourceLines[commentScanStart].find_first_not_of( " \t" );
				if( nonWs != std::string::npos ) {
					break;
				}
				--commentScanStart;
			}

			std::string collectedComments;
			for( int32_t commentLineIndex = commentScanStart;
				 commentLineIndex >= 0; --commentLineIndex ) {

				std::string commentLine = sourceLines[commentLineIndex];
				size_t contentStart = commentLine.find_first_not_of( " \t" );
				if( contentStart == std::string::npos ) {
					break;
				}

				if( commentLine[contentStart] == '#' && ( contentStart + 1 >= commentLine.size() || commentLine[contentStart + 1] != '{' ) ) {
					size_t textStart = commentLine.find_first_not_of( " \t", contentStart + 1 );
					std::string commentText = textStart != std::string::npos
						? commentLine.substr( textStart ) : "";
					if( collectedComments.empty() ) {
						collectedComments = commentText;
					}
					else {
						collectedComments = commentText + "\n" + collectedComments;
					}
				}
				else {
					break;
				}
			}
			return collectedComments;
		}

		size_t tripleQuoteEndOnSameLine = lineAfterDeclaration.find( "\"\"\"", tripleQuoteStart + 3 );
		if( tripleQuoteEndOnSameLine != std::string::npos ) {
			std::string docContent = lineAfterDeclaration.substr(
				tripleQuoteStart + 3,
				tripleQuoteEndOnSameLine - tripleQuoteStart - 3
			);
			size_t contentStart = docContent.find_first_not_of( " \t" );
			if( contentStart != std::string::npos ) {
				docContent = docContent.substr( contentStart );
			}
			size_t contentEnd = docContent.find_last_not_of( " \t" );
			if( contentEnd != std::string::npos ) {
				docContent = docContent.substr( 0, contentEnd + 1 );
			}
			return docContent;
		}

		size_t baseIndent = tripleQuoteStart;

		std::string collectedDocumentation;
		for( int32_t docLineIndex = searchLineIndex + 1;
			 docLineIndex < static_cast<int32_t>( sourceLines.size() ); ++docLineIndex ) {

			std::string docLine = sourceLines[docLineIndex];
			size_t closingTripleQuote = docLine.find( "\"\"\"" );
			if( closingTripleQuote != std::string::npos ) {
				size_t stripAmount = 0;
				for( size_t i = 0; i < closingTripleQuote && i < baseIndent; ++i ) {
					if( docLine[i] == ' ' || docLine[i] == '\t' ) { ++stripAmount; }
					else { break; }
				}
				std::string lastPart = docLine.substr( stripAmount, closingTripleQuote - stripAmount );
				if( lastPart.find_first_not_of( " \t" ) != std::string::npos ) {
					if( collectedDocumentation.empty() == false ) {
						collectedDocumentation += "\n";
					}
					collectedDocumentation += lastPart;
				}
				break;
			}

			size_t stripAmount = 0;
			for( size_t i = 0; i < docLine.size() && i < baseIndent; ++i ) {
				if( docLine[i] == ' ' || docLine[i] == '\t' ) { ++stripAmount; }
				else { break; }
			}
			std::string strippedLine = docLine.substr( stripAmount );

			if( strippedLine.find_first_not_of( " \t" ) != std::string::npos ) {
				if( collectedDocumentation.empty() == false ) {
					collectedDocumentation += "\n";
				}
				collectedDocumentation += strippedLine;
			}
			else if( collectedDocumentation.empty() == false ) {
				collectedDocumentation += "\n";
			}
		}

		return collectedDocumentation;
	}

	std::string extractParameterDocFromDocstring(
		const std::string& functionDocstring,
		const std::string& parameterName
	) {
		std::vector<std::string> lines;
		std::string currentLine;
		for( char character : functionDocstring ) {
			if( character == '\n' ) {
				lines.push_back( currentLine );
				currentLine.clear();
			}
			else {
				currentLine += character;
			}
		}
		if( currentLine.empty() == false ) {
			lines.push_back( currentLine );
		}

		bool inParametersSection = false;
		bool foundParam = false;
		std::string paramDoc;

		for( size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex ) {
			const std::string& line = lines[lineIndex];
			size_t firstNonWs = line.find_first_not_of( " \t" );
			if( firstNonWs == std::string::npos ) {
				if( foundParam ) {
					break;
				}
				continue;
			}

			std::string trimmed = line.substr( firstNonWs );
			size_t indent = firstNonWs;

			if( indent == 0 && trimmed.size() > 1 && trimmed.back() == ':' ) {
				std::string sectionName = trimmed.substr( 0, trimmed.size() - 1 );
				if( sectionName == "Parameters" || sectionName == "Args" || sectionName == "Arguments" ) {
					inParametersSection = true;
					continue;
				}
				else if( inParametersSection ) {
					break;
				}
				continue;
			}

			if( inParametersSection == false ) { continue; }

			if( foundParam ) {
				if( indent > 4 ) {
					if( paramDoc.empty() == false ) {
						paramDoc += " ";
					}
					paramDoc += trimmed;
				}
				else {
					break;
				}
			}
			else {
				size_t parenOpen = trimmed.find( '(' );
				size_t parenClose = trimmed.find( ')' );
				if( parenOpen != std::string::npos && parenClose != std::string::npos &&
					parenClose > parenOpen && trimmed.back() == ':' ) {
					std::string entryName = trimmed.substr( 0, parenOpen );
					size_t nameEnd = entryName.find_last_not_of( " \t" );
					if( nameEnd != std::string::npos ) {
						entryName = entryName.substr( 0, nameEnd + 1 );
					}
					if( entryName == parameterName ) {
						foundParam = true;
					}
				}
			}
		}

		return paramDoc;
	}

	static size_t measureIndent( const std::string& line ) {
		size_t indent = 0;
		for( char character : line ) {
			if( character == ' ' ) { ++indent; }
			else if( character == '\t' ) { indent += 4; }
			else { break; }
		}
		return indent;
	}

	static std::string trimWhitespace( const std::string& text ) {
		size_t start = text.find_first_not_of( " \t" );
		if( start == std::string::npos ) { return ""; }
		size_t end = text.find_last_not_of( " \t" );
		return text.substr( start, end - start + 1 );
	}

	std::string formatDoccommentAsMarkdown( const std::string& rawDoccomment ) {
		if( rawDoccomment.empty() ) { return ""; }

		std::vector<std::string> lines;
		std::string currentLine;
		for( char character : rawDoccomment ) {
			if( character == '\n' ) {
				lines.push_back( currentLine );
				currentLine.clear();
			}
			else {
				currentLine += character;
			}
		}
		if( currentLine.empty() == false ) {
			lines.push_back( currentLine );
		}

		std::string description;
		std::string formatted;

		enum class Section { Description, Parameters, Returns, Raises, Complexity, Examples, Notes, Other };
		Section currentSection = Section::Description;
		std::string otherSectionName;

		struct ParamEntry {
			std::string name;
			std::string type;
			std::string description;
		};
		std::vector<ParamEntry> parameters;

		struct TypedEntry {
			std::string type;
			std::string description;
		};
		std::vector<TypedEntry> returns;
		std::vector<TypedEntry> raises;

		std::vector<std::string> complexityLines;
		std::vector<std::string> exampleLines;
		std::vector<std::string> noteLines;
		std::vector<std::string> otherLines;

		auto flushOtherSection = [&]() {
			if( otherSectionName.empty() == false && otherLines.empty() == false ) {
				formatted += "\n\n### " + otherSectionName + "\n";
				for( const auto& otherLine : otherLines ) {
					formatted += otherLine + "\n";
				}
				otherSectionName.clear();
				otherLines.clear();
			}
		};

		for( size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex ) {
			const std::string& line = lines[lineIndex];
			size_t indent = measureIndent( line );
			std::string trimmed = trimWhitespace( line );

			if( trimmed.empty() ) {
				if( currentSection == Section::Description && description.empty() == false ) {
					description += "\n";
				}
				continue;
			}

			if( indent == 0 && trimmed.size() > 1 && trimmed.back() == ':' ) {
				std::string sectionName = trimmed.substr( 0, trimmed.size() - 1 );

				if( sectionName == "Parameters" || sectionName == "Args" || sectionName == "Arguments" ) {
					flushOtherSection();
					currentSection = Section::Parameters;
					continue;
				}
				else if( sectionName == "Returns" || sectionName == "Return" ) {
					flushOtherSection();
					currentSection = Section::Returns;
					continue;
				}
				else if( sectionName == "Raises" || sectionName == "Throws" || sectionName == "Errors" ) {
					flushOtherSection();
					currentSection = Section::Raises;
					continue;
				}
				else if( sectionName == "Complexity" ) {
					flushOtherSection();
					currentSection = Section::Complexity;
					continue;
				}
				else if( sectionName == "Examples" || sectionName == "Example" ) {
					flushOtherSection();
					currentSection = Section::Examples;
					continue;
				}
				else if( sectionName == "Notes" || sectionName == "Note" ) {
					flushOtherSection();
					currentSection = Section::Notes;
					continue;
				}
				else if( currentSection != Section::Description ) {
					flushOtherSection();
					currentSection = Section::Other;
					otherSectionName = sectionName;
					otherLines.clear();
					continue;
				}
			}

			switch( currentSection ) {
				case Section::Description: {
					if( description.empty() == false ) {
						if( description.back() == '\n' ) {
							description += "\n" + trimmed;
						}
						else {
							description += "\n" + trimmed;
						}
					}
					else {
						description = trimmed;
					}
					break;
				}
				case Section::Parameters: {
					size_t parenOpen = trimmed.find( '(' );
					size_t parenClose = trimmed.find( ')' );
					bool endsWithColon = trimmed.size() > 1 && trimmed.back() == ':';

					if( indent <= 4 && parenOpen != std::string::npos &&
						parenClose != std::string::npos && parenClose > parenOpen && endsWithColon ) {
						ParamEntry entry;
						entry.name = trimWhitespace( trimmed.substr( 0, parenOpen ) );
						entry.type = trimWhitespace( trimmed.substr( parenOpen + 1, parenClose - parenOpen - 1 ) );
						parameters.push_back( entry );
					}
					else if( parameters.empty() == false ) {
						if( parameters.back().description.empty() == false ) {
							parameters.back().description += " " + trimmed;
						}
						else {
							parameters.back().description = trimmed;
						}
					}
					break;
				}
				case Section::Returns: {
					size_t colonPos = trimmed.find( ':' );
					bool isTypeEntry = indent <= 4 && colonPos != std::string::npos &&
						colonPos == trimmed.size() - 1;

					if( isTypeEntry && returns.empty() ) {
						TypedEntry entry;
						entry.type = trimWhitespace( trimmed.substr( 0, colonPos ) );
						returns.push_back( entry );
					}
					else if( returns.empty() == false ) {
						if( returns.back().description.empty() == false ) {
							returns.back().description += " " + trimmed;
						}
						else {
							returns.back().description = trimmed;
						}
					}
					else {
						TypedEntry entry;
						entry.description = trimmed;
						returns.push_back( entry );
					}
					break;
				}
				case Section::Raises: {
					size_t colonPos = trimmed.find( ':' );
					bool isTypeEntry = indent <= 4 && colonPos != std::string::npos &&
						colonPos == trimmed.size() - 1;

					if( isTypeEntry ) {
						TypedEntry entry;
						entry.type = trimWhitespace( trimmed.substr( 0, colonPos ) );
						raises.push_back( entry );
					}
					else if( raises.empty() == false ) {
						if( raises.back().description.empty() == false ) {
							raises.back().description += " " + trimmed;
						}
						else {
							raises.back().description = trimmed;
						}
					}
					break;
				}
				case Section::Complexity: {
					complexityLines.push_back( trimmed );
					break;
				}
				case Section::Examples: {
					exampleLines.push_back( trimmed );
					break;
				}
				case Section::Notes: {
					noteLines.push_back( trimmed );
					break;
				}
				case Section::Other: {
					otherLines.push_back( trimmed );
					break;
				}
			}
		}

		formatted = description;

		if( parameters.empty() == false ) {
			formatted += "\n\n---\n\n### Parameters\n";
			for( const auto& param : parameters ) {
				formatted += "- **" + param.name + "** `" + param.type + "`";
				if( param.description.empty() == false ) {
					formatted += "  \n  " + param.description;
				}
				formatted += "\n";
			}
		}

		if( returns.empty() == false ) {
			formatted += "\n### Returns\n";
			for( const auto& returnEntry : returns ) {
				if( returnEntry.type.empty() == false ) {
					formatted += "`" + returnEntry.type + "`";
					if( returnEntry.description.empty() == false ) {
						formatted += " — " + returnEntry.description;
					}
				}
				else if( returnEntry.description.empty() == false ) {
					formatted += returnEntry.description;
				}
				formatted += "\n";
			}
		}

		if( raises.empty() == false ) {
			formatted += "\n### Raises\n";
			for( const auto& raiseEntry : raises ) {
				formatted += "- `" + raiseEntry.type + "`";
				if( raiseEntry.description.empty() == false ) {
					formatted += " — " + raiseEntry.description;
				}
				formatted += "\n";
			}
		}

		if( complexityLines.empty() == false ) {
			formatted += "\n### Complexity\n";
			for( const auto& complexityLine : complexityLines ) {
				formatted += "- " + complexityLine + "\n";
			}
		}

		if( exampleLines.empty() == false ) {
			formatted += "\n### Examples\n```uranite\n";
			for( const auto& exampleLine : exampleLines ) {
				std::string codeLine = exampleLine;
				if( codeLine.size() >= 4 && codeLine.substr( 0, 4 ) == ">>> " ) {
					codeLine = codeLine.substr( 4 );
				}
				else if( codeLine.size() >= 3 && codeLine.substr( 0, 3 ) == ">>>" ) {
					codeLine = codeLine.substr( 3 );
				}
				formatted += codeLine + "\n";
			}
			formatted += "```\n";
		}

		if( noteLines.empty() == false ) {
			formatted += "\n### Notes\n";
			for( const auto& noteLine : noteLines ) {
				formatted += noteLine + "\n";
			}
		}

		flushOtherSection();

		return formatted;
	}

	std::string canonicalizeTypeForDisplay(
		const semantic::TypeSharedPointer& displayedType
	) {
		if( displayedType == nullptr ) {
			return "?";
		}

		if( displayedType->qualified.empty() == false && semantic::qualname::isOopWrapper( displayedType->qualified ) ) {
			return displayedType->name;
		}

		if( displayedType->kind == semantic::Type::Kind::Integer ) {
			semantic::IntegerTypeSharedPointer integerType =
				std::dynamic_pointer_cast<semantic::IntegerType>( displayedType );
			if( integerType != nullptr ) {
				std::string signednessPrefix = integerType->isSigned ? "I" : "U";
				return signednessPrefix + std::to_string( integerType->bitWidth );
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Float ) {
			semantic::FloatTypeSharedPointer floatType =
				std::dynamic_pointer_cast<semantic::FloatType>( displayedType );
			if( floatType != nullptr ) {
				if( floatType->bitWidth == 64 ) return "Float";
				if( floatType->bitWidth == 32 ) return "F32";
				return "F" + std::to_string( floatType->bitWidth );
			}
		}

		if( displayedType->kind == semantic::Type::Kind::String ) return "String";
		if( displayedType->kind == semantic::Type::Kind::Bool ) return "Boolean";
		if( displayedType->kind == semantic::Type::Kind::Void ) return "Void";
		if( displayedType->kind == semantic::Type::Kind::Char ) return "Char";

		if( displayedType->kind == semantic::Type::Kind::Function ) {
			semantic::FunctionTypeSharedPointer functionType =
				std::dynamic_pointer_cast<semantic::FunctionType>( displayedType );
			if( functionType != nullptr ) {
				std::string parameterList;
				for( size_t parameterIndex = 0; parameterIndex < functionType->parameterTypes.size(); ++parameterIndex ) {
					if( parameterIndex > 0 ) parameterList += ", ";
					parameterList += canonicalizeTypeForDisplay( functionType->parameterTypes[parameterIndex] );
				}
				if( functionType->isVariadic ) {
					if( functionType->parameterTypes.empty() == false ) parameterList += ", ";
					parameterList += "...";
				}
				std::string canonicalizedReturnType = canonicalizeTypeForDisplay( functionType->returnType );
				return "function(" + parameterList + ") -> " + canonicalizedReturnType;
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Optional ) {
			semantic::OptionalTypeSharedPointer optionalType =
				std::dynamic_pointer_cast<semantic::OptionalType>( displayedType );
			if( optionalType != nullptr ) {
				return "?" + canonicalizeTypeForDisplay( optionalType->inner );
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Array ) {
			semantic::ArrayTypeSharedPointer arrayType =
				std::dynamic_pointer_cast<semantic::ArrayType>( displayedType );
			if( arrayType != nullptr ) {
				std::string canonicalizedElementType = canonicalizeTypeForDisplay( arrayType->elementType );
				if( arrayType->size >= 0 ) {
					return "[" + canonicalizedElementType + "; " + std::to_string( arrayType->size ) + "]";
				}
				return "[" + canonicalizedElementType + "]";
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Pointer ) {
			semantic::PointerTypeSharedPointer pointerType =
				std::dynamic_pointer_cast<semantic::PointerType>( displayedType );
			if( pointerType != nullptr ) {
				std::string mutabilityPrefix = pointerType->isMutable ? "*mut " : "*";
				return mutabilityPrefix + canonicalizeTypeForDisplay( pointerType->inner );
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Reference ) {
			semantic::ReferenceTypeSharedPointer referenceType =
				std::dynamic_pointer_cast<semantic::ReferenceType>( displayedType );
			if( referenceType != nullptr ) {
				std::string mutabilityPrefix = referenceType->isMutable ? "&mut " : "&";
				return mutabilityPrefix + canonicalizeTypeForDisplay( referenceType->inner );
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Class ) {
			semantic::ClassTypeSharedPointer classType =
				std::dynamic_pointer_cast<semantic::ClassType>( displayedType );
			if( classType != nullptr && classType->genericParameters.empty() == false ) {
				std::string typeDisplay = classType->name + "<";
				for( size_t genericIndex = 0; genericIndex < classType->genericParameters.size(); ++genericIndex ) {
					if( genericIndex > 0 ) typeDisplay += ", ";
					typeDisplay += canonicalizeTypeForDisplay( classType->genericParameters[genericIndex] );
				}
				typeDisplay += ">";
				return typeDisplay;
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Struct ) {
			semantic::StructTypeSharedPointer structType =
				std::dynamic_pointer_cast<semantic::StructType>( displayedType );
			if( structType != nullptr && structType->genericParameters.empty() == false ) {
				std::string typeDisplay = structType->name + "<";
				for( size_t genericIndex = 0; genericIndex < structType->genericParameters.size(); ++genericIndex ) {
					if( genericIndex > 0 ) typeDisplay += ", ";
					typeDisplay += canonicalizeTypeForDisplay( structType->genericParameters[genericIndex] );
				}
				typeDisplay += ">";
				return typeDisplay;
			}
		}

		return displayedType->toString();
	}

	std::string canonicalizeQualifiedNameForDisplay(
		const semantic::TypeSharedPointer& displayedType
	) {
		if( displayedType == nullptr ) {
			return "";
		}

		std::string qualifiedName = displayedType->qualified;

		if( semantic::qualname::isOopWrapper( qualifiedName ) ) {
			return qualifiedName;
		}

		if( displayedType->kind == semantic::Type::Kind::Integer ) {
			semantic::IntegerTypeSharedPointer integerType =
				std::dynamic_pointer_cast<semantic::IntegerType>( displayedType );
			if( integerType != nullptr ) {
				std::string signednessPrefix = integerType->isSigned ? "I" : "U";
				std::string canonicalShortName = signednessPrefix + std::to_string( integerType->bitWidth );
				return "uranite.language." + canonicalShortName + "." + canonicalShortName;
			}
		}

		if( displayedType->kind == semantic::Type::Kind::Float ) {
			semantic::FloatTypeSharedPointer floatType =
				std::dynamic_pointer_cast<semantic::FloatType>( displayedType );
			if( floatType != nullptr ) {
				if( floatType->bitWidth == 64 ) return semantic::qualname::Float;
				if( floatType->bitWidth == 32 ) return semantic::qualname::F32;
			}
		}

		if( displayedType->kind == semantic::Type::Kind::String ) return semantic::qualname::String;
		if( displayedType->kind == semantic::Type::Kind::Bool ) return semantic::qualname::Boolean;
		if( displayedType->kind == semantic::Type::Kind::Void ) return semantic::qualname::Void;
		if( displayedType->kind == semantic::Type::Kind::Char ) return semantic::qualname::Char;

		if( qualifiedName == semantic::qualname::PrimString ) return semantic::qualname::String;
		if( qualifiedName == semantic::qualname::PrimBool ) return semantic::qualname::Boolean;
		if( qualifiedName == semantic::qualname::PrimVoid ) return semantic::qualname::Void;
		if( qualifiedName == semantic::qualname::PrimChar ) return semantic::qualname::Char;

		return qualifiedName;
	}

	std::string formatTypeSignature(
		const semantic::TypeSharedPointer& symbolType,
		const std::string& symbolName,
		semantic::Symbol::Kind symbolKind
	) {
		if( symbolType == nullptr ) {
			return symbolName;
		}

		switch( symbolKind ) {
			case semantic::Symbol::Kind::Function: {
				semantic::FunctionTypeSharedPointer functionType =
					std::dynamic_pointer_cast<semantic::FunctionType>( symbolType );
				if( functionType != nullptr ) {
					std::string signatureText = "function " + symbolName + "(";
					for( size_t parameterIndex = 0; parameterIndex < functionType->parameterTypes.size(); ++parameterIndex ) {
						if( parameterIndex > 0 ) {
							signatureText += ", ";
						}
						signatureText += canonicalizeTypeForDisplay( functionType->parameterTypes[parameterIndex] );
						if( parameterIndex < functionType->parameterNames.size() ) {
							signatureText += " " + functionType->parameterNames[parameterIndex];
						}
					}
					signatureText += ")";
					if( functionType->returnType != nullptr ) {
						signatureText += " -> " + canonicalizeTypeForDisplay( functionType->returnType );
					}
					return signatureText;
				}
				return "function " + symbolName + "(...)";
			}

			case semantic::Symbol::Kind::Type: {
				semantic::ClassTypeSharedPointer classType = std::dynamic_pointer_cast<semantic::ClassType>( symbolType );
				if( classType != nullptr ) {
					std::string typeSignature = "class " + symbolName;
					if( classType->genericParameters.empty() == false ) {
						typeSignature += "<";
						for( size_t genericIndex = 0; genericIndex < classType->genericParameters.size(); ++genericIndex ) {
							if( genericIndex > 0 ) {
								typeSignature += ", ";
							}
							typeSignature += canonicalizeTypeForDisplay( classType->genericParameters[genericIndex] );
						}
						typeSignature += ">";
					}
					if( classType->baseClass != nullptr ) {
						typeSignature += " extends " + canonicalizeTypeForDisplay( classType->baseClass );
					}
					return typeSignature;
				}

				semantic::StructTypeSharedPointer structType = std::dynamic_pointer_cast<semantic::StructType>( symbolType );
				if( structType != nullptr ) {
					std::string typeSignature = "struct " + symbolName;
					if( structType->genericParameters.empty() == false ) {
						typeSignature += "<";
						for( size_t genericIndex = 0; genericIndex < structType->genericParameters.size(); ++genericIndex ) {
							if( genericIndex > 0 ) {
								typeSignature += ", ";
							}
							typeSignature += canonicalizeTypeForDisplay( structType->genericParameters[genericIndex] );
						}
						typeSignature += ">";
					}
					return typeSignature;
				}

				semantic::EnumTypeSharedPointer enumType = std::dynamic_pointer_cast<semantic::EnumType>( symbolType );
				if( enumType != nullptr ) {
					std::string typeSignature = "enum " + symbolName;
					if( enumType->backedType != nullptr ) {
						typeSignature += " backed " + canonicalizeTypeForDisplay( enumType->backedType );
					}
					return typeSignature;
				}

				semantic::InterfaceTypeSharedPointer interfaceType = std::dynamic_pointer_cast<semantic::InterfaceType>( symbolType );
				if( interfaceType != nullptr ) {
					return "interface " + symbolName;
				}

				semantic::TraitTypeSharedPointer traitType = std::dynamic_pointer_cast<semantic::TraitType>( symbolType );
				if( traitType != nullptr ) {
					return "trait " + symbolName;
				}

				return "type " + symbolName;
			}

			case semantic::Symbol::Kind::Variable:
			case semantic::Symbol::Kind::Parameter:
			case semantic::Symbol::Kind::Field:
				return canonicalizeTypeForDisplay( symbolType ) + " " + symbolName;

			case semantic::Symbol::Kind::EnumVariant:
				return "unit " + symbolName;

			case semantic::Symbol::Kind::Module:
				return "module " + symbolName;
		}

		return symbolName;
	}

	static std::string extractTypeNameFromNode( const ast::nodes::TypeNodeSharedPointer& typeNode ) {
		if( typeNode == nullptr ) {
			return "";
		}

		std::shared_ptr<ast::nodes::SimpleTypeNode> simpleType =
			std::dynamic_pointer_cast<ast::nodes::SimpleTypeNode>( typeNode );
		if( simpleType != nullptr ) {
			return simpleType->name;
		}

		std::shared_ptr<ast::nodes::GenericTypeNode> genericType =
			std::dynamic_pointer_cast<ast::nodes::GenericTypeNode>( typeNode );
		if( genericType != nullptr ) {
			return genericType->name;
		}

		std::shared_ptr<ast::nodes::OptionalTypeNode> optionalType =
			std::dynamic_pointer_cast<ast::nodes::OptionalTypeNode>( typeNode );
		if( optionalType != nullptr ) {
			return extractTypeNameFromNode( optionalType->innerType );
		}

		return "";
	}

	static void collectVariablesFromStatements(
		const std::vector<ast::nodes::StatementSharedPointer>& statements,
		const std::string& enclosingFunctionName,
		std::vector<LocalVariableInfo>& collectedVariables
	) {
		for( const ast::nodes::StatementSharedPointer& statement : statements ) {
			if( statement == nullptr ) {
				continue;
			}

			if( statement->kind == ast::Node::Kind::VariableStatement ) {
				std::shared_ptr<ast::nodes::VariableStatement> variableStatement =
					std::dynamic_pointer_cast<ast::nodes::VariableStatement>( statement );
				if( variableStatement != nullptr && variableStatement->type != nullptr ) {
					LocalVariableInfo variableInfo;
					variableInfo.variableName = variableStatement->name;
					variableInfo.declaredTypeName = extractTypeNameFromNode( variableStatement->type );
					variableInfo.enclosingFunctionName = enclosingFunctionName;

					std::shared_ptr<ast::nodes::GenericTypeNode> genericTypeNode =
						std::dynamic_pointer_cast<ast::nodes::GenericTypeNode>( variableStatement->type );
					if( genericTypeNode != nullptr ) {
						for( const ast::nodes::TypeNodeSharedPointer& typeArgNode : genericTypeNode->typeArguments ) {
							variableInfo.typeArgumentNames.push_back( extractTypeNameFromNode( typeArgNode ) );
						}
					}

					if( variableStatement->source != nullptr && variableStatement->source->location != nullptr ) {
						uint32_t rawLine = variableStatement->source->location->line;
						uint32_t rawColumn = variableStatement->source->location->column;
						variableInfo.declarationLine = rawLine > 0 ? rawLine - 1 : 0;
						variableInfo.declarationColumn = rawColumn > 0 ? rawColumn - 1 : 0;
					}
					collectedVariables.push_back( variableInfo );
				}
			}

			if( statement->kind == ast::Node::Kind::IfStatement ) {
				std::shared_ptr<ast::nodes::IfStatement> ifStatement =
					std::dynamic_pointer_cast<ast::nodes::IfStatement>( statement );
				if( ifStatement != nullptr ) {
					collectVariablesFromStatements( ifStatement->thenBody, enclosingFunctionName, collectedVariables );
					collectVariablesFromStatements( ifStatement->elseBody, enclosingFunctionName, collectedVariables );
					for( const auto& elifBranch : ifStatement->elifBranches ) {
						collectVariablesFromStatements( elifBranch.second, enclosingFunctionName, collectedVariables );
					}
				}
			}
			else if( statement->kind == ast::Node::Kind::WhileStatement ) {
				std::shared_ptr<ast::nodes::WhileStatement> whileStatement =
					std::dynamic_pointer_cast<ast::nodes::WhileStatement>( statement );
				if( whileStatement != nullptr ) {
					collectVariablesFromStatements( whileStatement->body, enclosingFunctionName, collectedVariables );
				}
			}
			else if( statement->kind == ast::Node::Kind::ForStatement ) {
				std::shared_ptr<ast::nodes::ForStatement> forStatement =
					std::dynamic_pointer_cast<ast::nodes::ForStatement>( statement );
				if( forStatement != nullptr ) {
					collectVariablesFromStatements( forStatement->body, enclosingFunctionName, collectedVariables );
				}
			}
			else if( statement->kind == ast::Node::Kind::BlockStatement ) {
				std::shared_ptr<ast::nodes::BlockStatement> blockStatement =
					std::dynamic_pointer_cast<ast::nodes::BlockStatement>( statement );
				if( blockStatement != nullptr ) {
					collectVariablesFromStatements( blockStatement->statements, enclosingFunctionName, collectedVariables );
				}
			}
			else if( statement->kind == ast::Node::Kind::TryCatchStatement ) {
				std::shared_ptr<ast::nodes::TryCatchStatement> tryCatchStatement =
					std::dynamic_pointer_cast<ast::nodes::TryCatchStatement>( statement );
				if( tryCatchStatement != nullptr ) {
					collectVariablesFromStatements( tryCatchStatement->tryBody, enclosingFunctionName, collectedVariables );
					for( const ast::nodes::ExceptionClause& exceptionClause : tryCatchStatement->exceptionClauses ) {
						collectVariablesFromStatements( exceptionClause.body, enclosingFunctionName, collectedVariables );
					}
					collectVariablesFromStatements( tryCatchStatement->finallyBody, enclosingFunctionName, collectedVariables );
				}
			}
			else if( statement->kind == ast::Node::Kind::UnsafeBlock ) {
				std::shared_ptr<ast::nodes::UnsafeBlockStatement> unsafeBlock =
					std::dynamic_pointer_cast<ast::nodes::UnsafeBlockStatement>( statement );
				if( unsafeBlock != nullptr ) {
					collectVariablesFromStatements( unsafeBlock->body, enclosingFunctionName, collectedVariables );
				}
			}
		}
	}

	static void collectVariablesFromFunctionDeclaration(
		const std::shared_ptr<ast::nodes::FunctionDeclaration>& functionDeclaration,
		const std::string& enclosingClassName,
		std::vector<LocalVariableInfo>& collectedVariables
	) {
		if( functionDeclaration == nullptr ) {
			return;
		}

		std::string qualifiedFunctionName = enclosingClassName.empty()
			? functionDeclaration->name
			: enclosingClassName + "." + functionDeclaration->name;

		for( const ast::nodes::FunctionParameterSharedPointer& parameter : functionDeclaration->parameters ) {
			if( parameter != nullptr && parameter->type != nullptr ) {
				LocalVariableInfo parameterInfo;
				parameterInfo.variableName = parameter->name;
				parameterInfo.declaredTypeName = extractTypeNameFromNode( parameter->type );
				parameterInfo.enclosingFunctionName = qualifiedFunctionName;

				std::shared_ptr<ast::nodes::GenericTypeNode> genericParamTypeNode =
					std::dynamic_pointer_cast<ast::nodes::GenericTypeNode>( parameter->type );
				if( genericParamTypeNode != nullptr ) {
					for( const ast::nodes::TypeNodeSharedPointer& typeArgNode : genericParamTypeNode->typeArguments ) {
						parameterInfo.typeArgumentNames.push_back( extractTypeNameFromNode( typeArgNode ) );
					}
				}

				if( parameter->source != nullptr && parameter->source->location != nullptr ) {
					uint32_t rawLine = parameter->source->location->line;
					uint32_t rawColumn = parameter->source->location->column;
					parameterInfo.declarationLine = rawLine > 0 ? rawLine - 1 : 0;
					parameterInfo.declarationColumn = rawColumn > 0 ? rawColumn - 1 : 0;
				}
				collectedVariables.push_back( parameterInfo );
			}
		}

		collectVariablesFromStatements( functionDeclaration->body, qualifiedFunctionName, collectedVariables );
	}

	void collectLocalVariablesFromProgram(
		const ast::nodes::ProgramSharedPointer& program,
		std::vector<LocalVariableInfo>& collectedVariables
	) {
		if( program == nullptr ) {
			return;
		}

		for( const ast::nodes::DeclarationSharedPointer& declaration : program->declarations ) {
			if( declaration == nullptr ) {
				continue;
			}

			if( declaration->kind == ast::Node::Kind::FunctionDeclaration ) {
				std::shared_ptr<ast::nodes::FunctionDeclaration> functionDeclaration =
					std::dynamic_pointer_cast<ast::nodes::FunctionDeclaration>( declaration );
				collectVariablesFromFunctionDeclaration( functionDeclaration, "", collectedVariables );
			}
			else if( declaration->kind == ast::Node::Kind::ClassDeclaration ) {
				std::shared_ptr<ast::nodes::ClassDeclaration> classDeclaration =
					std::dynamic_pointer_cast<ast::nodes::ClassDeclaration>( declaration );
				if( classDeclaration != nullptr ) {
					for( const ast::nodes::DeclarationSharedPointer& methodDeclaration : classDeclaration->methods ) {
						if( methodDeclaration != nullptr && methodDeclaration->kind == ast::Node::Kind::FunctionDeclaration ) {
							std::shared_ptr<ast::nodes::FunctionDeclaration> methodFunction =
								std::dynamic_pointer_cast<ast::nodes::FunctionDeclaration>( methodDeclaration );
							collectVariablesFromFunctionDeclaration( methodFunction, classDeclaration->name, collectedVariables );
						}
					}
				}
			}
		}
	}

	semantic::TypeSharedPointer resolveLocalVariableType(
		const SemanticAnalysisResult& analysisResult,
		const std::string& variableName,
		uint32_t cursorLine
	) {
		const LocalVariableInfo* bestMatch = nullptr;

		for( const LocalVariableInfo& localVariable : analysisResult.localVariables ) {
			if( localVariable.variableName == variableName && localVariable.declarationLine <= cursorLine ) {
				if( bestMatch == nullptr || localVariable.declarationLine > bestMatch->declarationLine ) {
					bestMatch = &localVariable;
				}
			}
		}

		if( bestMatch == nullptr || bestMatch->declaredTypeName.empty() ) {
			return nullptr;
		}

		if( analysisResult.typeRegistry != nullptr ) {
			semantic::TypeSharedPointer registryType = analysisResult.typeRegistry->lookupType( bestMatch->declaredTypeName );
			if( registryType != nullptr ) {
				return registryType;
			}
		}

		auto resolvedTypeIterator = analysisResult.resolvedTypes.find( bestMatch->declaredTypeName );
		if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
			return resolvedTypeIterator->second;
		}

		for( const auto& moduleEntry : analysisResult.loadedModules ) {
			auto moduleTypeIterator = moduleEntry.second.analyzedTypes.find( bestMatch->declaredTypeName );
			if( moduleTypeIterator != moduleEntry.second.analyzedTypes.end() ) {
				return moduleTypeIterator->second;
			}
		}

		return nullptr;
	}

	static void extractSourceLocationIntoResult(
		const lookup::SourceSharedPointer& sourceInfo,
		ResolvedSymbolInfo& resolvedInfo,
		const std::string& identifierName,
		const std::string& currentDocumentPath
	) {
		if( sourceInfo == nullptr ) return;

		resolvedInfo.filePath = sourceInfo->filename.empty()
			? currentDocumentPath : sourceInfo->filename;

		bool rangeHasValues = sourceInfo->range != nullptr &&
			( sourceInfo->range->begin->line > 0 || sourceInfo->range->begin->column > 0 );

		if( rangeHasValues ) {
			resolvedInfo.definitionLine = sourceInfo->range->begin->line - 1;
			resolvedInfo.definitionColumn = sourceInfo->range->begin->column > 0
				? sourceInfo->range->begin->column - 1 : 0;
			resolvedInfo.definitionEndLine = sourceInfo->range->end->line > 0
				? sourceInfo->range->end->line - 1 : 0;
			resolvedInfo.definitionEndColumn = sourceInfo->range->end->column > 0
				? sourceInfo->range->end->column - 1 : 0;
		}
		else if( sourceInfo->location != nullptr &&
			( sourceInfo->location->line > 0 || sourceInfo->location->column > 0 ) ) {
			resolvedInfo.definitionLine = sourceInfo->location->line > 0
				? sourceInfo->location->line - 1 : 0;
			resolvedInfo.definitionColumn = sourceInfo->location->column > 0
				? sourceInfo->location->column - 1 : 0;
			resolvedInfo.definitionEndLine = resolvedInfo.definitionLine;
			resolvedInfo.definitionEndColumn = resolvedInfo.definitionColumn +
				static_cast<uint32_t>( identifierName.size() );
		}
	}

	static void findMethodSourceLocation(
		const std::vector<ast::nodes::DeclarationSharedPointer>& methodDeclarations,
		const std::string& memberName,
		ResolvedSymbolInfo& resolvedInfo,
		const std::string& currentDocumentPath
	) {
		for( const ast::nodes::DeclarationSharedPointer& methodDeclaration : methodDeclarations ) {
			if( methodDeclaration == nullptr ) continue;
			std::shared_ptr<ast::nodes::FunctionDeclaration> functionDeclaration =
				std::dynamic_pointer_cast<ast::nodes::FunctionDeclaration>( methodDeclaration );
			if( functionDeclaration != nullptr && functionDeclaration->name == memberName ) {
				extractSourceLocationIntoResult( functionDeclaration->source, resolvedInfo, memberName, currentDocumentPath );
				break;
			}
		}
	}

	static void findFieldSourceLocation(
		const std::vector<ast::nodes::FieldDeclarationSharedPointer>& fieldDeclarations,
		const std::string& memberName,
		ResolvedSymbolInfo& resolvedInfo,
		const std::string& currentDocumentPath
	) {
		for( const ast::nodes::FieldDeclarationSharedPointer& fieldDeclaration : fieldDeclarations ) {
			if( fieldDeclaration != nullptr && fieldDeclaration->name == memberName ) {
				extractSourceLocationIntoResult( fieldDeclaration->source, resolvedInfo, memberName, currentDocumentPath );
				break;
			}
		}
	}

	ResolvedSymbolInfo resolveMemberDefinition(
		const SemanticAnalysisResult& analysisResult,
		const semantic::TypeSharedPointer& ownerType,
		const std::string& memberName,
		const std::string& currentDocumentPath
	) {
		ResolvedSymbolInfo resolvedInfo;
		resolvedInfo.symbolName = memberName;

		if( ownerType == nullptr ) {
			return resolvedInfo;
		}

		semantic::OptionalTypeSharedPointer optionalType = std::dynamic_pointer_cast<semantic::OptionalType>( ownerType );
		if( optionalType != nullptr && optionalType->inner != nullptr ) {
			return resolveMemberDefinition( analysisResult, optionalType->inner, memberName, currentDocumentPath );
		}

		semantic::ClassTypeSharedPointer classType = std::dynamic_pointer_cast<semantic::ClassType>( ownerType );
		if( classType != nullptr ) {
			for( const semantic::MethodInfo& methodInfo : classType->methods ) {
				if( methodInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::Function;
					resolvedInfo.resolvedType = methodInfo.type;
					resolvedInfo.typeName = methodInfo.type != nullptr ? methodInfo.type->toString() : "";
					resolvedInfo.qualifiedName = classType->qualified + "." + memberName;
					if( classType->astDeclaration != nullptr ) {
						findMethodSourceLocation( classType->astDeclaration->methods, memberName, resolvedInfo, currentDocumentPath );
					}
					return resolvedInfo;
				}
			}

			for( const semantic::FieldInfo& fieldInfo : classType->fields ) {
				if( fieldInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::Field;
					resolvedInfo.resolvedType = fieldInfo.type;
					resolvedInfo.typeName = fieldInfo.type != nullptr ? fieldInfo.type->toString() : "";
					resolvedInfo.qualifiedName = classType->qualified + "." + memberName;
					if( classType->astDeclaration != nullptr ) {
						findFieldSourceLocation( classType->astDeclaration->fields, memberName, resolvedInfo, currentDocumentPath );
					}
					return resolvedInfo;
				}
			}

			if( classType->baseClass != nullptr ) {
				ResolvedSymbolInfo baseResult = resolveMemberDefinition(
					analysisResult, classType->baseClass, memberName, currentDocumentPath
				);
				if( baseResult.resolvedType != nullptr ) {
					return baseResult;
				}
			}
		}

		semantic::StructTypeSharedPointer structType = std::dynamic_pointer_cast<semantic::StructType>( ownerType );
		if( structType != nullptr ) {
			for( const semantic::MethodInfo& methodInfo : structType->methods ) {
				if( methodInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::Function;
					resolvedInfo.resolvedType = methodInfo.type;
					resolvedInfo.typeName = methodInfo.type != nullptr ? methodInfo.type->toString() : "";
					resolvedInfo.qualifiedName = structType->qualified + "." + memberName;
					if( structType->astDeclaration != nullptr ) {
						findMethodSourceLocation( structType->astDeclaration->methods, memberName, resolvedInfo, currentDocumentPath );
					}
					return resolvedInfo;
				}
			}

			for( const semantic::FieldInfo& fieldInfo : structType->fields ) {
				if( fieldInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::Field;
					resolvedInfo.resolvedType = fieldInfo.type;
					resolvedInfo.typeName = fieldInfo.type != nullptr ? fieldInfo.type->toString() : "";
					resolvedInfo.qualifiedName = structType->qualified + "." + memberName;
					if( structType->astDeclaration != nullptr ) {
						findFieldSourceLocation( structType->astDeclaration->fields, memberName, resolvedInfo, currentDocumentPath );
					}
					return resolvedInfo;
				}
			}
		}

		semantic::EnumTypeSharedPointer enumType = std::dynamic_pointer_cast<semantic::EnumType>( ownerType );
		if( enumType != nullptr ) {
			for( const semantic::EnumVariantInfo& variantInfo : enumType->variants ) {
				if( variantInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::EnumVariant;
					resolvedInfo.typeName = enumType->toString();
					resolvedInfo.qualifiedName = enumType->qualified + "." + memberName;
					return resolvedInfo;
				}
			}
			for( const semantic::MethodInfo& methodInfo : enumType->methods ) {
				if( methodInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::Function;
					resolvedInfo.resolvedType = methodInfo.type;
					resolvedInfo.typeName = methodInfo.type != nullptr ? methodInfo.type->toString() : "";
					resolvedInfo.qualifiedName = enumType->qualified + "." + memberName;
					return resolvedInfo;
				}
			}
		}

		semantic::InterfaceTypeSharedPointer interfaceType = std::dynamic_pointer_cast<semantic::InterfaceType>( ownerType );
		if( interfaceType != nullptr ) {
			for( const semantic::MethodInfo& methodInfo : interfaceType->methods ) {
				if( methodInfo.name == memberName ) {
					resolvedInfo.symbolKind = semantic::Symbol::Kind::Function;
					resolvedInfo.resolvedType = methodInfo.type;
					resolvedInfo.typeName = methodInfo.type != nullptr ? methodInfo.type->toString() : "";
					resolvedInfo.qualifiedName = interfaceType->qualified + "." + memberName;
					return resolvedInfo;
				}
			}
		}

		return resolvedInfo;
	}

	MemberAccessInfo findMemberAccessModifier(
		const semantic::TypeSharedPointer& ownerType,
		const std::string& memberName
	) {
		MemberAccessInfo info;
		if( ownerType == nullptr ) {
			return info;
		}

		semantic::OptionalTypeSharedPointer optionalType = std::dynamic_pointer_cast<semantic::OptionalType>( ownerType );
		if( optionalType != nullptr && optionalType->inner != nullptr ) {
			return findMemberAccessModifier( optionalType->inner, memberName );
		}

		semantic::ClassTypeSharedPointer classType = std::dynamic_pointer_cast<semantic::ClassType>( ownerType );
		if( classType != nullptr ) {
			for( const semantic::FieldInfo& fieldInfo : classType->fields ) {
				if( fieldInfo.name == memberName ) {
					info.found = true;
					info.access = fieldInfo.access;
					info.ownerTypeName = classType->name;
					return info;
				}
			}
			for( const semantic::MethodInfo& methodInfo : classType->methods ) {
				if( methodInfo.name == memberName ) {
					info.found = true;
					info.access = methodInfo.access;
					info.ownerTypeName = classType->name;
					return info;
				}
			}
			if( classType->baseClass != nullptr ) {
				return findMemberAccessModifier( classType->baseClass, memberName );
			}
		}

		semantic::StructTypeSharedPointer structType = std::dynamic_pointer_cast<semantic::StructType>( ownerType );
		if( structType != nullptr ) {
			for( const semantic::FieldInfo& fieldInfo : structType->fields ) {
				if( fieldInfo.name == memberName ) {
					info.found = true;
					info.access = fieldInfo.access;
					info.ownerTypeName = structType->name;
					return info;
				}
			}
			for( const semantic::MethodInfo& methodInfo : structType->methods ) {
				if( methodInfo.name == memberName ) {
					info.found = true;
					info.access = methodInfo.access;
					info.ownerTypeName = structType->name;
					return info;
				}
			}
		}

		semantic::EnumTypeSharedPointer enumType = std::dynamic_pointer_cast<semantic::EnumType>( ownerType );
		if( enumType != nullptr ) {
			for( const semantic::MethodInfo& methodInfo : enumType->methods ) {
				if( methodInfo.name == memberName ) {
					info.found = true;
					info.access = methodInfo.access;
					info.ownerTypeName = enumType->name;
					return info;
				}
			}
		}

		semantic::InterfaceTypeSharedPointer interfaceType = std::dynamic_pointer_cast<semantic::InterfaceType>( ownerType );
		if( interfaceType != nullptr ) {
			for( const semantic::MethodInfo& methodInfo : interfaceType->methods ) {
				if( methodInfo.name == memberName ) {
					info.found = true;
					info.access = methodInfo.access;
					info.ownerTypeName = interfaceType->name;
					return info;
				}
			}
		}

		semantic::TraitTypeSharedPointer traitType = std::dynamic_pointer_cast<semantic::TraitType>( ownerType );
		if( traitType != nullptr ) {
			for( const semantic::FieldInfo& fieldInfo : traitType->fields ) {
				if( fieldInfo.name == memberName ) {
					info.found = true;
					info.access = fieldInfo.access;
					info.ownerTypeName = traitType->name;
					return info;
				}
			}
			for( const semantic::MethodInfo& methodInfo : traitType->methods ) {
				if( methodInfo.name == memberName ) {
					info.found = true;
					info.access = methodInfo.access;
					info.ownerTypeName = traitType->name;
					return info;
				}
			}
		}

		return info;
	}

	static semantic::TypeSharedPointer resolveEnclosingClassType(
		const SemanticAnalysisResult& analysisResult,
		uint32_t cursorLine
	) {
		for( const LocalVariableInfo& localVariable : analysisResult.localVariables ) {
			if( localVariable.declarationLine <= cursorLine && localVariable.enclosingFunctionName.empty() == false ) {
				size_t dotPosition = localVariable.enclosingFunctionName.find( '.' );
				if( dotPosition != std::string::npos ) {
					std::string enclosingClassName = localVariable.enclosingFunctionName.substr( 0, dotPosition );
					auto resolvedTypeIterator = analysisResult.resolvedTypes.find( enclosingClassName );
					if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
						return resolvedTypeIterator->second;
					}
				}
			}
		}

		if( analysisResult.analyzedProgram != nullptr ) {
			for( const ast::nodes::DeclarationSharedPointer& declaration : analysisResult.analyzedProgram->declarations ) {
				if( declaration != nullptr && declaration->kind == ast::Node::Kind::ClassDeclaration ) {
					std::shared_ptr<ast::nodes::ClassDeclaration> classDeclaration =
						std::dynamic_pointer_cast<ast::nodes::ClassDeclaration>( declaration );
					if( classDeclaration != nullptr ) {
						auto resolvedTypeIterator = analysisResult.resolvedTypes.find( classDeclaration->name );
						if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
							return resolvedTypeIterator->second;
						}
					}
				}
			}
		}

		return nullptr;
	}

	semantic::TypeSharedPointer resolveObjectTypeForMemberAccess(
		const SemanticAnalysisResult& analysisResult,
		const std::string& objectIdentifier,
		uint32_t cursorLine
	) {
		if( objectIdentifier == "self" ) {
			semantic::TypeSharedPointer enclosingClassType = resolveEnclosingClassType( analysisResult, cursorLine );
			if( enclosingClassType != nullptr ) {
				return enclosingClassType;
			}
		}

		if( objectIdentifier == "parent" ) {
			semantic::TypeSharedPointer enclosingClassType = resolveEnclosingClassType( analysisResult, cursorLine );
			if( enclosingClassType != nullptr ) {
				semantic::ClassTypeSharedPointer enclosingAsClassType =
					std::dynamic_pointer_cast<semantic::ClassType>( enclosingClassType );
				if( enclosingAsClassType != nullptr && enclosingAsClassType->baseClass != nullptr ) {
					return enclosingAsClassType->baseClass;
				}
			}
		}

		if( analysisResult.globalScope != nullptr ) {
			semantic::SymbolSharedPointer objectSymbol = analysisResult.globalScope->lookup( objectIdentifier );
			if( objectSymbol != nullptr && objectSymbol->typeref != nullptr ) {
				return objectSymbol->typeref;
			}
		}

		auto resolvedSymbolIterator = analysisResult.resolvedSymbols.find( objectIdentifier );
		if( resolvedSymbolIterator != analysisResult.resolvedSymbols.end() &&
			resolvedSymbolIterator->second.empty() == false &&
			resolvedSymbolIterator->second.front()->typeref != nullptr ) {
			return resolvedSymbolIterator->second.front()->typeref;
		}

		auto resolvedTypeIterator = analysisResult.resolvedTypes.find( objectIdentifier );
		if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
			return resolvedTypeIterator->second;
		}

		return resolveLocalVariableType( analysisResult, objectIdentifier, cursorLine );
	}

	SignatureHelp resolveSignatureHelp(
		const SemanticAnalysisResult& analysisResult,
		const SignatureHelpContext& helpContext,
		const std::string& currentDocumentPath
	) {
		SignatureHelp signatureResult;

		semantic::FunctionTypeSharedPointer functionType;

		if( helpContext.isMemberCall ) {
			semantic::TypeSharedPointer objectType = helpContext.resolvedObjectType;
			if( objectType == nullptr && helpContext.objectIdentifier.empty() == false ) {
				objectType = resolveObjectTypeForMemberAccess(
					analysisResult, helpContext.objectIdentifier, helpContext.cursorLine
				);
			}
			if( objectType != nullptr ) {
				ResolvedSymbolInfo memberInfo = resolveMemberDefinition(
					analysisResult, objectType, helpContext.functionName, currentDocumentPath
				);
				functionType = std::dynamic_pointer_cast<semantic::FunctionType>( memberInfo.resolvedType );
			}
		}

		if( functionType == nullptr ) {
			if( analysisResult.globalScope != nullptr ) {
				semantic::SymbolSharedPointer functionSymbol = analysisResult.globalScope->lookup( helpContext.functionName );
				if( functionSymbol != nullptr && functionSymbol->typeref != nullptr ) {
					functionType = std::dynamic_pointer_cast<semantic::FunctionType>( functionSymbol->typeref );
				}
			}
		}

		if( functionType == nullptr ) {
			auto symbolIterator = analysisResult.resolvedSymbols.find( helpContext.functionName );
			if( symbolIterator != analysisResult.resolvedSymbols.end() &&
				symbolIterator->second.empty() == false &&
				symbolIterator->second.front()->typeref != nullptr ) {
				functionType = std::dynamic_pointer_cast<semantic::FunctionType>( symbolIterator->second.front()->typeref );
			}
		}

		if( functionType == nullptr ) {
			for( const auto& moduleEntry : analysisResult.loadedModules ) {
				auto moduleSymbolIterator = moduleEntry.second.analyzedSymbols.find( helpContext.functionName );
				if( moduleSymbolIterator != moduleEntry.second.analyzedSymbols.end() &&
					moduleSymbolIterator->second.empty() == false &&
					moduleSymbolIterator->second.front()->typeref != nullptr ) {
					functionType = std::dynamic_pointer_cast<semantic::FunctionType>( moduleSymbolIterator->second.front()->typeref );
					if( functionType != nullptr ) break;
				}
			}
		}

		if( functionType == nullptr ) {
			return signatureResult;
		}

		SignatureInformation signatureInfo;

		std::string signatureLabel = helpContext.functionName + "(";
		std::vector<ParameterInformation> parameterInfoList;

		size_t startParameterIndex = 0;
		if( helpContext.isMemberCall && functionType->parameterNames.empty() == false &&
			functionType->parameterNames[0] == "self" ) {
			startParameterIndex = 1;
		}

		bool firstVisibleParameter = true;
		for( size_t parameterIndex = startParameterIndex; parameterIndex < functionType->parameterTypes.size(); ++parameterIndex ) {
			if( firstVisibleParameter == false ) {
				signatureLabel += ", ";
			}
			firstVisibleParameter = false;

			std::string parameterLabel;
			parameterLabel += canonicalizeTypeForDisplay( functionType->parameterTypes[parameterIndex] );
			if( parameterIndex < functionType->parameterNames.size() ) {
				parameterLabel += " " + functionType->parameterNames[parameterIndex];
			}

			ParameterInformation paramInfo;
			paramInfo.label = parameterLabel;
			parameterInfoList.push_back( paramInfo );

			signatureLabel += parameterLabel;
		}

		if( functionType->isVariadic ) {
			if( functionType->parameterTypes.empty() == false ) {
				signatureLabel += ", ";
			}
			signatureLabel += "...";
		}

		signatureLabel += ")";

		if( functionType->returnType != nullptr && functionType->returnType->isVoid() == false ) {
			signatureLabel += " -> " + canonicalizeTypeForDisplay( functionType->returnType );
		}

		signatureInfo.label = signatureLabel;
		signatureInfo.parameters = parameterInfoList;

		signatureResult.signatures.push_back( signatureInfo );
		signatureResult.activeSignature = 0;
		signatureResult.activeParameter = helpContext.activeParameterIndex;

		return signatureResult;
	}

	std::vector<IdentifierOccurrence> findAllOccurrences(
		const std::string& documentContent,
		const std::string& identifierName
	) {
		std::vector<IdentifierOccurrence> occurrences;

		if( identifierName.empty() ) {
			return occurrences;
		}

		std::vector<std::string> contentLines;
		std::string currentLine;
		for( char character : documentContent ) {
			if( character == '\n' ) {
				contentLines.push_back( currentLine );
				currentLine.clear();
			}
			else {
				currentLine += character;
			}
		}
		if( currentLine.empty() == false ) {
			contentLines.push_back( currentLine );
		}

		size_t identifierLength = identifierName.size();

		for( uint32_t lineIndex = 0; lineIndex < contentLines.size(); ++lineIndex ) {
			const std::string& lineText = contentLines[lineIndex];
			size_t searchPosition = 0;

			while( searchPosition < lineText.size() ) {
				size_t foundPosition = lineText.find( identifierName, searchPosition );
				if( foundPosition == std::string::npos ) {
					break;
				}

				bool validBoundaryBefore = ( foundPosition == 0 );
				if( validBoundaryBefore == false ) {
					char precedingCharacter = lineText[foundPosition - 1];
					validBoundaryBefore = (
						( precedingCharacter >= 'a' && precedingCharacter <= 'z' ) == false &&
						( precedingCharacter >= 'A' && precedingCharacter <= 'Z' ) == false &&
						( precedingCharacter >= '0' && precedingCharacter <= '9' ) == false &&
						precedingCharacter != '_'
					);
				}

				bool validBoundaryAfter = ( foundPosition + identifierLength >= lineText.size() );
				if( validBoundaryAfter == false ) {
					char followingCharacter = lineText[foundPosition + identifierLength];
					validBoundaryAfter = (
						( followingCharacter >= 'a' && followingCharacter <= 'z' ) == false &&
						( followingCharacter >= 'A' && followingCharacter <= 'Z' ) == false &&
						( followingCharacter >= '0' && followingCharacter <= '9' ) == false &&
						followingCharacter != '_'
					);
				}

				if( validBoundaryBefore && validBoundaryAfter ) {
					IdentifierOccurrence occurrence;
					occurrence.startLine = lineIndex;
					occurrence.startColumn = static_cast<uint32_t>( foundPosition );
					occurrence.endLine = lineIndex;
					occurrence.endColumn = static_cast<uint32_t>( foundPosition + identifierLength );
					occurrences.push_back( occurrence );
				}

				searchPosition = foundPosition + 1;
			}
		}

		return occurrences;
	}

	std::vector<std::string> findLocalVariableTypeArguments(
		const SemanticAnalysisResult& analysisResult,
		const std::string& variableName,
		uint32_t cursorLine
	) {
		const LocalVariableInfo* bestMatch = nullptr;

		spdlog::info( "[LSP-GENERIC] findLocalVariableTypeArguments: name='{}' cursorLine={} totalLocalVars={}",
			variableName, cursorLine, analysisResult.localVariables.size() );

		for( const LocalVariableInfo& localVariable : analysisResult.localVariables ) {
			if( localVariable.variableName == variableName ) {
				spdlog::info( "[LSP-GENERIC]   candidate: name='{}' type='{}' line={} typeArgs={} enclosing='{}'",
					localVariable.variableName, localVariable.declaredTypeName,
					localVariable.declarationLine, localVariable.typeArgumentNames.size(),
					localVariable.enclosingFunctionName );
				if( localVariable.declarationLine <= cursorLine ) {
					if( bestMatch == nullptr || localVariable.declarationLine > bestMatch->declarationLine ) {
						bestMatch = &localVariable;
					}
				}
			}
		}

		if( bestMatch != nullptr ) {
			spdlog::info( "[LSP-GENERIC]   bestMatch: type='{}' typeArgs={}",
				bestMatch->declaredTypeName, bestMatch->typeArgumentNames.size() );
			for( size_t argIndex = 0; argIndex < bestMatch->typeArgumentNames.size(); ++argIndex ) {
				spdlog::info( "[LSP-GENERIC]     typeArg[{}]='{}'", argIndex, bestMatch->typeArgumentNames[argIndex] );
			}
			return bestMatch->typeArgumentNames;
		}

		spdlog::info( "[LSP-GENERIC]   NO MATCH FOUND" );
		return {};
	}

	semantic::TypeSharedPointer substituteGenericParameters(
		const semantic::TypeSharedPointer& originalType,
		const std::map<std::string, semantic::TypeSharedPointer>& substitutionMap
	) {
		if( originalType == nullptr || substitutionMap.empty() ) {
			return originalType;
		}

		if( originalType->kind == semantic::Type::Kind::GenericParameter ) {
			auto substitutionIterator = substitutionMap.find( originalType->name );
			if( substitutionIterator != substitutionMap.end() ) {
				return substitutionIterator->second;
			}
			return originalType;
		}

		semantic::FunctionTypeSharedPointer functionType =
			std::dynamic_pointer_cast<semantic::FunctionType>( originalType );
		if( functionType != nullptr ) {
			std::vector<semantic::TypeSharedPointer> substitutedParameterTypes;
			for( const semantic::TypeSharedPointer& parameterType : functionType->parameterTypes ) {
				substitutedParameterTypes.push_back(
					substituteGenericParameters( parameterType, substitutionMap )
				);
			}
			semantic::TypeSharedPointer substitutedReturnType =
				substituteGenericParameters( functionType->returnType, substitutionMap );

			semantic::FunctionTypeSharedPointer substitutedFunction = std::make_shared<semantic::FunctionType>(
				std::move( substitutedParameterTypes ),
				substitutedReturnType,
				functionType->isVariadic
			);
			substitutedFunction->name = functionType->name;
			substitutedFunction->qualified = functionType->qualified;
			substitutedFunction->parameterNames = functionType->parameterNames;
			return substitutedFunction;
		}

		semantic::ClassTypeSharedPointer classType =
			std::dynamic_pointer_cast<semantic::ClassType>( originalType );
		if( classType != nullptr && classType->genericParameters.empty() == false ) {
			semantic::ClassTypeSharedPointer substitutedClass = std::make_shared<semantic::ClassType>(
				classType->name
			);
			substitutedClass->isAbstract = classType->isAbstract;
			substitutedClass->qualified = classType->qualified;
			substitutedClass->baseClass = classType->baseClass;
			substitutedClass->fields = classType->fields;
			substitutedClass->methods = classType->methods;
			substitutedClass->interfaces = classType->interfaces;
			substitutedClass->isFinal = classType->isFinal;
			substitutedClass->astDeclaration = classType->astDeclaration;
			for( const semantic::TypeSharedPointer& genericParam : classType->genericParameters ) {
				substitutedClass->genericParameters.push_back(
					substituteGenericParameters( genericParam, substitutionMap )
				);
			}
			return substitutedClass;
		}

		semantic::StructTypeSharedPointer structType =
			std::dynamic_pointer_cast<semantic::StructType>( originalType );
		if( structType != nullptr && structType->genericParameters.empty() == false ) {
			semantic::StructTypeSharedPointer substitutedStruct = std::make_shared<semantic::StructType>(
				structType->name
			);
			substitutedStruct->qualified = structType->qualified;
			substitutedStruct->fields = structType->fields;
			substitutedStruct->methods = structType->methods;
			substitutedStruct->astDeclaration = structType->astDeclaration;
			for( const semantic::TypeSharedPointer& genericParam : structType->genericParameters ) {
				substitutedStruct->genericParameters.push_back(
					substituteGenericParameters( genericParam, substitutionMap )
				);
			}
			return substitutedStruct;
		}

		semantic::OptionalTypeSharedPointer optionalType =
			std::dynamic_pointer_cast<semantic::OptionalType>( originalType );
		if( optionalType != nullptr && optionalType->inner != nullptr ) {
			semantic::TypeSharedPointer substitutedInner = substituteGenericParameters( optionalType->inner, substitutionMap );
			if( substitutedInner != optionalType->inner ) {
				std::shared_ptr<semantic::OptionalType> substitutedOptional = std::make_shared<semantic::OptionalType>( substitutedInner );
				return substitutedOptional;
			}
			return originalType;
		}

		return originalType;
	}

	std::map<std::string, semantic::TypeSharedPointer> buildGenericSubstitutionMap(
		const SemanticAnalysisResult& analysisResult,
		const semantic::TypeSharedPointer& ownerType,
		const std::vector<std::string>& typeArgumentNames
	) {
		std::map<std::string, semantic::TypeSharedPointer> substitutionMap;

		if( ownerType == nullptr || typeArgumentNames.empty() ) {
			return substitutionMap;
		}

		semantic::ClassTypeSharedPointer classType =
			std::dynamic_pointer_cast<semantic::ClassType>( ownerType );
		if( classType != nullptr && classType->genericParameters.empty() == false ) {
			size_t parameterCount = std::min( classType->genericParameters.size(), typeArgumentNames.size() );
			for( size_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex ) {
				std::string genericParamName = classType->genericParameters[parameterIndex]->name;
				std::string concreteTypeName = typeArgumentNames[parameterIndex];

				semantic::TypeSharedPointer concreteType = nullptr;

				if( analysisResult.typeRegistry != nullptr ) {
					concreteType = analysisResult.typeRegistry->lookupType( concreteTypeName );
				}
				if( concreteType == nullptr ) {
					auto resolvedTypeIterator = analysisResult.resolvedTypes.find( concreteTypeName );
					if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
						concreteType = resolvedTypeIterator->second;
					}
				}
				if( concreteType == nullptr ) {
					for( const auto& moduleEntry : analysisResult.loadedModules ) {
						auto moduleTypeIterator = moduleEntry.second.analyzedTypes.find( concreteTypeName );
						if( moduleTypeIterator != moduleEntry.second.analyzedTypes.end() ) {
							concreteType = moduleTypeIterator->second;
							break;
						}
					}
				}

				if( concreteType != nullptr ) {
					substitutionMap[genericParamName] = concreteType;
				}
			}
		}

		semantic::StructTypeSharedPointer structType =
			std::dynamic_pointer_cast<semantic::StructType>( ownerType );
		if( structType != nullptr && structType->genericParameters.empty() == false ) {
			size_t parameterCount = std::min( structType->genericParameters.size(), typeArgumentNames.size() );
			for( size_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex ) {
				std::string genericParamName = structType->genericParameters[parameterIndex]->name;
				std::string concreteTypeName = typeArgumentNames[parameterIndex];

				semantic::TypeSharedPointer concreteType = nullptr;

				if( analysisResult.typeRegistry != nullptr ) {
					concreteType = analysisResult.typeRegistry->lookupType( concreteTypeName );
				}
				if( concreteType == nullptr ) {
					auto resolvedTypeIterator = analysisResult.resolvedTypes.find( concreteTypeName );
					if( resolvedTypeIterator != analysisResult.resolvedTypes.end() ) {
						concreteType = resolvedTypeIterator->second;
					}
				}

				if( concreteType != nullptr ) {
					substitutionMap[genericParamName] = concreteType;
				}
			}
		}

		return substitutionMap;
	}

} // namespace uranite::lsp
