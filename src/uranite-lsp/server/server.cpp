
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

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>

#include "uranite-lsp/server/server.hpp"
#include "uranite-lsp/semantic/analyzer.hpp"

namespace uranite::lsp {

	static const std::vector<std::string> SEMANTIC_TOKEN_TYPE_LEGEND = {
		"namespace", "type", "class", "enum", "interface", "struct",
		"typeParameter", "parameter", "variable", "property", "enumMember",
		"function", "method", "keyword", "modifier", "comment", "string",
		"number", "operator"
	};

	static const std::vector<std::string> URANITE_KEYWORDS = {
		"and", "as", "asm", "async", "await", "abstract", "addressof",
		"backed", "break",
		"case", "class", "const", "continue",
		"defer", "delete",
		"elif", "else", "enum", "except", "export", "extends", "extern",
		"False", "final", "finally", "for", "from", "function",
		"if", "implements", "import", "in", "instanceof", "interface", "is",
		"lambda",
		"match", "move", "mut",
		"naked", "native", "new", "None", "not",
		"or", "override",
		"package", "pass", "private", "property", "protect", "public",
		"raise", "raises", "Readonly", "ref", "return",
		"self", "static", "struct", "subclassof", "switch",
		"trait", "True", "try", "type",
		"unit", "unsafe", "use",
		"virtual", "volatile",
		"where", "while",
		"yield"
	};

	// ─── Deep Chained Expression Type Resolution ────────────────────────────────

	static semantic::TypeSharedPointer resolveExpressionNodeType(
		const SemanticAnalysisResult& semanticAnalysisResult,
		Document* ownerDocument,
		TSNode expressionNode,
		uint32_t cursorLine
	) {
		if( ts_node_is_null( expressionNode ) ) {
			return nullptr;
		}

		const char* expressionNodeType = ts_node_type( expressionNode );

		if( std::strcmp( expressionNodeType, "expression" ) == 0 ||
			std::strcmp( expressionNodeType, "primary_expression" ) == 0 ) {
			uint32_t namedChildCount = ts_node_named_child_count( expressionNode );
			if( namedChildCount > 0 ) {
				return resolveExpressionNodeType(
					semanticAnalysisResult, ownerDocument,
					ts_node_named_child( expressionNode, 0 ), cursorLine
				);
			}
			return nullptr;
		}

		if( std::strcmp( expressionNodeType, "identifier" ) == 0 ||
			std::strcmp( expressionNodeType, "self" ) == 0 ||
			std::strcmp( expressionNodeType, "self_expression" ) == 0 ||
			std::strcmp( expressionNodeType, "parent" ) == 0 ||
			std::strcmp( expressionNodeType, "parent_expression" ) == 0 ) {
			std::string identifierText = ownerDocument->nodeText( expressionNode );
			return resolveObjectTypeForMemberAccess( semanticAnalysisResult, identifierText, cursorLine );
		}

		if( std::strcmp( expressionNodeType, "member_access_expression" ) == 0 ) {
			TSNode objectSubNode = ts_node_child_by_field_name( expressionNode, "accessed_object", 15 );
			TSNode memberSubNode = ts_node_child_by_field_name( expressionNode, "accessed_member", 15 );

			if( ts_node_is_null( objectSubNode ) || ts_node_is_null( memberSubNode ) ) {
				return nullptr;
			}

			semantic::TypeSharedPointer objectResolvedType = resolveExpressionNodeType(
				semanticAnalysisResult, ownerDocument, objectSubNode, cursorLine
			);

			if( objectResolvedType == nullptr ) {
				return nullptr;
			}

			std::string memberName = ownerDocument->nodeText( memberSubNode );

			ResolvedSymbolInfo memberResolution = resolveMemberDefinition(
				semanticAnalysisResult, objectResolvedType, memberName, ""
			);

			semantic::TypeSharedPointer resolvedMemberType = memberResolution.resolvedType;

			if( resolvedMemberType != nullptr ) {
				std::string objectIdentifierText = ownerDocument->nodeText( objectSubNode );
				std::vector<std::string> objectTypeArguments = findLocalVariableTypeArguments(
					semanticAnalysisResult, objectIdentifierText, cursorLine
				);
				if( objectTypeArguments.empty() == false ) {
					std::map<std::string, semantic::TypeSharedPointer> genericSubstitutionMap =
						buildGenericSubstitutionMap( semanticAnalysisResult, objectResolvedType, objectTypeArguments );
					if( genericSubstitutionMap.empty() == false ) {
						resolvedMemberType = substituteGenericParameters( resolvedMemberType, genericSubstitutionMap );
					}
				}
			}

			return resolvedMemberType;
		}

		if( std::strcmp( expressionNodeType, "call_expression" ) == 0 ) {
			TSNode calledFunctionNode = ts_node_child_by_field_name( expressionNode, "called_function", 15 );
			if( ts_node_is_null( calledFunctionNode ) ) {
				return nullptr;
			}

			semantic::TypeSharedPointer calledFunctionType = resolveExpressionNodeType(
				semanticAnalysisResult, ownerDocument, calledFunctionNode, cursorLine
			);

			if( calledFunctionType == nullptr ) {
				return nullptr;
			}

			semantic::FunctionTypeSharedPointer resolvedFunctionType =
				std::dynamic_pointer_cast<semantic::FunctionType>( calledFunctionType );
			if( resolvedFunctionType != nullptr && resolvedFunctionType->returnType != nullptr ) {
				return resolvedFunctionType->returnType;
			}

			return nullptr;
		}

		auto lookupTypeByName = [&]( const std::string& typeName ) -> semantic::TypeSharedPointer {
			for( const auto& mod : semanticAnalysisResult.loadedModules ) {
				auto mit = mod.second.analyzedTypes.find( typeName );
				if( mit != mod.second.analyzedTypes.end() ) { return mit->second; }
			}
			auto it = semanticAnalysisResult.resolvedTypes.find( typeName );
			if( it != semanticAnalysisResult.resolvedTypes.end() ) { return it->second; }
			if( semanticAnalysisResult.typeRegistry != nullptr ) {
				semantic::TypeSharedPointer found = semanticAnalysisResult.typeRegistry->lookupType( typeName );
				if( found != nullptr ) { return found; }
			}
			return nullptr;
		};

		if( std::strcmp( expressionNodeType, "string_literal" ) == 0 ) {
			return lookupTypeByName( "String" );
		}
		if( std::strcmp( expressionNodeType, "integer_literal" ) == 0 ) {
			return lookupTypeByName( "I64" );
		}
		if( std::strcmp( expressionNodeType, "float_literal" ) == 0 ) {
			return lookupTypeByName( "F64" );
		}
		if( std::strcmp( expressionNodeType, "boolean_literal" ) == 0 ) {
			return lookupTypeByName( "Boolean" );
		}
		if( std::strcmp( expressionNodeType, "character_literal" ) == 0 ) {
			return lookupTypeByName( "Char" );
		}
		if( std::strcmp( expressionNodeType, "list_literal" ) == 0 ) {
			return lookupTypeByName( "ArrayList" );
		}
		if( std::strcmp( expressionNodeType, "dict_literal" ) == 0 ) {
			return lookupTypeByName( "HashMap" );
		}
		if( std::strcmp( expressionNodeType, "set_literal" ) == 0 ) {
			return lookupTypeByName( "HashSet" );
		}
		if( std::strcmp( expressionNodeType, "tuple_literal" ) == 0 ) {
			return lookupTypeByName( "Tuple" );
		}

		return nullptr;
	}

	// ─────────────────────────────────────────────────────────────────────────────

	Server::Server( std::FILE* inputStream, std::FILE* outputStream )
		: transport( inputStream, outputStream ) {
	}

