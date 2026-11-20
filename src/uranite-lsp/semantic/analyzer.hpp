
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

#ifndef _URANITE_LSP_SEMANTIC_ANALYZER_HPP_
#define _URANITE_LSP_SEMANTIC_ANALYZER_HPP_

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "uranite/ast/node.hpp"
#include "uranite/compiler/driver.hpp"
#include "uranite/diagnostic/diagnostic.hpp"
#include "uranite/semantic/analyzer.hpp"
#include "uranite/semantic/scope.hpp"
#include "uranite/semantic/symbol.hpp"
#include "uranite/semantic/typeref.hpp"
#include "uranite-lsp/protocol/protocol.hpp"

namespace uranite::lsp {

	struct SemanticDiagnostic {
		Range range;
		int32_t severity = DiagnosticSeverity::ERROR;
		std::string source;
		std::string message;
		std::string hinting;
	};

	struct ResolvedSymbolInfo {
		std::string symbolName;
		std::string qualifiedName;
		std::string typeName;
		std::string documentation;
		std::string filePath;
		uint32_t definitionLine = 0;
		uint32_t definitionColumn = 0;
		uint32_t definitionEndLine = 0;
		uint32_t definitionEndColumn = 0;
		semantic::Symbol::Kind symbolKind = semantic::Symbol::Kind::Variable;
		semantic::TypeSharedPointer resolvedType;
	};

	struct MemberCompletionEntry {
		std::string memberName;
		std::string memberDetail;
		std::string memberDocumentation;
		int32_t completionKind = CompletionItemKind::FIELD;
		std::string snippetInsertText;
	};

	struct ImportPathSuggestion {
		std::string modulePath;
		std::string displayLabel;
		std::string moduleDescription;
	};

	struct LocalVariableInfo {
		std::string variableName;
		std::string declaredTypeName;
		std::vector<std::string> typeArgumentNames;
		uint32_t declarationLine = 0;
		uint32_t declarationColumn = 0;
		std::string enclosingFunctionName;
	};

	struct SemanticAnalysisResult {
		bool analysisSucceeded = false;

		std::vector<SemanticDiagnostic> semanticDiagnostics;

		semantic::ScopeSharedPointer globalScope;
		std::shared_ptr<semantic::Registry> typeRegistry;

		ast::nodes::ProgramSharedPointer analyzedProgram;

		std::unordered_map<std::string, std::vector<semantic::SymbolSharedPointer>> resolvedSymbols;
		std::unordered_map<std::string, semantic::TypeSharedPointer> resolvedTypes;

		std::unordered_map<std::string, compiler::ModuleInfo> loadedModules;

		std::vector<semantic::CompletionItem> analyzerCompletions;

		std::vector<LocalVariableInfo> localVariables;

		std::string packageName;
	};

	SemanticAnalysisResult performSemanticAnalysis(
		const std::string& documentContent,
		const std::string& documentFilePath,
		const std::string& workspaceRootPath
	);

	std::vector<MemberCompletionEntry> collectMemberCompletions(
		const semantic::TypeSharedPointer& objectType,
		const std::string& currentDocumentContent = "",
		const SemanticAnalysisResult* analysisResult = nullptr,
		bool isInternalAccess = false
	);

	std::vector<ImportPathSuggestion> collectImportPathSuggestions(
		const std::string& partialImportPath,
		const std::string& workspaceRootPath
	);

	ResolvedSymbolInfo resolveSymbolAtPosition(
		const SemanticAnalysisResult& analysisResult,
		const std::string& identifierText,
		const std::string& currentDocumentPath,
		uint32_t cursorLine = 0
	);

	std::string extractDoccommentForDeclaration(
		const std::string& documentContent,
		uint32_t declarationStartLine
	);

	std::string formatDoccommentAsMarkdown(
		const std::string& rawDoccomment
	);

	std::string extractParameterDocFromDocstring(
		const std::string& functionDocstring,
		const std::string& parameterName
	);

	std::string formatTypeSignature(
		const semantic::TypeSharedPointer& symbolType,
		const std::string& symbolName,
		semantic::Symbol::Kind symbolKind
	);

	void collectLocalVariablesFromProgram(
		const ast::nodes::ProgramSharedPointer& program,
		std::vector<LocalVariableInfo>& collectedVariables
	);

	semantic::TypeSharedPointer resolveLocalVariableType(
		const SemanticAnalysisResult& analysisResult,
		const std::string& variableName,
		uint32_t cursorLine
	);

	ResolvedSymbolInfo resolveMemberDefinition(
		const SemanticAnalysisResult& analysisResult,
		const semantic::TypeSharedPointer& ownerType,
		const std::string& memberName,
		const std::string& currentDocumentPath
	);

	semantic::TypeSharedPointer resolveObjectTypeForMemberAccess(
		const SemanticAnalysisResult& analysisResult,
		const std::string& objectIdentifier,
		uint32_t cursorLine
	);

	std::vector<std::string> findLocalVariableTypeArguments(
		const SemanticAnalysisResult& analysisResult,
		const std::string& variableName,
		uint32_t cursorLine
	);

	semantic::TypeSharedPointer substituteGenericParameters(
		const semantic::TypeSharedPointer& originalType,
		const std::map<std::string, semantic::TypeSharedPointer>& substitutionMap
	);

	std::map<std::string, semantic::TypeSharedPointer> buildGenericSubstitutionMap(
		const SemanticAnalysisResult& analysisResult,
		const semantic::TypeSharedPointer& ownerType,
		const std::vector<std::string>& typeArgumentNames
	);

	struct SignatureHelpContext {
		std::string functionName;
		std::string objectIdentifier;
		uint32_t activeParameterIndex = 0;
		uint32_t cursorLine = 0;
		bool isMemberCall = false;
		semantic::TypeSharedPointer resolvedObjectType;
	};

	struct IdentifierOccurrence {
		uint32_t startLine = 0;
		uint32_t startColumn = 0;
		uint32_t endLine = 0;
		uint32_t endColumn = 0;
	};

	SignatureHelp resolveSignatureHelp(
		const SemanticAnalysisResult& analysisResult,
		const SignatureHelpContext& helpContext,
		const std::string& currentDocumentPath
	);

	std::vector<IdentifierOccurrence> findAllOccurrences(
		const std::string& documentContent,
		const std::string& identifierName
	);

	struct MemberAccessInfo {
		bool found = false;
		ast::AccessModifier access = ast::AccessModifier::Public;
		std::string ownerTypeName;
	};

	MemberAccessInfo findMemberAccessModifier(
		const semantic::TypeSharedPointer& ownerType,
		const std::string& memberName
	);

	std::string canonicalizeTypeForDisplay(
		const semantic::TypeSharedPointer& displayedType
	);

	std::string canonicalizeQualifiedNameForDisplay(
		const semantic::TypeSharedPointer& displayedType
	);

} // namespace uranite::lsp

#endif // end _URANITE_LSP_SEMANTIC_ANALYZER_HPP_