	void Server::run() {
		while( true ) {
			JsonRpcMessage receivedMessage;
			if( this->transport.receiveMessage( receivedMessage ) == false ) {
				break;
			}

			if( receivedMessage.method == "initialize" ) {
				this->handleInitialize( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "initialized" ) {
				this->handleInitialized();
			}
			else if( receivedMessage.method == "shutdown" ) {
				this->handleShutdown( receivedMessage.id );
			}
			else if( receivedMessage.method == "exit" ) {
				break;
			}
			else if( receivedMessage.method == "textDocument/didOpen" ) {
				this->handleTextDocumentDidOpen( receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/didChange" ) {
				this->handleTextDocumentDidChange( receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/didSave" ) {
				this->handleTextDocumentDidSave( receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/didClose" ) {
				this->handleTextDocumentDidClose( receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/completion" ) {
				this->handleTextDocumentCompletion( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/hover" ) {
				this->handleTextDocumentHover( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/definition" ) {
				this->handleTextDocumentDefinition( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/documentSymbol" ) {
				this->handleTextDocumentDocumentSymbol( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/foldingRange" ) {
				this->handleTextDocumentFoldingRange( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/semanticTokens/full" ) {
				this->handleTextDocumentSemanticTokensFull( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "workspace/symbol" ) {
				this->handleWorkspaceSymbol( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/signatureHelp" ) {
				this->handleTextDocumentSignatureHelp( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/references" ) {
				this->handleTextDocumentReferences( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/rename" ) {
				this->handleTextDocumentRename( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.method == "textDocument/prepareRename" ) {
				this->handleTextDocumentPrepareRename( receivedMessage.id, receivedMessage.params );
			}
			else if( receivedMessage.isNotification == false ) {
				this->transport.sendError( receivedMessage.id, -32601, "Method not found" );
			}
		}
	}

	std::string Server::extractDocumentUri( const llvm::json::Value& requestParams ) const {
		if( const llvm::json::Object* paramsObject = requestParams.getAsObject() ) {
			if( const llvm::json::Value* textDocumentValue = paramsObject->get( "textDocument" ) ) {
				if( const llvm::json::Object* textDocumentObject = textDocumentValue->getAsObject() ) {
					if( const llvm::json::Value* uriValue = textDocumentObject->get( "uri" ) ) {
						if( std::optional<llvm::StringRef> uriString = uriValue->getAsString() ) {
							return uriString->str();
						}
					}
				}
			}
		}
		return "";
	}

	Position Server::extractCursorPosition( const llvm::json::Value& requestParams ) const {
		Position cursorPosition = {};
		if( const llvm::json::Object* paramsObject = requestParams.getAsObject() ) {
			if( const llvm::json::Value* positionValue = paramsObject->get( "position" ) ) {
				if( const llvm::json::Object* positionObject = positionValue->getAsObject() ) {
					if( const llvm::json::Value* lineValue = positionObject->get( "line" ) ) {
						if( std::optional<int64_t> lineNumber = lineValue->getAsInteger() ) {
							cursorPosition.line = static_cast<uint32_t>( *lineNumber );
						}
					}
					if( const llvm::json::Value* characterValue = positionObject->get( "character" ) ) {
						if( std::optional<int64_t> characterNumber = characterValue->getAsInteger() ) {
							cursorPosition.character = static_cast<uint32_t>( *characterNumber );
						}
					}
				}
			}
		}
		return cursorPosition;
	}

	void Server::handleInitialize( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string workspaceRootUri;
		if( const llvm::json::Object* paramsObject = requestParams.getAsObject() ) {
			if( const llvm::json::Value* rootUriValue = paramsObject->get( "rootUri" ) ) {
				if( std::optional<llvm::StringRef> rootUriString = rootUriValue->getAsString() ) {
					workspaceRootUri = rootUriString->str();
				}
			}
		}

		this->workspace = std::make_unique<Workspace>( workspaceRootUri );

		llvm::json::Array tokenTypeLegend;
		for( const std::string& tokenTypeName : SEMANTIC_TOKEN_TYPE_LEGEND ) {
			tokenTypeLegend.push_back( tokenTypeName );
		}

		llvm::json::Object semanticTokensCapability;
		llvm::json::Object semanticTokensLegend;
		semanticTokensLegend["tokenTypes"] = llvm::json::Value( std::move( tokenTypeLegend ) );
		semanticTokensLegend["tokenModifiers"] = llvm::json::Array{};
		semanticTokensCapability["legend"] = llvm::json::Value( std::move( semanticTokensLegend ) );
		semanticTokensCapability["full"] = true;

		llvm::json::Object completionCapability;
		llvm::json::Array completionTriggerCharacters;
		completionTriggerCharacters.push_back( "." );
		completionCapability["triggerCharacters"] = llvm::json::Value( std::move( completionTriggerCharacters ) );

		llvm::json::Object textDocumentSyncCapability;
		textDocumentSyncCapability["openClose"] = true;
		textDocumentSyncCapability["change"] = 2;
		textDocumentSyncCapability["save"] = true;

		llvm::json::Object serverCapabilities;
		serverCapabilities["textDocumentSync"] = llvm::json::Value( std::move( textDocumentSyncCapability ) );
		serverCapabilities["completionProvider"] = llvm::json::Value( std::move( completionCapability ) );
		serverCapabilities["hoverProvider"] = true;
		serverCapabilities["definitionProvider"] = true;
		serverCapabilities["documentSymbolProvider"] = true;
		serverCapabilities["foldingRangeProvider"] = true;
		serverCapabilities["workspaceSymbolProvider"] = true;
		serverCapabilities["semanticTokensProvider"] = llvm::json::Value( std::move( semanticTokensCapability ) );

		llvm::json::Object signatureHelpCapability;
		llvm::json::Array signatureHelpTriggerCharacters;
		signatureHelpTriggerCharacters.push_back( "(" );
		signatureHelpTriggerCharacters.push_back( "," );
		signatureHelpCapability["triggerCharacters"] = llvm::json::Value( std::move( signatureHelpTriggerCharacters ) );
		serverCapabilities["signatureHelpProvider"] = llvm::json::Value( std::move( signatureHelpCapability ) );

		serverCapabilities["referencesProvider"] = true;
		serverCapabilities["renameProvider"] = true;

		llvm::json::Object serverInfo;
		serverInfo["name"] = "uranite-lsp";
		serverInfo["version"] = "2.0.0";

		llvm::json::Object initializeResult;
		initializeResult["capabilities"] = llvm::json::Value( std::move( serverCapabilities ) );
		initializeResult["serverInfo"] = llvm::json::Value( std::move( serverInfo ) );

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( initializeResult ) ) );
	}

	void Server::handleInitialized() {
		spdlog::info( "Uranite LSP server initialized" );
	}

	void Server::handleShutdown( const llvm::json::Value& requestId ) {
		this->isShutdownRequested = true;
		this->transport.sendResponse( requestId, nullptr );
	}

	void Server::handleTextDocumentDidOpen( const llvm::json::Value& notificationParams ) {
		const llvm::json::Object* paramsObject = notificationParams.getAsObject();
		if( paramsObject == nullptr ) return;

		const llvm::json::Value* textDocumentValue = paramsObject->get( "textDocument" );
		if( textDocumentValue == nullptr ) return;

		const llvm::json::Object* textDocumentObject = textDocumentValue->getAsObject();
		if( textDocumentObject == nullptr ) return;

		std::string documentUri;
		std::string documentContent;
		int64_t documentVersion = 0;

		if( const llvm::json::Value* uriValue = textDocumentObject->get( "uri" ) ) {
			if( std::optional<llvm::StringRef> uriString = uriValue->getAsString() ) {
				documentUri = uriString->str();
			}
		}
		if( const llvm::json::Value* textValue = textDocumentObject->get( "text" ) ) {
			if( std::optional<llvm::StringRef> textString = textValue->getAsString() ) {
				documentContent = textString->str();
			}
		}
		if( const llvm::json::Value* versionValue = textDocumentObject->get( "version" ) ) {
			if( std::optional<int64_t> versionNumber = versionValue->getAsInteger() ) {
				documentVersion = *versionNumber;
			}
		}

		Document* openedDocument = this->workspace->openDocument( documentUri, documentContent, documentVersion );
		this->runSemanticAnalysisAndPublish( openedDocument );
	}

	void Server::handleTextDocumentDidChange( const llvm::json::Value& notificationParams ) {
		std::string documentUri = this->extractDocumentUri( notificationParams );
		Document* changedDocument = this->workspace->getDocument( documentUri );
		if( changedDocument == nullptr ) return;

		const llvm::json::Object* paramsObject = notificationParams.getAsObject();
		if( paramsObject == nullptr ) return;

		std::vector<ContentChangeEvent> parsedContentChanges;
		int64_t updatedVersion = 0;

		if( const llvm::json::Value* versionValue = paramsObject->get( "version" ) ) {
			if( std::optional<int64_t> versionNumber = versionValue->getAsInteger() ) {
				updatedVersion = *versionNumber;
			}
		}

		if( const llvm::json::Value* contentChangesValue = paramsObject->get( "contentChanges" ) ) {
			if( const llvm::json::Array* contentChangesArray = contentChangesValue->getAsArray() ) {
				for( const llvm::json::Value& changeValue : *contentChangesArray ) {
					const llvm::json::Object* changeObject = changeValue.getAsObject();
					if( changeObject == nullptr ) continue;

					ContentChangeEvent parsedChange;

					if( const llvm::json::Value* rangeValue = changeObject->get( "range" ) ) {
						if( const llvm::json::Object* rangeObject = rangeValue->getAsObject() ) {
							parsedChange.hasRange = true;
							if( const llvm::json::Value* startValue = rangeObject->get( "start" ) ) {
								if( const llvm::json::Object* startObject = startValue->getAsObject() ) {
									if( const llvm::json::Value* lineValue = startObject->get( "line" ) ) {
										if( std::optional<int64_t> lineNumber = lineValue->getAsInteger() ) {
											parsedChange.range.start.line = static_cast<uint32_t>( *lineNumber );
										}
									}
									if( const llvm::json::Value* characterValue = startObject->get( "character" ) ) {
										if( std::optional<int64_t> characterNumber = characterValue->getAsInteger() ) {
											parsedChange.range.start.character = static_cast<uint32_t>( *characterNumber );
										}
									}
								}
							}
							if( const llvm::json::Value* endValue = rangeObject->get( "end" ) ) {
								if( const llvm::json::Object* endObject = endValue->getAsObject() ) {
									if( const llvm::json::Value* lineValue = endObject->get( "line" ) ) {
										if( std::optional<int64_t> lineNumber = lineValue->getAsInteger() ) {
											parsedChange.range.end.line = static_cast<uint32_t>( *lineNumber );
										}
									}
									if( const llvm::json::Value* characterValue = endObject->get( "character" ) ) {
										if( std::optional<int64_t> characterNumber = characterValue->getAsInteger() ) {
											parsedChange.range.end.character = static_cast<uint32_t>( *characterNumber );
										}
									}
								}
							}
						}
					}

					if( const llvm::json::Value* textValue = changeObject->get( "text" ) ) {
						if( std::optional<llvm::StringRef> textString = textValue->getAsString() ) {
							parsedChange.text = textString->str();
						}
					}

					parsedContentChanges.push_back( parsedChange );
				}
			}
		}

		changedDocument->applyContentChanges( parsedContentChanges, updatedVersion );
		this->publishDiagnostics( changedDocument );
	}

	void Server::handleTextDocumentDidSave( const llvm::json::Value& notificationParams ) {
		std::string documentUri = this->extractDocumentUri( notificationParams );
		Document* savedDocument = this->workspace->getDocument( documentUri );
		if( savedDocument == nullptr ) return;

		this->runSemanticAnalysisAndPublish( savedDocument );
	}

	void Server::handleTextDocumentDidClose( const llvm::json::Value& notificationParams ) {
		std::string documentUri = this->extractDocumentUri( notificationParams );
		this->workspace->closeDocument( documentUri );

		llvm::json::Object clearDiagnosticsParams;
		clearDiagnosticsParams["uri"] = documentUri;
		clearDiagnosticsParams["diagnostics"] = llvm::json::Array{};
		this->transport.sendNotification( "textDocument/publishDiagnostics",
			llvm::json::Value( std::move( clearDiagnosticsParams ) ) );
	}

	// ─── Pillar 3: Context-Aware Autocomplete ────────────────────────────────────

	void Server::handleTextDocumentCompletion( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		llvm::json::Array completionItems;

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, llvm::json::Value( std::move( completionItems ) ) );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );
		TSNode nodeAtCursor = targetDocument->findNodeAtPosition( cursorPosition );
		TSNode parentNode = ts_node_parent( nodeAtCursor );
		const char* parentNodeType = ts_node_is_null( parentNode ) == false ? ts_node_type( parentNode ) : "";
		const char* currentNodeType = ts_node_is_null( nodeAtCursor ) == false ? ts_node_type( nodeAtCursor ) : "";

		bool isDotCompletionContext = (
			std::strcmp( parentNodeType, "member_access_expression" ) == 0 ||
			std::strcmp( parentNodeType, "method_call_expression" ) == 0 ||
			std::strcmp( currentNodeType, "member_access_expression" ) == 0 ||
			std::strcmp( currentNodeType, "method_call_expression" ) == 0
		);

		TSNode memberAccessNode = ts_node_is_null( nodeAtCursor ) == false &&
			( std::strcmp( currentNodeType, "member_access_expression" ) == 0 ||
			  std::strcmp( currentNodeType, "method_call_expression" ) == 0 )
			? nodeAtCursor : parentNode;

		std::string dotObjectIdentifier;

		if( isDotCompletionContext ) {
			TSNode objectExpressionNode = ts_node_child_by_field_name( memberAccessNode, "accessed_object", 15 );
			if( ts_node_is_null( objectExpressionNode ) == false ) {
				dotObjectIdentifier = targetDocument->nodeText( objectExpressionNode );
			}
		}

		if( dotObjectIdentifier.empty() ) {
			const std::vector<std::string>& documentLines = targetDocument->sourceLines();
			if( cursorPosition.line < documentLines.size() ) {
				const std::string& currentLineText = documentLines[cursorPosition.line];
				uint32_t searchColumn = cursorPosition.character;
				while( searchColumn > 0 && currentLineText[searchColumn - 1] != '.' ) {
					if( currentLineText[searchColumn - 1] == ' ' || currentLineText[searchColumn - 1] == '\t' ) {
						break;
					}
					searchColumn--;
				}
				if( searchColumn > 0 && currentLineText[searchColumn - 1] == '.' ) {
					isDotCompletionContext = true;
					uint32_t identifierEnd = searchColumn - 1;
					uint32_t identifierStart = identifierEnd;
					while( identifierStart > 0 ) {
						char precedingChar = currentLineText[identifierStart - 1];
						if( ( precedingChar >= 'a' && precedingChar <= 'z' ) ||
							( precedingChar >= 'A' && precedingChar <= 'Z' ) ||
							( precedingChar >= '0' && precedingChar <= '9' ) ||
							precedingChar == '_' ) {
							identifierStart--;
						}
						else {
							break;
						}
					}
					if( identifierStart < identifierEnd ) {
						dotObjectIdentifier = currentLineText.substr( identifierStart, identifierEnd - identifierStart );
					}
				}
			}
		}

		if( isDotCompletionContext ) {
			const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();

			semantic::TypeSharedPointer objectTypeForCompletion;

			{
				TSNode objectExpressionTreeNode = ts_node_child_by_field_name( memberAccessNode, "accessed_object", 15 );
				if( ts_node_is_null( objectExpressionTreeNode ) == false ) {
					objectTypeForCompletion = resolveExpressionNodeType(
						semanticResult, targetDocument, objectExpressionTreeNode, cursorPosition.line
					);
				}
			}

			if( objectTypeForCompletion == nullptr && dotObjectIdentifier.empty() == false ) {
				objectTypeForCompletion = resolveObjectTypeForMemberAccess(
					semanticResult, dotObjectIdentifier, cursorPosition.line
				);
			}

			if( objectTypeForCompletion == nullptr && cursorPosition.line < targetDocument->sourceLines().size() ) {
				const std::string& lineText = targetDocument->sourceLines()[cursorPosition.line];
				uint32_t dotCol = cursorPosition.character;
				while( dotCol > 0 && lineText[dotCol - 1] != '.' ) { --dotCol; }
				if( dotCol > 1 ) {
					uint32_t beforeDot = dotCol - 2;
					auto lookupLiteralType = [&]( const std::string& typeName ) -> semantic::TypeSharedPointer {
						for( const auto& mod : semanticResult.loadedModules ) {
							auto mit = mod.second.analyzedTypes.find( typeName );
							if( mit != mod.second.analyzedTypes.end() ) { return mit->second; }
						}
						auto it = semanticResult.resolvedTypes.find( typeName );
						if( it != semanticResult.resolvedTypes.end() ) { return it->second; }
						if( semanticResult.typeRegistry != nullptr ) {
							semantic::TypeSharedPointer found = semanticResult.typeRegistry->lookupType( typeName );
							if( found != nullptr ) { return found; }
						}
						return nullptr;
					};

					if( beforeDot < lineText.size() && lineText[beforeDot] == '"' ) {
						objectTypeForCompletion = lookupLiteralType( "String" );
					}
					else if( beforeDot < lineText.size() && lineText[beforeDot] == '\'' ) {
						objectTypeForCompletion = lookupLiteralType( "Char" );
					}
					else if( dotObjectIdentifier == "true" || dotObjectIdentifier == "false" ) {
						objectTypeForCompletion = lookupLiteralType( "Boolean" );
					}
					else if( dotObjectIdentifier.empty() == false &&
						dotObjectIdentifier.find_first_not_of( "0123456789" ) == std::string::npos ) {
						objectTypeForCompletion = lookupLiteralType( "I64" );
					}
					else if( dotObjectIdentifier.empty() == false &&
						dotObjectIdentifier.find_first_not_of( "0123456789." ) == std::string::npos &&
						dotObjectIdentifier.find( '.' ) != std::string::npos ) {
						objectTypeForCompletion = lookupLiteralType( "F64" );
					}
					else if( beforeDot < lineText.size() && lineText[beforeDot] == ']' ) {
						objectTypeForCompletion = lookupLiteralType( "ArrayList" );
					}
					else if( beforeDot < lineText.size() && lineText[beforeDot] == '}' ) {
						bool hasColon = false;
						int braceDepth = 0;
						for( int scanPos = static_cast<int>( beforeDot ); scanPos >= 0; --scanPos ) {
							char scanChar = lineText[scanPos];
							if( scanChar == '}' ) { ++braceDepth; }
							else if( scanChar == '{' ) {
								--braceDepth;
								if( braceDepth == 0 ) { break; }
							}
							else if( scanChar == ':' && braceDepth == 1 ) { hasColon = true; break; }
						}
						objectTypeForCompletion = lookupLiteralType( hasColon ? "HashMap" : "HashSet" );
					}
					else if( beforeDot < lineText.size() && lineText[beforeDot] == ')' ) {
						objectTypeForCompletion = lookupLiteralType( "Tuple" );
					}
				}
			}

			if( objectTypeForCompletion != nullptr ) {
				if( std::dynamic_pointer_cast<semantic::ClassType>( objectTypeForCompletion ) == nullptr &&
					std::dynamic_pointer_cast<semantic::StructType>( objectTypeForCompletion ) == nullptr &&
					std::dynamic_pointer_cast<semantic::EnumType>( objectTypeForCompletion ) == nullptr &&
					std::dynamic_pointer_cast<semantic::InterfaceType>( objectTypeForCompletion ) == nullptr &&
					std::dynamic_pointer_cast<semantic::TraitType>( objectTypeForCompletion ) == nullptr ) {
					std::string primitiveName = objectTypeForCompletion->name;
					static const std::unordered_map<std::string, std::string> primitiveToClassName = {
						{ "str", "String" }, { "bool", "Boolean" }, { "char", "Char" },
						{ "i8", "I8" }, { "i16", "I16" }, { "i32", "I32" }, { "i64", "I64" },
						{ "u8", "U8" }, { "u16", "U16" }, { "u32", "U32" }, { "u64", "U64" },
						{ "f32", "F32" }, { "f64", "F64" },
						{ "String", "String" }, { "Boolean", "Boolean" }, { "Char", "Char" },
						{ "I64", "I64" }, { "F64", "F64" }, { "NoneType", "NoneType" }
					};
					auto classNameIt = primitiveToClassName.find( primitiveName );
					if( classNameIt != primitiveToClassName.end() ) {
						for( const auto& mod : semanticResult.loadedModules ) {
							auto mit = mod.second.analyzedTypes.find( classNameIt->second );
							if( mit != mod.second.analyzedTypes.end() ) {
								objectTypeForCompletion = mit->second;
								break;
							}
						}
						if( std::dynamic_pointer_cast<semantic::ClassType>( objectTypeForCompletion ) == nullptr ) {
							auto rit = semanticResult.resolvedTypes.find( classNameIt->second );
							if( rit != semanticResult.resolvedTypes.end() ) {
								objectTypeForCompletion = rit->second;
							}
						}
					}
				}

				std::map<std::string, semantic::TypeSharedPointer> completionGenericSubstitutionMap;
				if( dotObjectIdentifier.empty() == false ) {
					std::vector<std::string> completionTypeArguments = findLocalVariableTypeArguments(
						semanticResult, dotObjectIdentifier, cursorPosition.line
					);
					if( completionTypeArguments.empty() == false ) {
						completionGenericSubstitutionMap = buildGenericSubstitutionMap(
							semanticResult, objectTypeForCompletion, completionTypeArguments
						);
					}
				}

				bool isInternalAccess = ( dotObjectIdentifier == "self" || dotObjectIdentifier == "parent" );
				std::vector<MemberCompletionEntry> memberEntries = collectMemberCompletions(
					objectTypeForCompletion, targetDocument->content(), &semanticResult, isInternalAccess
				);
				for( const MemberCompletionEntry& memberEntry : memberEntries ) {
					llvm::json::Object completionItem;
					completionItem["label"] = memberEntry.memberName;
					completionItem["kind"] = static_cast<int64_t>( memberEntry.completionKind );

					std::string substitutedDetail = memberEntry.memberDetail;
					if( completionGenericSubstitutionMap.empty() == false ) {
						for( const auto& substitutionEntry : completionGenericSubstitutionMap ) {
							size_t searchPosition = 0;
							while( ( searchPosition = substitutedDetail.find( substitutionEntry.first, searchPosition ) ) != std::string::npos ) {
								bool validBoundaryBefore = ( searchPosition == 0 ||
									std::isalnum( substitutedDetail[searchPosition - 1] ) == false );
								bool validBoundaryAfter = ( searchPosition + substitutionEntry.first.size() >= substitutedDetail.size() ||
									std::isalnum( substitutedDetail[searchPosition + substitutionEntry.first.size()] ) == false );
								if( validBoundaryBefore && validBoundaryAfter ) {
									substitutedDetail.replace( searchPosition, substitutionEntry.first.size(),
										substitutionEntry.second->name );
									searchPosition += substitutionEntry.second->name.size();
								}
								else {
									searchPosition += substitutionEntry.first.size();
								}
							}
						}
					}
					completionItem["detail"] = substitutedDetail;
					if( memberEntry.memberDocumentation.empty() == false ) {
						llvm::json::Object documentationContent;
						documentationContent["kind"] = "markdown";
						documentationContent["value"] = memberEntry.memberDocumentation;
						completionItem["documentation"] = llvm::json::Value( std::move( documentationContent ) );
					}
					if( memberEntry.snippetInsertText.empty() == false ) {
						completionItem["insertText"] = memberEntry.snippetInsertText;
						completionItem["insertTextFormat"] = 2;
					}
					completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
				}

				this->transport.sendResponse( requestId, llvm::json::Value( std::move( completionItems ) ) );
				return;
			}
		}

		// Import-path completion: inside import/from statements
		auto isInsideImportStatement = [&]() -> bool {
			TSNode ancestorNode = parentNode;
			for( int ancestorDepth = 0; ancestorDepth < 6 && ts_node_is_null( ancestorNode ) == false; ++ancestorDepth ) {
				const char* ancestorType = ts_node_type( ancestorNode );
				if( std::strcmp( ancestorType, "import_statement" ) == 0 ) {
					return true;
				}
				ancestorNode = ts_node_parent( ancestorNode );
			}
			return false;
		};

		bool isImportContext = (
			std::strcmp( parentNodeType, "import_statement" ) == 0 ||
			std::strcmp( parentNodeType, "dotted_identifier" ) == 0 ||
			std::strcmp( parentNodeType, "import_item" ) == 0 ||
			std::strcmp( parentNodeType, "import_item_list" ) == 0 ||
			std::strcmp( parentNodeType, "import_item_brace_list" ) == 0 ||
			std::strcmp( currentNodeType, "import_statement" ) == 0 ||
			std::strcmp( currentNodeType, "dotted_identifier" ) == 0
		);

		if( isImportContext == false ) {
			isImportContext = isInsideImportStatement();
		}

		if( isImportContext == false && cursorPosition.character > 0 ) {
			Position previousCharPosition = cursorPosition;
			previousCharPosition.character = cursorPosition.character - 1;
			TSNode previousCharNode = targetDocument->findNodeAtPosition( previousCharPosition );
			if( ts_node_is_null( previousCharNode ) == false ) {
				TSNode previousAncestor = previousCharNode;
				for( int fallbackDepth = 0; fallbackDepth < 8 && ts_node_is_null( previousAncestor ) == false; ++fallbackDepth ) {
					const char* fallbackAncestorType = ts_node_type( previousAncestor );
					if( std::strcmp( fallbackAncestorType, "import_statement" ) == 0 ) {
						isImportContext = true;
						nodeAtCursor = previousCharNode;
						parentNode = ts_node_parent( previousCharNode );
						parentNodeType = ts_node_is_null( parentNode ) == false ? ts_node_type( parentNode ) : "";
						currentNodeType = ts_node_type( previousCharNode );
						break;
					}
					previousAncestor = ts_node_parent( previousAncestor );
				}
			}
		}

		// Text-based fallback for import detection when tree-sitter produces ERROR node
		std::string importLineText;
		if( isImportContext == false && cursorPosition.line < targetDocument->sourceLines().size() ) {
			const std::string& currentLineContent = targetDocument->sourceLines()[cursorPosition.line];
			std::string trimmedLine = currentLineContent;
			size_t firstNonSpace = trimmedLine.find_first_not_of( " \t" );
			if( firstNonSpace != std::string::npos ) {
				trimmedLine = trimmedLine.substr( firstNonSpace );
			}
			if( trimmedLine.rfind( "from ", 0 ) == 0 || trimmedLine.rfind( "import ", 0 ) == 0 ) {
				isImportContext = true;
				importLineText = trimmedLine;
			}
		}

		if( isImportContext ) {
			std::string partialImportPath;
			TSNode ancestorNode = ts_node_is_null( nodeAtCursor ) == false ? nodeAtCursor : parentNode;
			for( int searchDepth = 0; searchDepth < 6 && ts_node_is_null( ancestorNode ) == false; ++searchDepth ) {
				const char* ancestorType = ts_node_type( ancestorNode );
				if( std::strcmp( ancestorType, "import_statement" ) == 0 ) {
					TSNode modulePathNode = ts_node_child_by_field_name( ancestorNode, "module_path", 11 );
					if( ts_node_is_null( modulePathNode ) == false ) {
						partialImportPath = targetDocument->nodeText( modulePathNode );
					}
					break;
				}
				if( std::strcmp( ancestorType, "dotted_identifier" ) == 0 ) {
					partialImportPath = targetDocument->nodeText( ancestorNode );
					break;
				}
				ancestorNode = ts_node_parent( ancestorNode );
			}
			if( partialImportPath.empty() ) {
				partialImportPath = targetDocument->nodeText( nodeAtCursor );
			}

			// Text-based extraction when tree-sitter produces ERROR
			if( importLineText.empty() == false ) {
				if( importLineText.rfind( "from ", 0 ) == 0 ) {
					std::string afterFrom = importLineText.substr( 5 );
					size_t importKeywordPos = afterFrom.find( " import" );
					if( importKeywordPos != std::string::npos ) {
						partialImportPath = afterFrom.substr( 0, importKeywordPos );
					}
					else {
						size_t spacePos = afterFrom.find( ' ' );
						partialImportPath = ( spacePos != std::string::npos )
							? afterFrom.substr( 0, spacePos ) : afterFrom;
					}
				}
				else if( importLineText.rfind( "import ", 0 ) == 0 ) {
					partialImportPath = importLineText.substr( 7 );
					size_t spacePos = partialImportPath.find( ' ' );
					if( spacePos != std::string::npos ) {
						partialImportPath = partialImportPath.substr( 0, spacePos );
					}
				}
			}

			std::string documentFilePath = this->workspace->uriToFilePath( documentUri );
			std::vector<ImportPathSuggestion> importSuggestions = collectImportPathSuggestions(
				partialImportPath,
				this->workspace->getRootFilePath()
			);

			if( std::strcmp( parentNodeType, "import_item" ) == 0 ||
				std::strcmp( parentNodeType, "import_item_list" ) == 0 ||
				std::strcmp( parentNodeType, "import_item_brace_list" ) == 0 ) {

				std::string importModulePath;
				TSNode importAncestorNode = parentNode;
				for( int importSearchDepth = 0; importSearchDepth < 8 && ts_node_is_null( importAncestorNode ) == false; ++importSearchDepth ) {
					if( std::strcmp( ts_node_type( importAncestorNode ), "import_statement" ) == 0 ) {
						TSNode importedModulePathNode = ts_node_child_by_field_name( importAncestorNode, "module_path", 11 );
						if( ts_node_is_null( importedModulePathNode ) == false ) {
							importModulePath = targetDocument->nodeText( importedModulePathNode );
						}
						break;
					}
					importAncestorNode = ts_node_parent( importAncestorNode );
				}

				auto convertDottedPathToFilesystemSegments = []( const std::string& dottedPath ) -> std::string {
					std::string filesystemSegments = dottedPath;
					for( char& pathCharacter : filesystemSegments ) {
						if( pathCharacter == '.' ) {
							pathCharacter = '/';
						}
					}
					return filesystemSegments;
				};

				std::string normalizedImportSegments = convertDottedPathToFilesystemSegments( importModulePath );

				const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();
				for( const auto& moduleEntry : semanticResult.loadedModules ) {
					bool modulePathMatches = false;
					if( importModulePath.empty() == false ) {
						std::string modulePackagePath = convertDottedPathToFilesystemSegments( moduleEntry.second.packageName );
						if( modulePackagePath == normalizedImportSegments ||
							moduleEntry.first.find( normalizedImportSegments ) != std::string::npos ) {
							modulePathMatches = true;
						}
					}
					else {
						modulePathMatches = true;
					}
					if( modulePathMatches == false ) continue;
					if( moduleEntry.second.program == nullptr ) continue;

					std::string moduleBasename = std::filesystem::path( moduleEntry.first ).filename().string();

					for( const ast::nodes::DeclarationSharedPointer& moduleDeclaration : moduleEntry.second.program->declarations ) {
						if( moduleDeclaration == nullptr ) continue;

						if( moduleDeclaration->source != nullptr &&
						moduleDeclaration->source->filename.empty() == false ) {
						std::string declarationSourceFilename = moduleDeclaration->source->filename;
						bool declarationBelongsToModule = (
							declarationSourceFilename == moduleBasename ||
							declarationSourceFilename == moduleEntry.first ||
							declarationSourceFilename.find( moduleBasename ) != std::string::npos
						);
						if( declarationBelongsToModule == false ) continue;
					}

						std::string entityName;
						int32_t entityKind = CompletionItemKind::VARIABLE;
						if( moduleDeclaration->kind == ast::Node::Kind::FunctionDeclaration ) {
							entityName = static_cast<ast::nodes::FunctionDeclaration&>( *moduleDeclaration ).name;
							entityKind = CompletionItemKind::FUNCTION;
						}
						else if( moduleDeclaration->kind == ast::Node::Kind::ClassDeclaration ) {
							entityName = static_cast<ast::nodes::ClassDeclaration&>( *moduleDeclaration ).name;
							entityKind = CompletionItemKind::CLASS;
						}
						else if( moduleDeclaration->kind == ast::Node::Kind::StructDeclaration ) {
							entityName = static_cast<ast::nodes::StructDeclaration&>( *moduleDeclaration ).name;
							entityKind = CompletionItemKind::STRUCT;
						}
						else if( moduleDeclaration->kind == ast::Node::Kind::EnumDeclaration ) {
							entityName = static_cast<ast::nodes::EnumDeclaration&>( *moduleDeclaration ).name;
							entityKind = CompletionItemKind::ENUM;
						}
						else if( moduleDeclaration->kind == ast::Node::Kind::InterfaceDeclaration ) {
							entityName = static_cast<ast::nodes::InterfaceDeclaration&>( *moduleDeclaration ).name;
							entityKind = CompletionItemKind::INTERFACE;
						}
						if( entityName.empty() == false && moduleDeclaration->access == ast::AccessModifier::Public ) {
							llvm::json::Object completionItem;
							completionItem["label"] = entityName;
							completionItem["kind"] = static_cast<int64_t>( entityKind );
							completionItem["detail"] = moduleEntry.second.packageName;
							completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
						}
					}
				}

				this->transport.sendResponse( requestId, llvm::json::Value( std::move( completionItems ) ) );
				return;
			}

			for( const ImportPathSuggestion& importSuggestion : importSuggestions ) {
				llvm::json::Object completionItem;
				completionItem["label"] = importSuggestion.displayLabel;
				completionItem["kind"] = static_cast<int64_t>( CompletionItemKind::MODULE );
				completionItem["detail"] = importSuggestion.modulePath;
				completionItem["insertText"] = importSuggestion.modulePath;
				completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
			}

			this->transport.sendResponse( requestId, llvm::json::Value( std::move( completionItems ) ) );
			return;
		}

		// Build visible symbol set from imports + local declarations
		const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();

		std::set<std::string> visibleSymbolNames;
		bool hasWildcardImport = false;

		std::string completionDocumentPath = this->workspace->uriToFilePath( documentUri );
		std::string completionDocumentBasename = std::filesystem::path( completionDocumentPath ).filename().string();

		if( semanticResult.analyzedProgram != nullptr ) {
			for( const ast::nodes::DeclarationSharedPointer& programDeclaration : semanticResult.analyzedProgram->declarations ) {
				if( programDeclaration == nullptr ) continue;
				if( programDeclaration->source != nullptr &&
					programDeclaration->source->filename.empty() == false &&
					programDeclaration->source->filename != completionDocumentBasename &&
					programDeclaration->source->filename != completionDocumentPath ) {
					continue;
				}
				std::string declaredName;
				if( programDeclaration->kind == ast::Node::Kind::FunctionDeclaration )
					declaredName = static_cast<ast::nodes::FunctionDeclaration&>( *programDeclaration ).name;
				else if( programDeclaration->kind == ast::Node::Kind::ClassDeclaration )
					declaredName = static_cast<ast::nodes::ClassDeclaration&>( *programDeclaration ).name;
				else if( programDeclaration->kind == ast::Node::Kind::StructDeclaration )
					declaredName = static_cast<ast::nodes::StructDeclaration&>( *programDeclaration ).name;
				else if( programDeclaration->kind == ast::Node::Kind::EnumDeclaration )
					declaredName = static_cast<ast::nodes::EnumDeclaration&>( *programDeclaration ).name;
				else if( programDeclaration->kind == ast::Node::Kind::InterfaceDeclaration )
					declaredName = static_cast<ast::nodes::InterfaceDeclaration&>( *programDeclaration ).name;
				else if( programDeclaration->kind == ast::Node::Kind::ExternDeclaration )
					declaredName = static_cast<ast::nodes::ExternDeclaration&>( *programDeclaration ).name;
				else if( programDeclaration->kind == ast::Node::Kind::ConstantDeclaration )
					declaredName = static_cast<ast::nodes::ConstantDeclaration&>( *programDeclaration ).name;
				if( declaredName.empty() == false ) {
					visibleSymbolNames.insert( declaredName );
				}
			}

			for( const ast::nodes::ImportDeclarationSharedPointer& importDeclaration : semanticResult.analyzedProgram->imports ) {
				if( importDeclaration == nullptr ) continue;
				if( importDeclaration->isFromImport && importDeclaration->importAll == false ) {
					for( const ast::nodes::ImportItem& importedItem : importDeclaration->importItems ) {
						std::string resolvedName = importedItem.alias.empty() ? importedItem.name : importedItem.alias;
						if( resolvedName.empty() == false ) {
							visibleSymbolNames.insert( resolvedName );
						}
					}
				}
				else if( importDeclaration->importAll ) {
					hasWildcardImport = true;
				}
			}
		}

		static const std::set<std::string> builtinPrimitiveNames = {
			"I8", "I16", "I32", "I64", "U8", "U16", "U32", "U64",
			"Float", "Double", "Boolean", "String", "Void", "Object",
			"Memory", "None", "True", "False"
		};
		for( const std::string& builtinName : builtinPrimitiveNames ) {
			visibleSymbolNames.insert( builtinName );
		}

		// Type context: suggest builtin and user-defined types
		bool insideTypeContext = (
			std::strcmp( parentNodeType, "type_specifier" ) == 0 ||
			std::strcmp( parentNodeType, "generic_type_specifier" ) == 0
		);

		auto extractBaseTypeName = []( const std::string& typeName ) -> std::string {
			size_t angleBracketPosition = typeName.find( '<' );
			if( angleBracketPosition != std::string::npos ) {
				return typeName.substr( 0, angleBracketPosition );
			}
			return typeName;
		};

		if( insideTypeContext ) {
			std::vector<std::string> builtinTypeNames = {
				"I8", "I16", "I32", "I64",
				"U8", "U16", "U32", "U64",
				"Float", "Double", "Boolean", "String", "Void",
				"Object", "Memory", "ArrayList", "HashMap",
				"LinkedList", "Future", "Callable"
			};
			for( const std::string& typeName : builtinTypeNames ) {
				llvm::json::Object completionItem;
				completionItem["label"] = typeName;
				completionItem["kind"] = static_cast<int64_t>( CompletionItemKind::CLASS );
				completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
			}

			for( const auto& typeEntry : semanticResult.resolvedTypes ) {
				std::string baseTypeName = extractBaseTypeName( typeEntry.first );
				if( hasWildcardImport == false &&
					visibleSymbolNames.count( typeEntry.first ) == 0 &&
					visibleSymbolNames.count( baseTypeName ) == 0 ) {
					continue;
				}
				semantic::Type::Kind typeKind = typeEntry.second->kind;
				if( typeKind == semantic::Type::Kind::Class ||
					typeKind == semantic::Type::Kind::Struct ||
					typeKind == semantic::Type::Kind::Enum ||
					typeKind == semantic::Type::Kind::Interface ||
					typeKind == semantic::Type::Kind::Trait ) {
					llvm::json::Object completionItem;
					completionItem["label"] = typeEntry.first;
					completionItem["kind"] = static_cast<int64_t>( CompletionItemKind::CLASS );
					completionItem["detail"] = typeEntry.second->qualified;
					completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
				}
			}
		}

		if( semanticResult.globalScope != nullptr ) {
			const std::unordered_map<std::string, std::vector<semantic::SymbolSharedPointer>>& scopeSymbols =
				semanticResult.globalScope->symbols();
			for( const auto& scopeSymbolEntry : scopeSymbols ) {
				if( hasWildcardImport == false && visibleSymbolNames.count( scopeSymbolEntry.first ) == 0 ) {
					continue;
				}
				if( scopeSymbolEntry.second.empty() ) {
					continue;
				}

				const semantic::SymbolSharedPointer& firstSymbol = scopeSymbolEntry.second.front();
				llvm::json::Object completionItem;
				completionItem["label"] = scopeSymbolEntry.first;

				int32_t completionKind = CompletionItemKind::VARIABLE;
				switch( firstSymbol->kind ) {
					case semantic::Symbol::Kind::Function:
						completionKind = CompletionItemKind::FUNCTION;
						break;
					case semantic::Symbol::Kind::Type:
						completionKind = CompletionItemKind::CLASS;
						break;
					case semantic::Symbol::Kind::Module:
						completionKind = CompletionItemKind::MODULE;
						break;
					case semantic::Symbol::Kind::EnumVariant:
						completionKind = CompletionItemKind::ENUM_MEMBER;
						break;
					case semantic::Symbol::Kind::Field:
						completionKind = CompletionItemKind::FIELD;
						break;
					case semantic::Symbol::Kind::Parameter:
						completionKind = CompletionItemKind::VARIABLE;
						break;
					default:
						completionKind = CompletionItemKind::VARIABLE;
						break;
				}
				completionItem["kind"] = static_cast<int64_t>( completionKind );

				if( firstSymbol->typeref != nullptr ) {
					completionItem["detail"] = firstSymbol->typeref->toString();
				}

				completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
			}
		}

		// Imported/local types not in globalScope (types only live in resolvedTypes)
		if( insideTypeContext == false ) {
			std::set<std::string> alreadyAddedSymbols;
			if( semanticResult.globalScope != nullptr ) {
				for( const auto& scopeSymbolEntry : semanticResult.globalScope->symbols() ) {
					alreadyAddedSymbols.insert( scopeSymbolEntry.first );
				}
			}
			for( const auto& typeEntry : semanticResult.resolvedTypes ) {
				if( alreadyAddedSymbols.count( typeEntry.first ) > 0 ) continue;
				std::string baseTypeName = extractBaseTypeName( typeEntry.first );
				bool visibleByExact = visibleSymbolNames.count( typeEntry.first ) > 0;
				bool visibleByBase = visibleSymbolNames.count( baseTypeName ) > 0;
				if( hasWildcardImport == false && visibleByExact == false && visibleByBase == false ) continue;
				semantic::Type::Kind typeKind = typeEntry.second->kind;
				if( typeKind == semantic::Type::Kind::Class ||
					typeKind == semantic::Type::Kind::Struct ||
					typeKind == semantic::Type::Kind::Enum ||
					typeKind == semantic::Type::Kind::Interface ||
					typeKind == semantic::Type::Kind::Trait ) {
					llvm::json::Object completionItem;
					completionItem["label"] = typeEntry.first;
					completionItem["kind"] = static_cast<int64_t>( CompletionItemKind::CLASS );
					completionItem["detail"] = typeEntry.second->qualified;
					completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
				}
			}
		}

		// Keywords
		for( const std::string& keyword : URANITE_KEYWORDS ) {
			llvm::json::Object completionItem;
			completionItem["label"] = keyword;
			completionItem["kind"] = static_cast<int64_t>( CompletionItemKind::KEYWORD );
			completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
		}

		// Tree-sitter document symbols as fallback
		std::vector<DocumentSymbol> documentSymbols = targetDocument->collectDocumentSymbols();
		for( const DocumentSymbol& documentSymbol : documentSymbols ) {
			llvm::json::Object completionItem;
			completionItem["label"] = documentSymbol.name;

			int32_t completionKind = CompletionItemKind::TEXT;
			if( documentSymbol.kind == SymbolKind::FUNCTION ) completionKind = CompletionItemKind::FUNCTION;
			else if( documentSymbol.kind == SymbolKind::CLASS ) completionKind = CompletionItemKind::CLASS;
			else if( documentSymbol.kind == SymbolKind::STRUCT ) completionKind = CompletionItemKind::STRUCT;
			else if( documentSymbol.kind == SymbolKind::ENUM ) completionKind = CompletionItemKind::ENUM;
			else if( documentSymbol.kind == SymbolKind::INTERFACE ) completionKind = CompletionItemKind::INTERFACE;
			else if( documentSymbol.kind == SymbolKind::CONSTANT ) completionKind = CompletionItemKind::VARIABLE;
			else if( documentSymbol.kind == SymbolKind::VARIABLE ) completionKind = CompletionItemKind::VARIABLE;

			completionItem["kind"] = static_cast<int64_t>( completionKind );
			completionItems.push_back( llvm::json::Value( std::move( completionItem ) ) );
		}

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( completionItems ) ) );
	}

	// ─── Pillar 4: Rich Hover Documentation ──────────────────────────────────────

	void Server::handleTextDocumentHover( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );

		TSNode hoveredNode = targetDocument->findNodeAtPosition( cursorPosition );
		if( ts_node_is_null( hoveredNode ) ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		std::string hoveredText = targetDocument->nodeText( hoveredNode );
		const char* hoveredNodeType = ts_node_type( hoveredNode );

		TSNode parentNode = ts_node_parent( hoveredNode );
		const char* parentNodeType = ts_node_is_null( parentNode ) == false ? ts_node_type( parentNode ) : "";

		std::string hoverContent;

		const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();
		std::string documentFilePath = this->workspace->uriToFilePath( documentUri );

		bool isMemberAccessContext = (
			std::strcmp( parentNodeType, "member_access_expression" ) == 0 ||
			std::strcmp( parentNodeType, "method_call_expression" ) == 0
		);

		ResolvedSymbolInfo resolvedSymbol;

		spdlog::info( "[LSP-HOVER] text='{}' nodeType='{}' parentType='{}' isMemberAccess={}",
			hoveredText, hoveredNodeType, parentNodeType, isMemberAccessContext );

		if( isMemberAccessContext ) {
			TSNode objectNode = ts_node_child_by_field_name( parentNode, "accessed_object", 15 );

			spdlog::info( "[LSP-HOVER] objectNode null={} hoveredStart={} hoveredEnd={} objectStart={} objectEnd={}",
				ts_node_is_null( objectNode ),
				ts_node_start_byte( hoveredNode ), ts_node_end_byte( hoveredNode ),
				ts_node_is_null( objectNode ) ? 0 : ts_node_start_byte( objectNode ),
				ts_node_is_null( objectNode ) ? 0 : ts_node_end_byte( objectNode ) );

			bool hoveredNodeIsAccessedObject = (
				ts_node_is_null( objectNode ) == false &&
				ts_node_start_byte( hoveredNode ) == ts_node_start_byte( objectNode ) &&
				ts_node_end_byte( hoveredNode ) == ts_node_end_byte( objectNode )
			);

			bool hoveredNodeIsAccessedMember = (
				hoveredNodeIsAccessedObject == false &&
				std::strcmp( hoveredNodeType, "." ) != 0
			);

			spdlog::info( "[LSP-HOVER] isAccessedObject={} isAccessedMember={}", hoveredNodeIsAccessedObject, hoveredNodeIsAccessedMember );

			if( hoveredNodeIsAccessedMember && ts_node_is_null( objectNode ) == false ) {
				semantic::TypeSharedPointer objectType = resolveExpressionNodeType(
					semanticResult, targetDocument, objectNode, cursorPosition.line
				);
				spdlog::info( "[LSP-HOVER] objectType={}", objectType != nullptr ? objectType->toString() : "null" );
				if( objectType != nullptr ) {
					resolvedSymbol = resolveMemberDefinition(
						semanticResult, objectType, hoveredText, documentFilePath
					);

					if( resolvedSymbol.resolvedType != nullptr ) {
						std::string objectIdentifierText = targetDocument->nodeText( objectNode );
						spdlog::info( "[LSP-HOVER-GENERIC] objectIdentifier='{}' objectType='{}' memberType='{}'",
							objectIdentifierText,
							objectType != nullptr ? objectType->toString() : "null",
							resolvedSymbol.resolvedType->toString() );
						std::vector<std::string> objectTypeArguments = findLocalVariableTypeArguments(
							semanticResult, objectIdentifierText, cursorPosition.line
						);
						spdlog::info( "[LSP-HOVER-GENERIC] typeArgs found: {}", objectTypeArguments.size() );
						if( objectTypeArguments.empty() == false ) {
							std::map<std::string, semantic::TypeSharedPointer> genericSubstitutionMap =
								buildGenericSubstitutionMap( semanticResult, objectType, objectTypeArguments );
							spdlog::info( "[LSP-HOVER-GENERIC] substitutionMap size: {}", genericSubstitutionMap.size() );
							if( genericSubstitutionMap.empty() == false ) {
								resolvedSymbol.resolvedType = substituteGenericParameters(
									resolvedSymbol.resolvedType, genericSubstitutionMap
								);
								resolvedSymbol.typeName = resolvedSymbol.resolvedType->toString();
								spdlog::info( "[LSP-HOVER-GENERIC] AFTER substitution: '{}'", resolvedSymbol.typeName );
							}
						}
					}

					spdlog::info( "[LSP-HOVER] member result: resolvedType={} typeName='{}'",
						resolvedSymbol.resolvedType != nullptr ? resolvedSymbol.resolvedType->toString() : "null",
						resolvedSymbol.typeName );
				}
			}
			else if( hoveredNodeIsAccessedObject ) {
				semantic::TypeSharedPointer objectType = resolveExpressionNodeType(
					semanticResult, targetDocument, objectNode, cursorPosition.line
				);
				if( objectType != nullptr ) {
					resolvedSymbol.resolvedType = objectType;
					resolvedSymbol.typeName = objectType->toString();
					resolvedSymbol.qualifiedName = objectType->qualified;
					resolvedSymbol.symbolName = hoveredText;
					resolvedSymbol.symbolKind = semantic::Symbol::Kind::Variable;
				}
			}
		}

		if( resolvedSymbol.resolvedType == nullptr && resolvedSymbol.typeName.empty() ) {
			if( std::strcmp( hoveredNodeType, "self" ) == 0 ) {
				semantic::TypeSharedPointer enclosingClassType = resolveObjectTypeForMemberAccess(
					semanticResult, "self", cursorPosition.line
				);
				if( enclosingClassType != nullptr ) {
					resolvedSymbol.resolvedType = enclosingClassType;
					resolvedSymbol.typeName = enclosingClassType->toString();
					resolvedSymbol.qualifiedName = enclosingClassType->qualified;
					resolvedSymbol.symbolName = "self";
					resolvedSymbol.symbolKind = semantic::Symbol::Kind::Variable;
				}
			}
			else if( std::strcmp( hoveredNodeType, "parent" ) == 0 ) {
				semantic::TypeSharedPointer parentClassType = resolveObjectTypeForMemberAccess(
					semanticResult, "parent", cursorPosition.line
				);
				if( parentClassType != nullptr ) {
					resolvedSymbol.resolvedType = parentClassType;
					resolvedSymbol.typeName = parentClassType->toString();
					resolvedSymbol.qualifiedName = parentClassType->qualified;
					resolvedSymbol.symbolName = "parent";
					resolvedSymbol.symbolKind = semantic::Symbol::Kind::Type;
				}
			}
			else {
				resolvedSymbol = resolveSymbolAtPosition(
					semanticResult, hoveredText, documentFilePath, cursorPosition.line
				);

				if( resolvedSymbol.resolvedType != nullptr &&
					( resolvedSymbol.symbolKind == semantic::Symbol::Kind::Variable ||
					  resolvedSymbol.symbolKind == semantic::Symbol::Kind::Parameter ) ) {
					std::vector<std::string> variableTypeArguments = findLocalVariableTypeArguments(
						semanticResult, hoveredText, cursorPosition.line
					);
					if( variableTypeArguments.empty() == false ) {
						std::map<std::string, semantic::TypeSharedPointer> variableSubstitutionMap =
							buildGenericSubstitutionMap( semanticResult, resolvedSymbol.resolvedType, variableTypeArguments );
						if( variableSubstitutionMap.empty() == false ) {
							resolvedSymbol.resolvedType = substituteGenericParameters(
								resolvedSymbol.resolvedType, variableSubstitutionMap
							);
							resolvedSymbol.typeName = resolvedSymbol.resolvedType->toString();
						}
					}
				}

				bool needsClassMemberResolution = false;
				if( resolvedSymbol.resolvedType == nullptr && resolvedSymbol.typeName.empty() ) {
					needsClassMemberResolution = true;
				}
				else if( resolvedSymbol.resolvedType != nullptr &&
					resolvedSymbol.symbolKind == semantic::Symbol::Kind::Function &&
					std::dynamic_pointer_cast<semantic::FunctionType>( resolvedSymbol.resolvedType ) == nullptr ) {
					needsClassMemberResolution = true;
				}
				if( needsClassMemberResolution ) {
					semantic::TypeSharedPointer enclosingClassType = resolveObjectTypeForMemberAccess(
						semanticResult, "self", cursorPosition.line
					);
					if( enclosingClassType != nullptr ) {
						ResolvedSymbolInfo memberInfo = resolveMemberDefinition(
							semanticResult, enclosingClassType, hoveredText, documentFilePath
						);
						if( memberInfo.resolvedType != nullptr ) {
							resolvedSymbol = memberInfo;
						}
					}
				}
			}
		}

		bool hasSemanticInfo = ( resolvedSymbol.resolvedType != nullptr ||
			resolvedSymbol.typeName.empty() == false );

		if( hasSemanticInfo ) {
			std::string typeSignature = formatTypeSignature(
				resolvedSymbol.resolvedType,
				resolvedSymbol.symbolName,
				resolvedSymbol.symbolKind
			);

			hoverContent = "```uranite\n" + typeSignature + "\n```";

			std::string displayQualifiedName = resolvedSymbol.qualifiedName;
			if( resolvedSymbol.resolvedType != nullptr ) {
				std::string canonicalizedQualifiedName = canonicalizeQualifiedNameForDisplay( resolvedSymbol.resolvedType );
				if( canonicalizedQualifiedName.empty() == false ) {
					displayQualifiedName = canonicalizedQualifiedName;
				}
			}
			if( displayQualifiedName.empty() == false ) {
				hoverContent += "\n\n*" + displayQualifiedName + "*";
			}

			std::string doccomment;
			{
				std::string resolvedFilePath = resolvedSymbol.filePath;

				bool isBasenameOnly = resolvedFilePath.empty() == false &&
					resolvedFilePath.find( '/' ) == std::string::npos &&
					resolvedFilePath.find( '\\' ) == std::string::npos;

				if( isBasenameOnly ) {
					std::string documentBasename = std::filesystem::path( documentFilePath ).filename().string();
					if( resolvedFilePath == documentBasename ) {
						resolvedFilePath = documentFilePath;
					}
					else {
						for( const auto& moduleEntry : semanticResult.loadedModules ) {
							std::string moduleBasename = std::filesystem::path( moduleEntry.first ).filename().string();
							if( moduleBasename == resolvedFilePath ) {
								resolvedFilePath = moduleEntry.first;
								break;
							}
						}
					}
				}

				std::string sourceContent;
				if( resolvedFilePath == documentFilePath || resolvedFilePath.empty() ) {
					sourceContent = targetDocument->content();
				}
				else if( std::filesystem::is_regular_file( resolvedFilePath ) ) {
					std::ifstream sourceFileStream( resolvedFilePath );
					if( sourceFileStream.is_open() ) {
						sourceContent = std::string(
							std::istreambuf_iterator<char>( sourceFileStream ),
							std::istreambuf_iterator<char>()
						);
					}
				}

				if( sourceContent.empty() ) {
					bool isUnresolvedBasename = resolvedFilePath.empty() == false &&
						resolvedFilePath.find( '/' ) == std::string::npos &&
						resolvedFilePath.find( '\\' ) == std::string::npos;
					if( isUnresolvedBasename || resolvedFilePath.empty() ) {
						sourceContent = targetDocument->content();
					}
				}

				if( sourceContent.empty() == false ) {
					doccomment = extractDoccommentForDeclaration(
						sourceContent,
						resolvedSymbol.definitionLine
					);
				}
			}

			if( resolvedSymbol.symbolKind == semantic::Symbol::Kind::Parameter && doccomment.empty() == false ) {
				std::string paramDoc = extractParameterDocFromDocstring( doccomment, resolvedSymbol.symbolName );
				if( paramDoc.empty() == false ) {
					hoverContent += "\n\n---\n\n" + paramDoc;
				}
			}
			else if( resolvedSymbol.symbolKind == semantic::Symbol::Kind::Parameter && doccomment.empty() ) {
				const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();
				const LocalVariableInfo* paramMatch = nullptr;
				for( const LocalVariableInfo& localVar : semanticResult.localVariables ) {
					if( localVar.variableName == resolvedSymbol.symbolName &&
						localVar.declarationLine <= cursorPosition.line ) {
						if( paramMatch == nullptr || localVar.declarationLine > paramMatch->declarationLine ) {
							paramMatch = &localVar;
						}
					}
				}
				if( paramMatch != nullptr && paramMatch->enclosingFunctionName.empty() == false ) {
					std::string functionName = paramMatch->enclosingFunctionName;
					size_t dotPos = functionName.find( '.' );
					if( dotPos != std::string::npos ) {
						functionName = functionName.substr( dotPos + 1 );
					}
					ResolvedSymbolInfo functionInfo = resolveSymbolAtPosition(
						semanticResult, functionName, documentFilePath, cursorPosition.line
					);
					if( functionInfo.resolvedType != nullptr ) {
						std::string functionSourceContent;
						std::string functionFilePath = functionInfo.filePath;
						if( functionFilePath == documentFilePath || functionFilePath.empty() ) {
							functionSourceContent = targetDocument->content();
						}
						else if( std::filesystem::is_regular_file( functionFilePath ) ) {
							std::ifstream functionFileStream( functionFilePath );
							if( functionFileStream.is_open() ) {
								functionSourceContent = std::string(
									std::istreambuf_iterator<char>( functionFileStream ),
									std::istreambuf_iterator<char>()
								);
							}
						}
						if( functionSourceContent.empty() == false ) {
							std::string functionDoc = extractDoccommentForDeclaration(
								functionSourceContent, functionInfo.definitionLine
							);
							if( functionDoc.empty() == false ) {
								std::string paramDoc = extractParameterDocFromDocstring(
									functionDoc, resolvedSymbol.symbolName
								);
								if( paramDoc.empty() == false ) {
									hoverContent += "\n\n---\n\n" + paramDoc;
								}
							}
						}
					}
				}
			}
			else if( doccomment.empty() == false ) {
				std::string formattedDoc = formatDoccommentAsMarkdown( doccomment );
				hoverContent += "\n\n---\n\n" + formattedDoc;
			}

			// Show fields for class/struct types
			semantic::ClassTypeSharedPointer classType =
				std::dynamic_pointer_cast<semantic::ClassType>( resolvedSymbol.resolvedType );
			if( classType != nullptr && classType->fields.empty() == false ) {
				hoverContent += "\n\n**Fields:**\n";
				for( const semantic::FieldInfo& fieldInfo : classType->fields ) {
					std::string canonicalizedFieldType = fieldInfo.type != nullptr
						? canonicalizeTypeForDisplay( fieldInfo.type ) : "?";
					hoverContent += "- `" + canonicalizedFieldType + " " + fieldInfo.name + "`\n";
				}
			}

			semantic::StructTypeSharedPointer structType =
				std::dynamic_pointer_cast<semantic::StructType>( resolvedSymbol.resolvedType );
			if( structType != nullptr && structType->fields.empty() == false ) {
				hoverContent += "\n\n**Fields:**\n";
				for( const semantic::FieldInfo& fieldInfo : structType->fields ) {
					std::string canonicalizedFieldType = fieldInfo.type != nullptr
						? canonicalizeTypeForDisplay( fieldInfo.type ) : "?";
					hoverContent += "- `" + canonicalizedFieldType + " " + fieldInfo.name + "`\n";
				}
			}

			// Show variants for enum types
			semantic::EnumTypeSharedPointer enumType =
				std::dynamic_pointer_cast<semantic::EnumType>( resolvedSymbol.resolvedType );
			if( enumType != nullptr && enumType->variants.empty() == false ) {
				hoverContent += "\n\n**Variants:**\n";
				for( const semantic::EnumVariantInfo& variantInfo : enumType->variants ) {
					hoverContent += "- `unit " + variantInfo.name + "`\n";
				}
			}
		}
		else {
			// Fallback to tree-sitter structural hover
			if( std::strcmp( parentNodeType, "function_declaration" ) == 0 ) {
				hoverContent = "```uranite\nfunction " + hoveredText + "(...)\n```";
			}
			else if( std::strcmp( parentNodeType, "class_declaration" ) == 0 ) {
				hoverContent = "```uranite\nclass " + hoveredText + "\n```";
			}
			else if( std::strcmp( parentNodeType, "struct_declaration" ) == 0 ) {
				hoverContent = "```uranite\nstruct " + hoveredText + "\n```";
			}
			else if( std::strcmp( parentNodeType, "enum_declaration" ) == 0 ) {
				hoverContent = "```uranite\nenum " + hoveredText + "\n```";
			}
			else if( std::strcmp( parentNodeType, "interface_declaration" ) == 0 ) {
				hoverContent = "```uranite\ninterface " + hoveredText + "\n```";
			}
			else if( std::strcmp( parentNodeType, "variable_declaration" ) == 0 ||
					 std::strcmp( parentNodeType, "parameter_declaration" ) == 0 ) {
				TSNode typeNode = ts_node_child_by_field_name( parentNode, "variable_type", 13 );
				if( ts_node_is_null( typeNode ) ) {
					typeNode = ts_node_child_by_field_name( parentNode, "parameter_type", 14 );
				}
				std::string typeName = ts_node_is_null( typeNode ) == false ? targetDocument->nodeText( typeNode ) : "?";
				hoverContent = "```uranite\n" + typeName + " " + hoveredText + "\n```";
			}
			else if( std::strcmp( hoveredNodeType, "type_specifier" ) == 0 ||
					 std::strcmp( parentNodeType, "type_specifier" ) == 0 ) {
				hoverContent = "```uranite\ntype " + hoveredText + "\n```";
			}
			else {
				hoverContent = "`" + hoveredText + "` (" + std::string( hoveredNodeType ) + ")";
			}
		}

		llvm::json::Object hoverContents;
		hoverContents["kind"] = "markdown";
		hoverContents["value"] = hoverContent;

		llvm::json::Object hoverResult;
		hoverResult["contents"] = llvm::json::Value( std::move( hoverContents ) );

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( hoverResult ) ) );
	}

	// ─── Pillar 2: Go-To-Definition ──────────────────────────────────────────────

	void Server::handleTextDocumentDefinition( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );

		TSNode hoveredNode = targetDocument->findNodeAtPosition( cursorPosition );
		if( ts_node_is_null( hoveredNode ) ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		std::string identifierText = targetDocument->nodeText( hoveredNode );
		std::string documentFilePath = this->workspace->uriToFilePath( documentUri );
		const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();

		TSNode parentNode = ts_node_parent( hoveredNode );
		const char* parentNodeType = ts_node_is_null( parentNode ) == false ? ts_node_type( parentNode ) : "";

		bool isMemberAccessContext = (
			std::strcmp( parentNodeType, "member_access_expression" ) == 0 ||
			std::strcmp( parentNodeType, "method_call_expression" ) == 0
		);

		if( isMemberAccessContext ) {
			TSNode objectNode = ts_node_child_by_field_name( parentNode, "accessed_object", 15 );

			if( ts_node_is_null( objectNode ) == false ) {
				semantic::TypeSharedPointer objectType = resolveExpressionNodeType(
					semanticResult, targetDocument, objectNode, cursorPosition.line
				);

				if( objectType != nullptr ) {
					ResolvedSymbolInfo memberInfo = resolveMemberDefinition(
						semanticResult, objectType, identifierText, documentFilePath
					);

					bool hasMemberDefinitionLocation = (
						memberInfo.definitionLine > 0 ||
						memberInfo.definitionColumn > 0 ||
						memberInfo.filePath.empty() == false
					);

					if( hasMemberDefinitionLocation ) {
						std::string targetUri;
						std::string documentBasename = std::filesystem::path( documentFilePath ).filename().string();
						bool memberPathIsBasename = (
							memberInfo.filePath.find( '/' ) == std::string::npos &&
							memberInfo.filePath.find( '\\' ) == std::string::npos
						);
						bool isSameFile = (
							memberInfo.filePath.empty() ||
							memberInfo.filePath == documentFilePath ||
							( memberPathIsBasename && memberInfo.filePath == documentBasename )
						);
						if( isSameFile ) {
							targetUri = documentUri;
						}
						else {
							targetUri = this->workspace->filePathToUri( memberInfo.filePath );
						}

						llvm::json::Object startPosition;
						startPosition["line"] = static_cast<int64_t>( memberInfo.definitionLine );
						startPosition["character"] = static_cast<int64_t>( memberInfo.definitionColumn );

						llvm::json::Object endPosition;
						endPosition["line"] = static_cast<int64_t>( memberInfo.definitionEndLine );
						endPosition["character"] = static_cast<int64_t>( memberInfo.definitionEndColumn );

						llvm::json::Object definitionRange;
						definitionRange["start"] = llvm::json::Value( std::move( startPosition ) );
						definitionRange["end"] = llvm::json::Value( std::move( endPosition ) );

						llvm::json::Object definitionLocation;
						definitionLocation["uri"] = targetUri;
						definitionLocation["range"] = llvm::json::Value( std::move( definitionRange ) );

						this->transport.sendResponse( requestId, llvm::json::Value( std::move( definitionLocation ) ) );
						return;
					}
				}
			}
		}

		ResolvedSymbolInfo resolvedSymbol = resolveSymbolAtPosition(
			semanticResult, identifierText, documentFilePath, cursorPosition.line
		);

		bool hasDefinitionLocation = (
			resolvedSymbol.definitionLine > 0 ||
			resolvedSymbol.definitionColumn > 0 ||
			resolvedSymbol.filePath.empty() == false
		);

		if( hasDefinitionLocation ) {
			std::string targetUri;
			std::string symbolDocBasename = std::filesystem::path( documentFilePath ).filename().string();
			bool symbolPathIsBasename = (
				resolvedSymbol.filePath.find( '/' ) == std::string::npos &&
				resolvedSymbol.filePath.find( '\\' ) == std::string::npos
			);
			bool symbolIsSameFile = (
				resolvedSymbol.filePath.empty() ||
				resolvedSymbol.filePath == documentFilePath ||
				( symbolPathIsBasename && resolvedSymbol.filePath == symbolDocBasename )
			);
			if( symbolIsSameFile ) {
				targetUri = documentUri;
			}
			else {
				targetUri = this->workspace->filePathToUri( resolvedSymbol.filePath );
			}

			llvm::json::Object startPosition;
			startPosition["line"] = static_cast<int64_t>( resolvedSymbol.definitionLine );
			startPosition["character"] = static_cast<int64_t>( resolvedSymbol.definitionColumn );

			llvm::json::Object endPosition;
			endPosition["line"] = static_cast<int64_t>( resolvedSymbol.definitionEndLine );
			endPosition["character"] = static_cast<int64_t>( resolvedSymbol.definitionEndColumn );

			llvm::json::Object definitionRange;
			definitionRange["start"] = llvm::json::Value( std::move( startPosition ) );
			definitionRange["end"] = llvm::json::Value( std::move( endPosition ) );

			llvm::json::Object definitionLocation;
			definitionLocation["uri"] = targetUri;
			definitionLocation["range"] = llvm::json::Value( std::move( definitionRange ) );

			this->transport.sendResponse( requestId, llvm::json::Value( std::move( definitionLocation ) ) );
			return;
		}

		// Fallback: tree-sitter document symbol name match (same-file only)
		std::vector<DocumentSymbol> documentSymbols = targetDocument->collectDocumentSymbols();
		for( const DocumentSymbol& documentSymbol : documentSymbols ) {
			if( documentSymbol.name == identifierText ) {
				llvm::json::Object startPosition;
				startPosition["line"] = static_cast<int64_t>( documentSymbol.selectionRange.start.line );
				startPosition["character"] = static_cast<int64_t>( documentSymbol.selectionRange.start.character );

				llvm::json::Object endPosition;
				endPosition["line"] = static_cast<int64_t>( documentSymbol.selectionRange.end.line );
				endPosition["character"] = static_cast<int64_t>( documentSymbol.selectionRange.end.character );

				llvm::json::Object definitionRange;
				definitionRange["start"] = llvm::json::Value( std::move( startPosition ) );
				definitionRange["end"] = llvm::json::Value( std::move( endPosition ) );

				llvm::json::Object definitionLocation;
				definitionLocation["uri"] = documentUri;
				definitionLocation["range"] = llvm::json::Value( std::move( definitionRange ) );

				this->transport.sendResponse( requestId, llvm::json::Value( std::move( definitionLocation ) ) );
				return;
			}
		}

		this->transport.sendResponse( requestId, nullptr );
	}

	void Server::handleTextDocumentDocumentSymbol( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		llvm::json::Array symbolArray;
		if( targetDocument != nullptr ) {
			std::vector<DocumentSymbol> documentSymbols = targetDocument->collectDocumentSymbols();
			for( const DocumentSymbol& documentSymbol : documentSymbols ) {
				llvm::json::Object symbolRangeStart;
				symbolRangeStart["line"] = static_cast<int64_t>( documentSymbol.range.start.line );
				symbolRangeStart["character"] = static_cast<int64_t>( documentSymbol.range.start.character );

				llvm::json::Object symbolRangeEnd;
				symbolRangeEnd["line"] = static_cast<int64_t>( documentSymbol.range.end.line );
				symbolRangeEnd["character"] = static_cast<int64_t>( documentSymbol.range.end.character );

				llvm::json::Object symbolRange;
				symbolRange["start"] = llvm::json::Value( std::move( symbolRangeStart ) );
				symbolRange["end"] = llvm::json::Value( std::move( symbolRangeEnd ) );

				llvm::json::Object selectionRangeStart;
				selectionRangeStart["line"] = static_cast<int64_t>( documentSymbol.selectionRange.start.line );
				selectionRangeStart["character"] = static_cast<int64_t>( documentSymbol.selectionRange.start.character );

				llvm::json::Object selectionRangeEnd;
				selectionRangeEnd["line"] = static_cast<int64_t>( documentSymbol.selectionRange.end.line );
				selectionRangeEnd["character"] = static_cast<int64_t>( documentSymbol.selectionRange.end.character );

				llvm::json::Object selectionRange;
				selectionRange["start"] = llvm::json::Value( std::move( selectionRangeStart ) );
				selectionRange["end"] = llvm::json::Value( std::move( selectionRangeEnd ) );

				llvm::json::Object symbolObject;
				symbolObject["name"] = documentSymbol.name;
				symbolObject["kind"] = static_cast<int64_t>( documentSymbol.kind );
				symbolObject["range"] = llvm::json::Value( std::move( symbolRange ) );
				symbolObject["selectionRange"] = llvm::json::Value( std::move( selectionRange ) );

				symbolArray.push_back( llvm::json::Value( std::move( symbolObject ) ) );
			}
		}

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( symbolArray ) ) );
	}

	void Server::handleTextDocumentFoldingRange( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		llvm::json::Array foldingRangeArray;
		if( targetDocument != nullptr ) {
			std::vector<FoldingRange> foldingRanges = targetDocument->collectFoldingRanges();
			for( const FoldingRange& foldRange : foldingRanges ) {
				llvm::json::Object foldObject;
				foldObject["startLine"] = static_cast<int64_t>( foldRange.startLine );
				foldObject["startCharacter"] = static_cast<int64_t>( foldRange.startCharacter );
				foldObject["endLine"] = static_cast<int64_t>( foldRange.endLine );
				foldObject["endCharacter"] = static_cast<int64_t>( foldRange.endCharacter );
				if( foldRange.kind.empty() == false ) {
					foldObject["kind"] = foldRange.kind;
				}
				foldingRangeArray.push_back( llvm::json::Value( std::move( foldObject ) ) );
			}
		}

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( foldingRangeArray ) ) );
	}

	void Server::handleTextDocumentSemanticTokensFull( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		llvm::json::Array tokenDataArray;
		if( targetDocument != nullptr ) {
			std::vector<SemanticToken> semanticTokens = targetDocument->collectSemanticTokens();
			for( const SemanticToken& semanticToken : semanticTokens ) {
				tokenDataArray.push_back( static_cast<int64_t>( semanticToken.deltaLine ) );
				tokenDataArray.push_back( static_cast<int64_t>( semanticToken.deltaStartCharacter ) );
				tokenDataArray.push_back( static_cast<int64_t>( semanticToken.length ) );
				tokenDataArray.push_back( static_cast<int64_t>( semanticToken.tokenType ) );
				tokenDataArray.push_back( static_cast<int64_t>( semanticToken.tokenModifiers ) );
			}
		}

		llvm::json::Object semanticTokensResult;
		semanticTokensResult["data"] = llvm::json::Value( std::move( tokenDataArray ) );

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( semanticTokensResult ) ) );
	}

	void Server::handleWorkspaceSymbol( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string queryText;
		if( const llvm::json::Object* paramsObject = requestParams.getAsObject() ) {
			if( const llvm::json::Value* queryValue = paramsObject->get( "query" ) ) {
				if( std::optional<llvm::StringRef> queryString = queryValue->getAsString() ) {
					queryText = queryString->str();
				}
			}
		}

		llvm::json::Array symbolInfoArray;
		if( this->workspace != nullptr ) {
			std::vector<WorkspaceSymbol> matchedSymbols = this->workspace->searchSymbols( queryText );
			for( const WorkspaceSymbol& workspaceSymbol : matchedSymbols ) {
				llvm::json::Object locationStart;
				locationStart["line"] = static_cast<int64_t>( workspaceSymbol.symbolLocation.range.start.line );
				locationStart["character"] = static_cast<int64_t>( workspaceSymbol.symbolLocation.range.start.character );

				llvm::json::Object locationEnd;
				locationEnd["line"] = static_cast<int64_t>( workspaceSymbol.symbolLocation.range.end.line );
				locationEnd["character"] = static_cast<int64_t>( workspaceSymbol.symbolLocation.range.end.character );

				llvm::json::Object locationRange;
				locationRange["start"] = llvm::json::Value( std::move( locationStart ) );
				locationRange["end"] = llvm::json::Value( std::move( locationEnd ) );

				llvm::json::Object symbolLocation;
				symbolLocation["uri"] = workspaceSymbol.symbolLocation.uri;
				symbolLocation["range"] = llvm::json::Value( std::move( locationRange ) );

				llvm::json::Object symbolInfo;
				symbolInfo["name"] = workspaceSymbol.symbolName;
				symbolInfo["kind"] = static_cast<int64_t>( workspaceSymbol.symbolKind );
				symbolInfo["location"] = llvm::json::Value( std::move( symbolLocation ) );

				symbolInfoArray.push_back( llvm::json::Value( std::move( symbolInfo ) ) );
			}
		}

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( symbolInfoArray ) ) );
	}

	// ─── Access Violation Checking ──────────────────────────────────────────────

	static bool isSelfRootedAccess( TSNode node, Document* document ) {
		const char* nodeType = ts_node_type( node );
		if( std::strcmp( nodeType, "self_expression" ) == 0 ||
			std::strcmp( nodeType, "parent_expression" ) == 0 ) {
			return true;
		}
		std::string text = document->nodeText( node );
		if( text == "self" || text == "parent" ) {
			return true;
		}
		if( std::strcmp( nodeType, "member_access_expression" ) == 0 ) {
			TSNode innerObject = ts_node_child_by_field_name( node, "accessed_object", 15 );
			if( !ts_node_is_null( innerObject ) ) {
				return isSelfRootedAccess( innerObject, document );
			}
		}
		if( std::strcmp( nodeType, "call_expression" ) == 0 ) {
			TSNode calledFn = ts_node_child_by_field_name( node, "called_function", 15 );
			if( !ts_node_is_null( calledFn ) ) {
				return isSelfRootedAccess( calledFn, document );
			}
		}
		uint32_t namedChildCount = ts_node_named_child_count( node );
		if( namedChildCount == 1 ) {
			return isSelfRootedAccess( ts_node_named_child( node, 0 ), document );
		}
		return false;
	}

	static void walkTreeForAccessViolations(
		TSNode node,
		Document* document,
		const SemanticAnalysisResult& semanticResult,
		std::vector<SemanticDiagnostic>& violations,
		const std::chrono::steady_clock::time_point& deadline,
		const std::string& enclosingClassName
	) {
		if( ts_node_is_null( node ) ) return;
		if( std::chrono::steady_clock::now() > deadline ) return;

		const char* nodeType = ts_node_type( node );

		if( std::strcmp( nodeType, "member_access_expression" ) == 0 ) {
			TSNode objectNode = ts_node_child_by_field_name( node, "accessed_object", 15 );
			TSNode memberNode = ts_node_child_by_field_name( node, "accessed_member", 15 );

			if( !ts_node_is_null( objectNode ) && !ts_node_is_null( memberNode ) ) {
				bool isInternalAccess = isSelfRootedAccess( objectNode, document );

				if( !isInternalAccess ) {
					uint32_t cursorLine = ts_node_start_point( node ).row;
					semantic::TypeSharedPointer objectType = resolveExpressionNodeType(
						semanticResult, document, objectNode, cursorLine
					);

					if( objectType != nullptr ) {
						bool isSameClassAccess = !enclosingClassName.empty() && objectType->name == enclosingClassName;

						if( !isSameClassAccess ) {
							std::string memberName = document->nodeText( memberNode );
							MemberAccessInfo accessInfo = findMemberAccessModifier( objectType, memberName );

							if( accessInfo.found && accessInfo.access != ast::AccessModifier::Public ) {
								std::string accessLabel = "private";
								if( accessInfo.access == ast::AccessModifier::Protect ) {
									accessLabel = "protected";
								}

								SemanticDiagnostic violation;
								violation.severity = DiagnosticSeverity::ERROR;
								violation.source = "uranite-lsp";

								TSPoint memberStart = ts_node_start_point( memberNode );
								TSPoint memberEnd = ts_node_end_point( memberNode );
								violation.range.start.line = memberStart.row;
								violation.range.start.character = memberStart.column;
								violation.range.end.line = memberEnd.row;
								violation.range.end.character = memberEnd.column;

								violation.message = "cannot access " + accessLabel + " member '" +
									memberName + "' of type '" + accessInfo.ownerTypeName + "'";

								violations.push_back( violation );
							}
						}
					}
				}
			}
		}

		uint32_t childCount = ts_node_child_count( node );
		for( uint32_t i = 0; i < childCount; i++ ) {
			walkTreeForAccessViolations(
				ts_node_child( node, i ), document, semanticResult, violations, deadline, enclosingClassName
			);
		}
	}

	// ─── Pillar 1: Real-Time Semantic Diagnostics Pipeline ───────────────────────

	void Server::publishDiagnostics( Document* targetDocument ) {
		if( targetDocument == nullptr ) return;

		std::vector<Diagnostic> treeSitterDiagnostics = targetDocument->collectTreeErrors();

		llvm::json::Array diagnosticArray;
		for( const Diagnostic& diagnostic : treeSitterDiagnostics ) {
			llvm::json::Object rangeStart;
			rangeStart["line"] = static_cast<int64_t>( diagnostic.range.start.line );
			rangeStart["character"] = static_cast<int64_t>( diagnostic.range.start.character );

			llvm::json::Object rangeEnd;
			rangeEnd["line"] = static_cast<int64_t>( diagnostic.range.end.line );
			rangeEnd["character"] = static_cast<int64_t>( diagnostic.range.end.character );

			llvm::json::Object diagnosticRange;
			diagnosticRange["start"] = llvm::json::Value( std::move( rangeStart ) );
			diagnosticRange["end"] = llvm::json::Value( std::move( rangeEnd ) );

			llvm::json::Object diagnosticObject;
			diagnosticObject["range"] = llvm::json::Value( std::move( diagnosticRange ) );
			diagnosticObject["severity"] = static_cast<int64_t>( diagnostic.severity );
			diagnosticObject["source"] = diagnostic.source;
			diagnosticObject["message"] = diagnostic.message;

			diagnosticArray.push_back( llvm::json::Value( std::move( diagnosticObject ) ) );
		}

		// Merge cached semantic diagnostics
		const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();
		for( const SemanticDiagnostic& semanticDiagnostic : semanticResult.semanticDiagnostics ) {
			llvm::json::Object rangeStart;
			rangeStart["line"] = static_cast<int64_t>( semanticDiagnostic.range.start.line );
			rangeStart["character"] = static_cast<int64_t>( semanticDiagnostic.range.start.character );

			llvm::json::Object rangeEnd;
			rangeEnd["line"] = static_cast<int64_t>( semanticDiagnostic.range.end.line );
			rangeEnd["character"] = static_cast<int64_t>( semanticDiagnostic.range.end.character );

			llvm::json::Object diagnosticRange;
			diagnosticRange["start"] = llvm::json::Value( std::move( rangeStart ) );
			diagnosticRange["end"] = llvm::json::Value( std::move( rangeEnd ) );

			llvm::json::Object diagnosticObject;
			diagnosticObject["range"] = llvm::json::Value( std::move( diagnosticRange ) );
			diagnosticObject["severity"] = static_cast<int64_t>( semanticDiagnostic.severity );
			diagnosticObject["source"] = semanticDiagnostic.source;
			diagnosticObject["message"] = semanticDiagnostic.message;

			diagnosticArray.push_back( llvm::json::Value( std::move( diagnosticObject ) ) );
		}

		if( semanticResult.analysisSucceeded && targetDocument->tree() != nullptr ) {
			std::vector<SemanticDiagnostic> accessViolations;
			TSNode rootNode = ts_tree_root_node( const_cast<TSTree*>( targetDocument->tree() ) );
			auto walkDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );

			std::string enclosingClassName;
			if( semanticResult.analyzedProgram != nullptr ) {
				for( const auto& declaration : semanticResult.analyzedProgram->declarations ) {
					if( declaration != nullptr && declaration->kind == ast::Node::Kind::ClassDeclaration ) {
						auto classDecl = std::dynamic_pointer_cast<ast::nodes::ClassDeclaration>( declaration );
						if( classDecl != nullptr ) {
							enclosingClassName = classDecl->name;
							break;
						}
					}
					if( declaration != nullptr && declaration->kind == ast::Node::Kind::StructDeclaration ) {
						auto structDecl = std::dynamic_pointer_cast<ast::nodes::StructDeclaration>( declaration );
						if( structDecl != nullptr ) {
							enclosingClassName = structDecl->name;
							break;
						}
					}
				}
			}

			walkTreeForAccessViolations( rootNode, targetDocument, semanticResult, accessViolations, walkDeadline, enclosingClassName );

			for( const SemanticDiagnostic& violation : accessViolations ) {
				llvm::json::Object rangeStart;
				rangeStart["line"] = static_cast<int64_t>( violation.range.start.line );
				rangeStart["character"] = static_cast<int64_t>( violation.range.start.character );

				llvm::json::Object rangeEnd;
				rangeEnd["line"] = static_cast<int64_t>( violation.range.end.line );
				rangeEnd["character"] = static_cast<int64_t>( violation.range.end.character );

				llvm::json::Object diagnosticRange;
				diagnosticRange["start"] = llvm::json::Value( std::move( rangeStart ) );
				diagnosticRange["end"] = llvm::json::Value( std::move( rangeEnd ) );

				llvm::json::Object diagnosticObject;
				diagnosticObject["range"] = llvm::json::Value( std::move( diagnosticRange ) );
				diagnosticObject["severity"] = static_cast<int64_t>( violation.severity );
				diagnosticObject["source"] = violation.source;
				diagnosticObject["message"] = violation.message;

				diagnosticArray.push_back( llvm::json::Value( std::move( diagnosticObject ) ) );
			}
		}

		llvm::json::Object publishParams;
		publishParams["uri"] = targetDocument->uri();
		publishParams["diagnostics"] = llvm::json::Value( std::move( diagnosticArray ) );

		this->transport.sendNotification( "textDocument/publishDiagnostics",
			llvm::json::Value( std::move( publishParams ) ) );
	}

	void Server::runSemanticAnalysisAndPublish( Document* targetDocument ) {
		if( targetDocument == nullptr ) return;

		std::string documentFilePath = this->workspace->uriToFilePath( targetDocument->uri() );
		std::string workspaceRootPath = this->workspace->getRootFilePath();

		targetDocument->refreshSemanticAnalysis( documentFilePath, workspaceRootPath );
		this->publishDiagnostics( targetDocument );
	}

	// ─── Signature Help ──────────────────────────────────────────────────────────

	void Server::handleTextDocumentSignatureHelp( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );
		const std::vector<std::string>& documentLines = targetDocument->sourceLines();

		if( cursorPosition.line >= documentLines.size() ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		const std::string& currentLineText = documentLines[cursorPosition.line];
		uint32_t scanColumn = cursorPosition.character;

		uint32_t activeParameterIndex = 0;
		int32_t parenDepth = 0;
		int32_t openParenColumn = -1;

		while( scanColumn > 0 ) {
			scanColumn--;
			char scannedCharacter = currentLineText[scanColumn];

			if( scannedCharacter == ')' || scannedCharacter == ']' ) {
				parenDepth++;
			}
			else if( scannedCharacter == '(' || scannedCharacter == '[' ) {
				if( parenDepth > 0 ) {
					parenDepth--;
				}
				else {
					openParenColumn = static_cast<int32_t>( scanColumn );
					break;
				}
			}
			else if( scannedCharacter == ',' && parenDepth == 0 ) {
				activeParameterIndex++;
			}
		}

		if( openParenColumn < 0 ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		SignatureHelpContext helpContext;
		helpContext.activeParameterIndex = activeParameterIndex;

		uint32_t identifierEnd = static_cast<uint32_t>( openParenColumn );
		while( identifierEnd > 0 && currentLineText[identifierEnd - 1] == ' ' ) {
			identifierEnd--;
		}

		uint32_t identifierStart = identifierEnd;
		while( identifierStart > 0 ) {
			char precedingChar = currentLineText[identifierStart - 1];
			if( ( precedingChar >= 'a' && precedingChar <= 'z' ) ||
				( precedingChar >= 'A' && precedingChar <= 'Z' ) ||
				( precedingChar >= '0' && precedingChar <= '9' ) ||
				precedingChar == '_' ) {
				identifierStart--;
			}
			else {
				break;
			}
		}

		if( identifierStart >= identifierEnd ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		std::string functionName = currentLineText.substr( identifierStart, identifierEnd - identifierStart );

		if( identifierStart > 0 && currentLineText[identifierStart - 1] == '.' ) {
			helpContext.isMemberCall = true;

			Position functionNamePosition;
			functionNamePosition.line = cursorPosition.line;
			functionNamePosition.character = identifierStart > 0 ? identifierStart - 1 : 0;

			TSNode functionNameNode = targetDocument->findNodeAtPosition( functionNamePosition );
			TSNode functionNameParent = ts_node_is_null( functionNameNode ) == false
				? ts_node_parent( functionNameNode ) : functionNameNode;

			const char* functionNameParentType = ts_node_is_null( functionNameParent ) == false
				? ts_node_type( functionNameParent ) : "";

			bool resolvedViaTreeSitter = false;

			if( std::strcmp( functionNameParentType, "member_access_expression" ) == 0 ) {
				TSNode objectExpressionNode = ts_node_child_by_field_name(
					functionNameParent, "accessed_object", 15
				);
				if( ts_node_is_null( objectExpressionNode ) == false ) {
					const SemanticAnalysisResult& earlySemanticResult = targetDocument->semanticResult();
					semantic::TypeSharedPointer chainResolvedType = resolveExpressionNodeType(
						earlySemanticResult, targetDocument, objectExpressionNode, cursorPosition.line
					);
					if( chainResolvedType != nullptr ) {
						helpContext.resolvedObjectType = chainResolvedType;
						resolvedViaTreeSitter = true;
					}
				}
			}

			if( resolvedViaTreeSitter == false ) {
				uint32_t objectEnd = identifierStart - 1;
				uint32_t objectStart = objectEnd;
				while( objectStart > 0 ) {
					char precedingChar = currentLineText[objectStart - 1];
					if( ( precedingChar >= 'a' && precedingChar <= 'z' ) ||
						( precedingChar >= 'A' && precedingChar <= 'Z' ) ||
						( precedingChar >= '0' && precedingChar <= '9' ) ||
						precedingChar == '_' ) {
						objectStart--;
					}
					else {
						break;
					}
				}
				if( objectStart < objectEnd ) {
					helpContext.objectIdentifier = currentLineText.substr( objectStart, objectEnd - objectStart );
				}
			}
		}

		helpContext.functionName = functionName;
		helpContext.cursorLine = cursorPosition.line;

		std::string documentFilePath = this->workspace->uriToFilePath( documentUri );
		const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();

		SignatureHelp signatureHelp = resolveSignatureHelp( semanticResult, helpContext, documentFilePath );

		if( signatureHelp.signatures.empty() ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		llvm::json::Array signaturesArray;
		for( const SignatureInformation& signatureInfo : signatureHelp.signatures ) {
			llvm::json::Object signatureObject;
			signatureObject["label"] = signatureInfo.label;

			if( signatureInfo.documentation.empty() == false ) {
				llvm::json::Object documentationContent;
				documentationContent["kind"] = "markdown";
				documentationContent["value"] = signatureInfo.documentation;
				signatureObject["documentation"] = llvm::json::Value( std::move( documentationContent ) );
			}

			llvm::json::Array parametersArray;
			for( const ParameterInformation& parameterInfo : signatureInfo.parameters ) {
				llvm::json::Object parameterObject;
				parameterObject["label"] = parameterInfo.label;
				if( parameterInfo.documentation.empty() == false ) {
					parameterObject["documentation"] = parameterInfo.documentation;
				}
				parametersArray.push_back( llvm::json::Value( std::move( parameterObject ) ) );
			}
			signatureObject["parameters"] = llvm::json::Value( std::move( parametersArray ) );

			signaturesArray.push_back( llvm::json::Value( std::move( signatureObject ) ) );
		}

		llvm::json::Object signatureHelpResult;
		signatureHelpResult["signatures"] = llvm::json::Value( std::move( signaturesArray ) );
		signatureHelpResult["activeSignature"] = static_cast<int64_t>( signatureHelp.activeSignature );
		signatureHelpResult["activeParameter"] = static_cast<int64_t>( signatureHelp.activeParameter );

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( signatureHelpResult ) ) );
	}

	// ─── Find References ─────────────────────────────────────────────────────────

	void Server::handleTextDocumentReferences( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, llvm::json::Value( llvm::json::Array{} ) );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );

		TSNode hoveredNode = targetDocument->findNodeAtPosition( cursorPosition );
		if( ts_node_is_null( hoveredNode ) ) {
			this->transport.sendResponse( requestId, llvm::json::Value( llvm::json::Array{} ) );
			return;
		}

		std::string identifierText = targetDocument->nodeText( hoveredNode );
		const char* hoveredNodeType = ts_node_type( hoveredNode );

		bool isIdentifierNode = (
			std::strcmp( hoveredNodeType, "identifier" ) == 0 ||
			std::strcmp( hoveredNodeType, "type_identifier" ) == 0
		);

		if( isIdentifierNode == false ) {
			this->transport.sendResponse( requestId, llvm::json::Value( llvm::json::Array{} ) );
			return;
		}

		bool includeDeclaration = true;
		if( const llvm::json::Object* paramsObject = requestParams.getAsObject() ) {
			if( const llvm::json::Value* contextValue = paramsObject->get( "context" ) ) {
				if( const llvm::json::Object* contextObject = contextValue->getAsObject() ) {
					if( const llvm::json::Value* includeValue = contextObject->get( "includeDeclaration" ) ) {
						if( std::optional<bool> includeBool = includeValue->getAsBoolean() ) {
							includeDeclaration = *includeBool;
						}
					}
				}
			}
		}

		std::vector<IdentifierOccurrence> occurrences = findAllOccurrences(
			targetDocument->content(), identifierText
		);

		std::string documentFilePath = this->workspace->uriToFilePath( documentUri );
		const SemanticAnalysisResult& semanticResult = targetDocument->semanticResult();

		ResolvedSymbolInfo resolvedSymbol = resolveSymbolAtPosition(
			semanticResult, identifierText, documentFilePath, cursorPosition.line
		);

		uint32_t declarationLine = resolvedSymbol.definitionLine;
		uint32_t declarationColumn = resolvedSymbol.definitionColumn;
		bool hasDeclarationLocation = (
			resolvedSymbol.definitionLine > 0 ||
			resolvedSymbol.definitionColumn > 0
		);

		llvm::json::Array referencesArray;

		for( const IdentifierOccurrence& occurrence : occurrences ) {
			if( includeDeclaration == false && hasDeclarationLocation ) {
				if( occurrence.startLine == declarationLine && occurrence.startColumn == declarationColumn ) {
					continue;
				}
			}

			llvm::json::Object startPosition;
			startPosition["line"] = static_cast<int64_t>( occurrence.startLine );
			startPosition["character"] = static_cast<int64_t>( occurrence.startColumn );

			llvm::json::Object endPosition;
			endPosition["line"] = static_cast<int64_t>( occurrence.endLine );
			endPosition["character"] = static_cast<int64_t>( occurrence.endColumn );

			llvm::json::Object referenceRange;
			referenceRange["start"] = llvm::json::Value( std::move( startPosition ) );
			referenceRange["end"] = llvm::json::Value( std::move( endPosition ) );

			llvm::json::Object referenceLocation;
			referenceLocation["uri"] = documentUri;
			referenceLocation["range"] = llvm::json::Value( std::move( referenceRange ) );

			referencesArray.push_back( llvm::json::Value( std::move( referenceLocation ) ) );
		}

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( referencesArray ) ) );
	}

	// ─── Rename Symbol ───────────────────────────────────────────────────────────

	void Server::handleTextDocumentPrepareRename( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );

		TSNode hoveredNode = targetDocument->findNodeAtPosition( cursorPosition );
		if( ts_node_is_null( hoveredNode ) ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		const char* hoveredNodeType = ts_node_type( hoveredNode );
		bool isRenameable = (
			std::strcmp( hoveredNodeType, "identifier" ) == 0 ||
			std::strcmp( hoveredNodeType, "type_identifier" ) == 0
		);

		if( isRenameable == false ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		std::string identifierText = targetDocument->nodeText( hoveredNode );

		for( const std::string& keyword : URANITE_KEYWORDS ) {
			if( identifierText == keyword ) {
				this->transport.sendResponse( requestId, nullptr );
				return;
			}
		}

		TSPoint nodeStart = ts_node_start_point( hoveredNode );
		TSPoint nodeEnd = ts_node_end_point( hoveredNode );

		llvm::json::Object rangeStart;
		rangeStart["line"] = static_cast<int64_t>( nodeStart.row );
		rangeStart["character"] = static_cast<int64_t>( nodeStart.column );

		llvm::json::Object rangeEnd;
		rangeEnd["line"] = static_cast<int64_t>( nodeEnd.row );
		rangeEnd["character"] = static_cast<int64_t>( nodeEnd.column );

		llvm::json::Object prepareRange;
		prepareRange["start"] = llvm::json::Value( std::move( rangeStart ) );
		prepareRange["end"] = llvm::json::Value( std::move( rangeEnd ) );

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( prepareRange ) ) );
	}

	void Server::handleTextDocumentRename( const llvm::json::Value& requestId, const llvm::json::Value& requestParams ) {
		std::string documentUri = this->extractDocumentUri( requestParams );
		Document* targetDocument = this->workspace->getDocument( documentUri );

		if( targetDocument == nullptr ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		Position cursorPosition = this->extractCursorPosition( requestParams );

		std::string newName;
		if( const llvm::json::Object* paramsObject = requestParams.getAsObject() ) {
			if( const llvm::json::Value* newNameValue = paramsObject->get( "newName" ) ) {
				if( std::optional<llvm::StringRef> newNameString = newNameValue->getAsString() ) {
					newName = newNameString->str();
				}
			}
		}

		if( newName.empty() ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		TSNode hoveredNode = targetDocument->findNodeAtPosition( cursorPosition );
		if( ts_node_is_null( hoveredNode ) ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		std::string identifierText = targetDocument->nodeText( hoveredNode );
		const char* hoveredNodeType = ts_node_type( hoveredNode );

		bool isRenameable = (
			std::strcmp( hoveredNodeType, "identifier" ) == 0 ||
			std::strcmp( hoveredNodeType, "type_identifier" ) == 0
		);

		if( isRenameable == false ) {
			this->transport.sendResponse( requestId, nullptr );
			return;
		}

		for( const std::string& keyword : URANITE_KEYWORDS ) {
			if( identifierText == keyword ) {
				this->transport.sendResponse( requestId, nullptr );
				return;
			}
		}

		std::vector<IdentifierOccurrence> occurrences = findAllOccurrences(
			targetDocument->content(), identifierText
		);

		llvm::json::Array textEditsArray;
		for( const IdentifierOccurrence& occurrence : occurrences ) {
			llvm::json::Object editStart;
			editStart["line"] = static_cast<int64_t>( occurrence.startLine );
			editStart["character"] = static_cast<int64_t>( occurrence.startColumn );

			llvm::json::Object editEnd;
			editEnd["line"] = static_cast<int64_t>( occurrence.endLine );
			editEnd["character"] = static_cast<int64_t>( occurrence.endColumn );

			llvm::json::Object editRange;
			editRange["start"] = llvm::json::Value( std::move( editStart ) );
			editRange["end"] = llvm::json::Value( std::move( editEnd ) );

			llvm::json::Object textEdit;
			textEdit["range"] = llvm::json::Value( std::move( editRange ) );
			textEdit["newText"] = newName;

			textEditsArray.push_back( llvm::json::Value( std::move( textEdit ) ) );
		}

		llvm::json::Object documentChanges;
		documentChanges[documentUri] = llvm::json::Value( std::move( textEditsArray ) );

		llvm::json::Object workspaceEdit;
		workspaceEdit["changes"] = llvm::json::Value( std::move( documentChanges ) );

		this->transport.sendResponse( requestId, llvm::json::Value( std::move( workspaceEdit ) ) );
	}

} // namespace uranite::lsp
