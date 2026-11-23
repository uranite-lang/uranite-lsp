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

#include "uranite-lsp/semantic/symbol-index.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "uranite/lookup/source.hpp"
#include "uranite/semantic/scope.hpp"
#include "uranite/semantic/symbol.hpp"
#include "uranite/semantic/typeref.hpp"

namespace uranite::lsp::semantic {

	namespace {

		using uranite::ast::Node;
		using uranite::ast::nodes::ArrayExpression;
		using uranite::ast::nodes::ArrayTypeNode;
		using uranite::ast::nodes::AssignStatement;
		using uranite::ast::nodes::AwaitExpression;
		using uranite::ast::nodes::BinaryExpression;
		using uranite::ast::nodes::BlockStatement;
		using uranite::ast::nodes::BoolLiteralExpression;
		using uranite::ast::nodes::BreakStatement;
		using uranite::ast::nodes::CallExpression;
		using uranite::ast::nodes::CallableTypeNode;
		using uranite::ast::nodes::CastExpression;
		using uranite::ast::nodes::CharLiteralExpression;
		using uranite::ast::nodes::ClassDeclaration;
		using uranite::ast::nodes::ComprehensionExpression;
		using uranite::ast::nodes::ConstantDeclaration;
		using uranite::ast::nodes::ConstructExpression;
		using uranite::ast::nodes::ContinueStatement;
		using uranite::ast::nodes::DeferStatement;
		using uranite::ast::nodes::DeleteStatement;
		using uranite::ast::nodes::EnumDeclaration;
		using uranite::ast::nodes::EnumVariantNode;
		using uranite::ast::nodes::ExportDeclaration;
		using uranite::ast::nodes::ExpressionStatement;
		using uranite::ast::nodes::ExternDeclaration;
		using uranite::ast::nodes::FieldDeclarationNode;
		using uranite::ast::nodes::FloatLiteralExpression;
		using uranite::ast::nodes::ForStatement;
		using uranite::ast::nodes::FunctionDeclaration;
		using uranite::ast::nodes::FunctionParameterNode;
		using uranite::ast::nodes::FunctionTypeNode;
		using uranite::ast::nodes::GenericParameterNode;
		using uranite::ast::nodes::GenericTypeNode;
		using uranite::ast::nodes::IdentifierExpression;
		using uranite::ast::nodes::IfStatement;
		using uranite::ast::nodes::ImplementDeclaration;
		using uranite::ast::nodes::ImportDeclaration;
		using uranite::ast::nodes::IndexExpression;
		using uranite::ast::nodes::InlineAssemblyStatement;
		using uranite::ast::nodes::InstanceofExpression;
		using uranite::ast::nodes::IntegerLiteralExpression;
		using uranite::ast::nodes::InterfaceDeclaration;
		using uranite::ast::nodes::LambdaExpression;
		using uranite::ast::nodes::MapLiteralExpression;
		using uranite::ast::nodes::MatchArmNode;
		using uranite::ast::nodes::MatchExpression;
		using uranite::ast::nodes::MatchStatement;
		using uranite::ast::nodes::MemberAccessExpression;
		using uranite::ast::nodes::MetaTypeNode;
		using uranite::ast::nodes::MethodCallExpression;
		using uranite::ast::nodes::ModuleDeclaration;
		using uranite::ast::nodes::NoneLiteralExpression;
		using uranite::ast::nodes::OptionalTypeNode;
		using uranite::ast::nodes::ParentExpression;
		using uranite::ast::nodes::PassStatement;
		using uranite::ast::nodes::PointerTypeNode;
		using uranite::ast::nodes::Program;
		using uranite::ast::nodes::RangeExpression;
		using uranite::ast::nodes::ReferenceTypeNode;
		using uranite::ast::nodes::RegexLiteralExpression;
		using uranite::ast::nodes::ReturnStatement;
		using uranite::ast::nodes::SelfExpression;
		using uranite::ast::nodes::SetLiteralExpression;
		using uranite::ast::nodes::SimpleTypeNode;
		using uranite::ast::nodes::StringLiteralExpression;
		using uranite::ast::nodes::StructDeclaration;
		using uranite::ast::nodes::SubclassofExpression;
		using uranite::ast::nodes::SwitchCaseNode;
		using uranite::ast::nodes::SwitchStatement;
		using uranite::ast::nodes::ThrowStatement;
		using uranite::ast::nodes::TraitDeclaration;
		using uranite::ast::nodes::TryCatchStatement;
		using uranite::ast::nodes::TupleExpression;
		using uranite::ast::nodes::TupleTypeNode;
		using uranite::ast::nodes::TypeAliasDeclaration;
		using uranite::ast::nodes::TypeReferenceExpression;
		using uranite::ast::nodes::UnaryExpression;
		using uranite::ast::nodes::UnionTypeNode;
		using uranite::ast::nodes::UnsafeBlockStatement;
		using uranite::ast::nodes::VariableStatement;
		using uranite::ast::nodes::WhileStatement;
		using uranite::ast::nodes::YieldExpression;
		using uranite::lookup::SourceSharedPointer;
		using uranite::semantic::ScopeSharedPointer;
		using uranite::semantic::Symbol;
		using uranite::semantic::SymbolSharedPointer;
		using uranite::semantic::TypeSharedPointer;

		void toLsp( const SourceSharedPointer& source, uint32_t& line, uint32_t& col,
		            uint32_t& endLine, uint32_t& endCol, std::string& pathname,
		            std::size_t identifierLength = 0 ) {
			line = 0;
			col = 0;
			endLine = 0;
			endCol = 0;
			if( source == nullptr ) return;

			// The lexer stores the full path in filename and its parent directory
			// in pathname; LSP locations need the full path.
			pathname = source->filename.empty() == false && source->filename != "<unknown>"
				? source->filename : source->pathname;
			if( source->location != nullptr ) {
				line = source->location->line > 0 ? source->location->line - 1 : 0;
				col = source->location->column > 0 ? source->location->column - 1 : 0;
			}
			if( source->range != nullptr && source->range->end != nullptr ) {
				endLine = source->range->end->line > 0 ? source->range->end->line - 1 : line;
				endCol = source->range->end->column > 0 ? source->range->end->column - 1 : col;
			}
			else {
				endLine = line;
				endCol = col;
			}
			if( endLine < line || ( endLine == line && endCol < col ) ) {
				endLine = line;
				endCol = col;
			}
			// Identifier sources frequently carry a single-point range. Extend
			// it by the identifier length so LSP positions have a real span.
			if( endLine == line && endCol == col && identifierLength > 0 ) {
				endCol = col + static_cast<uint32_t>( identifierLength );
			}
		}

		std::string makeQualified( const std::string& module, const std::string& container, const std::string& name ) {
			std::string result = module.empty() == false ? module : "";
			if( container.empty() == false && container != module ) {
				if( result.empty() == false ) result += ".";
				result += container;
			}
			if( result.empty() == false ) result += ".";
			result += name;
			return result;
		}

		// Scope has no name of its own; the nearest enclosing class/type or
		// function symbol provides the container name. This uses only compiler
		// scope and symbol data, never source-text matching.
		std::string containerFromScope( const ScopeSharedPointer& scope ) {
			ScopeSharedPointer parent = scope == nullptr ? nullptr : scope->parent();
			while( parent != nullptr ) {
				if( parent->classType != nullptr && parent->classType->name.empty() == false ) {
					return parent->classType->name;
				}
				for( const auto& entry : parent->symbols() ) {
					for( const SymbolSharedPointer& symbol : entry.second ) {
						if( symbol == nullptr ) continue;
						if( symbol->kind == Symbol::Kind::Type || symbol->kind == Symbol::Kind::Function ) {
							return symbol->name;
						}
					}
				}
				parent = parent->parent();
			}
			return "";
		}

		bool contains( uint32_t line, uint32_t col, uint32_t startLine, uint32_t startCol,
		               uint32_t endLine, uint32_t endCol ) {
			if( line < startLine || line > endLine ) return false;
			if( line == startLine && col < startCol ) return false;
			if( line == endLine && col > endCol ) return false;
			return true;
		}

		uint64_t span( uint32_t startLine, uint32_t startCol, uint32_t endLine, uint32_t endCol ) {
			const uint64_t lines = static_cast<uint64_t>( endLine ) - static_cast<uint64_t>( startLine );
			return lines * 1000000ULL + static_cast<uint64_t>( endCol ) - static_cast<uint64_t>( startCol ) + 1;
		}

		/** @brief Traverses every AST node kind and records resolved identifier usages and calls. */
		class NodeWalker {
			public:
				NodeWalker( SymbolIndex& index, std::set<const Symbol*>& seenDefinitions,
				            const std::unordered_map<std::string, TypeSharedPointer>& moduleTypes,
				            const std::unordered_map<std::string, SymbolSharedPointer>& typeSymbols )
					: index( index ), seenDefinitions( seenDefinitions ), moduleTypes( moduleTypes ), typeSymbols( typeSymbols ) {
				}

				void walk( const Program& program ) {
					this->walkProgram( program );
				}

			private:
				SymbolIndex& index;
				std::set<const Symbol*>& seenDefinitions;
				std::set<std::string> seenTypeDefinitions;
				const std::unordered_map<std::string, TypeSharedPointer>& moduleTypes;
				const std::unordered_map<std::string, SymbolSharedPointer>& typeSymbols;
				std::vector<uint32_t> argumentLines;
				std::vector<uint32_t> argumentCols;

				void walkNode( const Node& node ) {
					switch( node.kind ) {
						case Node::Kind::ExportDeclaration: this->walkExport( static_cast<const ExportDeclaration&>( node ) ); break;
						case Node::Kind::ImportDeclaration: this->walkImport( static_cast<const ImportDeclaration&>( node ) ); break;
						case Node::Kind::ModuleDeclaration: this->walkModule( static_cast<const ModuleDeclaration&>( node ) ); break;
						case Node::Kind::Program: this->walkProgram( static_cast<const Program&>( node ) ); break;

						case Node::Kind::ClassDeclaration: this->walkClass( static_cast<const ClassDeclaration&>( node ) ); break;
						case Node::Kind::ConstantDeclaration: this->walkConstant( static_cast<const ConstantDeclaration&>( node ) ); break;
						case Node::Kind::EnumDeclaration: this->walkEnum( static_cast<const EnumDeclaration&>( node ) ); break;
						case Node::Kind::ExternDeclaration: this->walkExtern( static_cast<const ExternDeclaration&>( node ) ); break;
						case Node::Kind::FunctionDeclaration: this->walkFunction( static_cast<const FunctionDeclaration&>( node ) ); break;
						case Node::Kind::ImplementationDeclaration: this->walkImplement( static_cast<const ImplementDeclaration&>( node ) ); break;
						case Node::Kind::InterfaceDeclaration: this->walkInterface( static_cast<const InterfaceDeclaration&>( node ) ); break;
						case Node::Kind::StructDeclaration: this->walkStruct( static_cast<const StructDeclaration&>( node ) ); break;
						case Node::Kind::TraitDeclaration: this->walkTrait( static_cast<const TraitDeclaration&>( node ) ); break;
						case Node::Kind::TypeAliasDeclaration: this->walkTypeAlias( static_cast<const TypeAliasDeclaration&>( node ) ); break;

						case Node::Kind::AssignmentStatement: this->walkAssign( static_cast<const AssignStatement&>( node ) ); break;
						case Node::Kind::BlockStatement: this->walkBlock( static_cast<const BlockStatement&>( node ) ); break;
						case Node::Kind::BreakStatement: this->walkLeaf( node ); break;
						case Node::Kind::ContinueStatement: this->walkLeaf( node ); break;
						case Node::Kind::DeferStatement: this->walkDefer( static_cast<const DeferStatement&>( node ) ); break;
						case Node::Kind::DeleteStatement: this->walkDelete( static_cast<const DeleteStatement&>( node ) ); break;
						case Node::Kind::ExpressionStatement: this->walkExpressionStatement( static_cast<const ExpressionStatement&>( node ) ); break;
						case Node::Kind::ForStatement: this->walkFor( static_cast<const ForStatement&>( node ) ); break;
						case Node::Kind::IfStatement: this->walkIf( static_cast<const IfStatement&>( node ) ); break;
						case Node::Kind::InlineAssemblyStatement: this->walkInlineAssembly( static_cast<const InlineAssemblyStatement&>( node ) ); break;
						case Node::Kind::MatchStatement: this->walkMatchStatement( static_cast<const MatchStatement&>( node ) ); break;
						case Node::Kind::PassStatement: this->walkLeaf( node ); break;
						case Node::Kind::ReturnStatement: this->walkReturn( static_cast<const ReturnStatement&>( node ) ); break;
						case Node::Kind::SwitchCase: this->walkSwitchCase( static_cast<const SwitchCaseNode&>( node ) ); break;
						case Node::Kind::SwitchStatement: this->walkSwitch( static_cast<const SwitchStatement&>( node ) ); break;
						case Node::Kind::ThrowStatement: this->walkThrow( static_cast<const ThrowStatement&>( node ) ); break;
						case Node::Kind::TryCatchStatement: this->walkTryCatch( static_cast<const TryCatchStatement&>( node ) ); break;
						case Node::Kind::UnsafeBlock: this->walkUnsafe( static_cast<const UnsafeBlockStatement&>( node ) ); break;
						case Node::Kind::VariableStatement: this->walkVariable( static_cast<const VariableStatement&>( node ) ); break;
						case Node::Kind::WhileStatement: this->walkWhile( static_cast<const WhileStatement&>( node ) ); break;

						case Node::Kind::ArrayExpression: this->walkArray( static_cast<const ArrayExpression&>( node ) ); break;
						case Node::Kind::AwaitExpression: this->walkAwait( static_cast<const AwaitExpression&>( node ) ); break;
						case Node::Kind::BinaryExpression: this->walkBinary( static_cast<const BinaryExpression&>( node ) ); break;
						case Node::Kind::BooleanLiteral: this->walkLeaf( node ); break;
						case Node::Kind::CallExpression: this->walkCall( static_cast<const CallExpression&>( node ) ); break;
						case Node::Kind::CastExpression: this->walkCast( static_cast<const CastExpression&>( node ) ); break;
						case Node::Kind::CharLiteral: this->walkLeaf( node ); break;
						case Node::Kind::ComprehensionExpression: this->walkComprehension( static_cast<const ComprehensionExpression&>( node ) ); break;
						case Node::Kind::ConstructExpression: this->walkConstruct( static_cast<const ConstructExpression&>( node ) ); break;
						case Node::Kind::FloatLiteral: this->walkLeaf( node ); break;
						case Node::Kind::IdentifierExpression: this->walkIdentifier( static_cast<const IdentifierExpression&>( node ) ); break;
						case Node::Kind::IndexExpression: this->walkIndex( static_cast<const IndexExpression&>( node ) ); break;
						case Node::Kind::InstanceofExpression: this->walkInstanceof( static_cast<const InstanceofExpression&>( node ) ); break;
						case Node::Kind::IntegerLiteral: this->walkLeaf( node ); break;
						case Node::Kind::LambdaExpression: this->walkLambda( static_cast<const LambdaExpression&>( node ) ); break;
						case Node::Kind::MapLiteralExpression: this->walkMapLiteral( static_cast<const MapLiteralExpression&>( node ) ); break;
						case Node::Kind::MatchArm: this->walkMatchArm( static_cast<const MatchArmNode&>( node ) ); break;
						case Node::Kind::MatchExpression: this->walkMatchExpression( static_cast<const MatchExpression&>( node ) ); break;
						case Node::Kind::MemberAccessExpression: this->walkMemberAccess( static_cast<const MemberAccessExpression&>( node ) ); break;
						case Node::Kind::MethodCallExpression: this->walkMethodCall( static_cast<const MethodCallExpression&>( node ) ); break;
						case Node::Kind::NoneLiteral: this->walkLeaf( node ); break;
						case Node::Kind::RangeExpression: this->walkRange( static_cast<const RangeExpression&>( node ) ); break;
						case Node::Kind::RegexLiteral: this->walkLeaf( node ); break;
						case Node::Kind::SelfExpression: this->walkSelf( static_cast<const SelfExpression&>( node ) ); break;
						case Node::Kind::SetLiteralExpression: this->walkSetLiteral( static_cast<const SetLiteralExpression&>( node ) ); break;
						case Node::Kind::StringLiteral: this->walkLeaf( node ); break;
						case Node::Kind::SubclassofExpression: this->walkSubclassof( static_cast<const SubclassofExpression&>( node ) ); break;
						case Node::Kind::ParentExpression: this->walkParent( static_cast<const ParentExpression&>( node ) ); break;
						case Node::Kind::TupleExpression: this->walkTuple( static_cast<const TupleExpression&>( node ) ); break;
						case Node::Kind::TypeReferenceExpression: this->walkTypeReference( static_cast<const TypeReferenceExpression&>( node ) ); break;
						case Node::Kind::UnaryExpression: this->walkUnary( static_cast<const UnaryExpression&>( node ) ); break;
						case Node::Kind::YieldExpression: this->walkYield( static_cast<const YieldExpression&>( node ) ); break;

						case Node::Kind::ArrayType: this->walkArrayType( static_cast<const ArrayTypeNode&>( node ) ); break;
						case Node::Kind::CallableType: this->walkCallableType( static_cast<const CallableTypeNode&>( node ) ); break;
						case Node::Kind::FunctionType: this->walkFunctionType( static_cast<const FunctionTypeNode&>( node ) ); break;
						case Node::Kind::GenericType: this->walkGenericType( static_cast<const GenericTypeNode&>( node ) ); break;
						case Node::Kind::MetaType: this->walkMetaType( static_cast<const MetaTypeNode&>( node ) ); break;
						case Node::Kind::OptionalType: this->walkOptionalType( static_cast<const OptionalTypeNode&>( node ) ); break;
						case Node::Kind::PointerType: this->walkPointerType( static_cast<const PointerTypeNode&>( node ) ); break;
						case Node::Kind::ReferenceType: this->walkReferenceType( static_cast<const ReferenceTypeNode&>( node ) ); break;
						case Node::Kind::ResultType: this->walkLeaf( node ); break;
						case Node::Kind::SimpleType: this->walkSimpleType( static_cast<const SimpleTypeNode&>( node ) ); break;
						case Node::Kind::TupleType: this->walkTupleType( static_cast<const TupleTypeNode&>( node ) ); break;
						case Node::Kind::UnionType: this->walkUnionType( static_cast<const UnionTypeNode&>( node ) ); break;

						case Node::Kind::EnumVariant: this->walkEnumVariant( static_cast<const EnumVariantNode&>( node ) ); break;
						case Node::Kind::FieldDeclaration: this->walkField( static_cast<const FieldDeclarationNode&>( node ) ); break;
						case Node::Kind::FunctionParameter: this->walkFunctionParameter( static_cast<const FunctionParameterNode&>( node ) ); break;
						case Node::Kind::GenericParameter: this->walkGenericParameter( static_cast<const GenericParameterNode&>( node ) ); break;
					}
				}

				void walkOptionalNode( const Node* node ) {
					if( node != nullptr ) this->walkNode( *node );
				}

				template <typename T>
				void walkOptional( const std::shared_ptr<T>& node ) {
					if( node != nullptr ) this->walkNode( *node );
				}

				template <typename T>
				void walkVector( const std::vector<std::shared_ptr<T>>& nodes ) {
					for( const std::shared_ptr<T>& node : nodes ) {
						if( node != nullptr ) this->walkNode( *node );
					}
				}

				void walkLeaf( const Node& /*node*/ ) {
				}

				void collectSymbol( const SymbolSharedPointer& symbol, const std::string& name,
				                    const SourceSharedPointer& source, const TypeSharedPointer& type ) {
					if( symbol == nullptr ) return;

					IndexedSymbol reference;
					reference.name = name;
					reference.qualifiedName = makeQualified( symbol->sourceModule, "", symbol->name );
					reference.kind = symbol->kind;
					reference.symbol = symbol;
					reference.type = type != nullptr ? type : symbol->typeref;
					reference.container = symbol->sourceModule;
					reference.isDefinition = false;
					toLsp( source, reference.line, reference.col, reference.endLine, reference.endCol,
						reference.pathname, name.size() );
					this->index.addReference( reference );

					if( this->seenDefinitions.insert( symbol.get() ).second == false ) return;
					IndexedSymbol definition;
					definition.name = symbol->name;
					definition.qualifiedName = reference.qualifiedName;
					definition.kind = symbol->kind;
					definition.symbol = symbol;
					definition.type = symbol->typeref;
					definition.container = symbol->sourceModule;
					definition.isDefinition = true;
					toLsp( symbol->source, definition.line, definition.col, definition.endLine, definition.endCol,
						definition.pathname, symbol->name.size() );
					this->index.addDefinition( definition );
				}

				void collectIdentifier( const uranite::ast::nodes::Expression& node, const std::string& name ) {
					this->collectSymbol( node.resolvedSymbol, name, node.source,
						node.semanticType != nullptr ? node.semanticType : ( node.resolvedSymbol != nullptr ? node.resolvedSymbol->typeref : nullptr ) );
				}

				void collectTypeDefinition( const std::string& name, const SourceSharedPointer& source,
				                             const TypeSharedPointer& type, const std::string& qualifiedName,
				                             Symbol::Kind kind = Symbol::Kind::Type ) {
					if( name.empty() || qualifiedName.empty() ) return;
					if( this->seenTypeDefinitions.insert( qualifiedName ).second == false ) return;
					IndexedSymbol definition;
					definition.name = name;
					definition.qualifiedName = qualifiedName;
					definition.kind = kind;
					definition.type = type;
					definition.container = "";
					definition.isDefinition = true;
					toLsp( source, definition.line, definition.col, definition.endLine, definition.endCol,
						definition.pathname, name.size() );
					this->index.addDefinition( definition );
				}

				void collectTypeRef( const std::string& name, const SourceSharedPointer& source,
				                     const TypeSharedPointer& type, const std::string& qualifiedName,
				                     Symbol::Kind kind = Symbol::Kind::Type ) {
					if( name.empty() || qualifiedName.empty() ) return;
					IndexedSymbol reference;
					reference.name = name;
					reference.qualifiedName = qualifiedName;
					reference.kind = kind;
					reference.type = type;
					reference.container = "";
					reference.isDefinition = false;
					toLsp( source, reference.line, reference.col, reference.endLine, reference.endCol,
						reference.pathname, name.size() );
					this->index.addReference( reference );
				}

				void collectTypeDeclaration( const std::string& name, const SourceSharedPointer& source ) {
					if( name.empty() ) return;
					const auto typeFound = this->moduleTypes.find( name );
					if( typeFound == this->moduleTypes.end() || typeFound->second == nullptr ) return;
					const TypeSharedPointer& type = typeFound->second;
					const std::string qualified = type->qualified.empty() == false ? type->qualified : type->name;
					this->collectTypeDefinition( name, source, type, qualified );
				}

				void collectTypeName( const uranite::ast::nodes::TypeNode& node, const std::string& name ) {
					if( name.empty() ) return;
					const auto found = this->typeSymbols.find( name );
					if( found != this->typeSymbols.end() && found->second != nullptr ) {
						this->collectSymbol( found->second, name, node.source, found->second->typeref );
						return;
					}
					const auto typeFound = this->moduleTypes.find( name );
					if( typeFound == this->moduleTypes.end() || typeFound->second == nullptr ) return;
					const TypeSharedPointer& type = typeFound->second;
					const std::string qualified = type->qualified.empty() == false ? type->qualified : type->name;
					this->collectTypeRef( name, node.source, type, qualified );
				}

				void collectCall( const uranite::ast::nodes::Expression& node, const std::string& name,
				                  const SymbolSharedPointer& explicitSymbol = nullptr,
				                  const TypeSharedPointer& explicitType = nullptr ) {
					SymbolSharedPointer symbol = node.resolvedSymbol != nullptr ? node.resolvedSymbol : explicitSymbol;
					std::vector<TypeSharedPointer> candidates;
					if( symbol != nullptr && symbol->typeref != nullptr ) candidates.push_back( symbol->typeref );
					if( symbol != nullptr ) {
						const auto found = this->moduleTypes.find( symbol->name );
						if( found != this->moduleTypes.end() && found->second != nullptr ) candidates.push_back( found->second );
					}
					if( explicitType != nullptr ) candidates.push_back( explicitType );
					if( node.semanticType != nullptr ) candidates.push_back( node.semanticType );

					TypeSharedPointer type;
					for( const TypeSharedPointer& candidate : candidates ) {
						if( std::dynamic_pointer_cast<uranite::semantic::FunctionType>( candidate ) != nullptr ) {
							type = candidate;
							break;
						}
					}
					if( type == nullptr && candidates.empty() == false ) type = candidates.front();
					if( type == nullptr ) return;

					IndexedCall call;
					call.name = name;
					call.symbol = symbol;
					call.type = type;
					call.qualifiedName = symbol == nullptr ? name : makeQualified( symbol->sourceModule, "", symbol->name );
					call.argumentLines = this->argumentLines;
					call.argumentCols = this->argumentCols;
					toLsp( node.source, call.line, call.col, call.endLine, call.endCol, call.pathname, name.size() );
					this->index.addCall( call );
				}

				void recordArguments( const std::vector<uranite::ast::nodes::ExpressionSharedPointer>& arguments ) {
					for( const uranite::ast::nodes::ExpressionSharedPointer& argument : arguments ) {
						if( argument == nullptr ) continue;
						const bool hasLocation = argument->source != nullptr && argument->source->location != nullptr;
						this->argumentLines.push_back( hasLocation && argument->source->location->line > 0
							? argument->source->location->line - 1 : 0 );
						this->argumentCols.push_back( hasLocation && argument->source->location->column > 0
							? argument->source->location->column - 1 : 0 );
					}
				}

				void walkProgram( const Program& node ) {
					this->walkOptional( node.module );
					this->walkVector( node.imports );
					this->walkVector( node.exports );
					this->walkVector( node.declarations );
				}

				void walkExport( const ExportDeclaration& node ) {
					this->walkOptional( node.declaration );
				}

				void walkImport( const ImportDeclaration& /*node*/ ) {
				}

				void walkModule( const ModuleDeclaration& /*node*/ ) {
				}

				void walkClass( const ClassDeclaration& node ) {
					this->collectTypeDeclaration( node.name, node.source );
					this->walkVector( node.genericParameters );
					this->walkOptional( node.baseClassType );
					this->walkVector( node.interfaces );
					this->walkVector( node.fields );
					this->walkVector( node.methods );
					this->walkVector( node.nestedDeclarations );
				}

				void walkConstant( const ConstantDeclaration& node ) {
					this->walkOptional( node.type );
					this->walkOptional( node.initializer );
				}

				void walkEnum( const EnumDeclaration& node ) {
					this->collectTypeDeclaration( node.name, node.source );
					this->walkVector( node.genericParameters );
					this->walkOptional( node.backedType );
					this->walkOptional( node.baseClassType );
					this->walkVector( node.interfaces );
					this->walkVector( node.variants );
					this->walkVector( node.methods );
				}

				void walkExtern( const ExternDeclaration& node ) {
					for( const uranite::ast::nodes::ExternParameter& parameter : node.parameters ) {
						this->walkOptional( parameter.type );
					}
					this->walkOptional( node.returnType );
				}

				void walkFunction( const FunctionDeclaration& node ) {
					this->walkVector( node.genericParameters );
					this->walkVector( node.parameters );
					this->walkOptional( node.returnType );
					this->walkVector( node.raisesTypes );
					this->walkVector( node.body );
					this->walkVector( node.nestedFunctions );
				}

				void walkImplement( const ImplementDeclaration& node ) {
					this->walkVector( node.genericParameters );
					this->walkOptional( node.targetType );
					this->walkOptional( node.interfaceType );
					this->walkVector( node.methods );
				}

				void walkInterface( const InterfaceDeclaration& node ) {
					this->collectTypeDeclaration( node.name, node.source );
					this->walkVector( node.genericParameters );
					this->walkVector( node.parentInterfaces );
					this->walkVector( node.methods );
					this->walkVector( node.nestedDeclarations );
				}

				void walkStruct( const StructDeclaration& node ) {
					this->collectTypeDeclaration( node.name, node.source );
					this->walkVector( node.genericParameters );
					this->walkVector( node.fields );
					this->walkVector( node.methods );
					this->walkVector( node.nestedDeclarations );
				}

				void walkTrait( const TraitDeclaration& node ) {
					this->collectTypeDeclaration( node.name, node.source );
					this->walkVector( node.genericParameters );
					this->walkVector( node.fields );
					this->walkVector( node.methods );
				}

				void walkTypeAlias( const TypeAliasDeclaration& node ) {
					this->collectTypeDeclaration( node.name, node.source );
					this->walkVector( node.genericParameters );
					this->walkOptional( node.aliasedTypeNode );
				}

				void walkAssign( const AssignStatement& node ) {
					this->walkOptional( node.target );
					this->walkOptional( node.value );
				}

				void walkBlock( const BlockStatement& node ) {
					this->walkVector( node.statements );
				}

				void walkDefer( const DeferStatement& node ) {
					this->walkOptional( node.body );
				}

				void walkDelete( const DeleteStatement& node ) {
					this->walkOptional( node.expression );
				}

				void walkExpressionStatement( const ExpressionStatement& node ) {
					this->walkOptional( node.expression );
				}

				void walkFor( const ForStatement& node ) {
					this->walkOptional( node.variableType );
					this->walkOptional( node.variableType2 );
					this->walkOptional( node.initializer );
					this->walkOptional( node.condition );
					this->walkOptional( node.iterable );
					this->walkOptional( node.update );
					this->walkVector( node.body );
				}

				void walkIf( const IfStatement& node ) {
					this->walkOptional( node.condition );
					this->walkVector( node.thenBody );
					for( const auto& branch : node.elifBranches ) {
						this->walkOptional( branch.first );
						this->walkVector( branch.second );
					}
					this->walkVector( node.elseBody );
				}

				void walkInlineAssembly( const InlineAssemblyStatement& node ) {
					for( const uranite::ast::nodes::InlineAssemblyOperand& operand : node.outputs ) {
						this->walkOptional( operand.expression );
					}
					for( const uranite::ast::nodes::InlineAssemblyOperand& operand : node.inputs ) {
						this->walkOptional( operand.expression );
					}
					for( const uranite::ast::nodes::InlineAssemblyArchVariant& variant : node.archVariants ) {
						for( const uranite::ast::nodes::InlineAssemblyOperand& operand : variant.outputs ) {
							this->walkOptional( operand.expression );
						}
						for( const uranite::ast::nodes::InlineAssemblyOperand& operand : variant.inputs ) {
							this->walkOptional( operand.expression );
						}
					}
				}

				void walkMatchStatement( const MatchStatement& node ) {
					this->walkOptional( node.subject );
					this->walkVector( node.arms );
				}

				void walkReturn( const ReturnStatement& node ) {
					this->walkOptional( node.value );
				}

				void walkSwitchCase( const SwitchCaseNode& node ) {
					this->walkOptional( node.pattern );
					this->walkVector( node.body );
				}

				void walkSwitch( const SwitchStatement& node ) {
					this->walkOptional( node.subject );
					this->walkVector( node.cases );
				}

				void walkThrow( const ThrowStatement& node ) {
					this->walkOptional( node.expression );
				}

				void walkTryCatch( const TryCatchStatement& node ) {
					this->walkVector( node.tryBody );
					for( const uranite::ast::nodes::ExceptionClause& clause : node.exceptionClauses ) {
						for( const uranite::ast::nodes::TypeNodeSharedPointer& type : clause.exceptionTypes ) {
							this->walkOptional( type );
						}
						this->walkVector( clause.body );
					}
					this->walkVector( node.finallyBody );
				}

				void walkUnsafe( const UnsafeBlockStatement& node ) {
					this->walkVector( node.body );
				}

				void walkVariable( const VariableStatement& node ) {
					this->walkOptional( node.type );
					this->walkOptional( node.initializer );
				}

				void walkWhile( const WhileStatement& node ) {
					this->walkOptional( node.condition );
					this->walkVector( node.body );
				}

				void walkArray( const ArrayExpression& node ) {
					this->walkVector( node.elements );
				}

				void walkAwait( const AwaitExpression& node ) {
					this->walkOptional( node.operand );
				}

				void walkBinary( const BinaryExpression& node ) {
					this->walkOptional( node.left );
					this->walkOptional( node.right );
				}

				void walkCall( const CallExpression& node ) {
					std::string calleeName;
					SymbolSharedPointer calleeSymbol;
					TypeSharedPointer calleeType;
					if( node.callee != nullptr ) {
						calleeSymbol = node.callee->resolvedSymbol;
						calleeType = node.callee->semanticType;
						if( node.callee->kind == Node::Kind::IdentifierExpression ) {
							calleeName = static_cast<const IdentifierExpression&>( *node.callee ).name;
						}
						else if( node.callee->kind == Node::Kind::MemberAccessExpression ) {
							calleeName = static_cast<const MemberAccessExpression&>( *node.callee ).member;
						}
					}

					this->walkOptional( node.callee );
					this->walkVector( node.typeArguments );
					this->argumentLines.clear();
					this->argumentCols.clear();
					this->recordArguments( node.arguments );
					this->walkVector( node.arguments );
					for( const uranite::ast::nodes::KeywordArgument& argument : node.keywordArguments ) {
						this->walkOptional( argument.value );
					}

					if( calleeName.empty() == false ) {
						this->collectCall( node, calleeName, calleeSymbol, calleeType );
					}
					this->argumentLines.clear();
					this->argumentCols.clear();
				}

				void walkCast( const CastExpression& node ) {
					this->walkOptional( node.expression );
					this->walkOptional( node.targetType );
				}

				void walkComprehension( const ComprehensionExpression& node ) {
					this->walkOptional( node.variableType );
					this->walkOptional( node.iterable );
					this->walkOptional( node.condition );
					this->walkOptional( node.bodyExpression );
					this->walkOptional( node.keyExpression );
				}

				void walkConstruct( const ConstructExpression& node ) {
					this->walkOptional( node.type );
					for( const auto& field : node.fields ) {
						this->walkOptional( field.second );
					}
				}

				void walkIdentifier( const IdentifierExpression& node ) {
					this->collectIdentifier( node, node.name );
					this->walkVector( node.typeArguments );
				}

				void walkIndex( const IndexExpression& node ) {
					this->walkOptional( node.object );
					this->walkOptional( node.index );
				}

				void walkInstanceof( const InstanceofExpression& node ) {
					this->walkOptional( node.object );
					this->walkOptional( node.targetType );
				}

				void walkLambda( const LambdaExpression& node ) {
					this->walkVector( node.parameters );
					this->walkOptional( node.returnType );
					this->walkVector( node.body );
				}

				void walkMapLiteral( const MapLiteralExpression& node ) {
					for( const auto& entry : node.entries ) {
						this->walkOptional( entry.first );
						this->walkOptional( entry.second );
					}
				}

				void walkMatchArm( const MatchArmNode& node ) {
					this->walkOptional( node.pattern );
					this->walkOptional( node.guard );
					this->walkVector( node.body );
				}

				void walkMatchExpression( const MatchExpression& node ) {
					this->walkOptional( node.subject );
					this->walkVector( node.patterns );
					this->walkVector( node.values );
					this->walkOptional( node.defaultValue );
				}

				void walkMemberAccess( const MemberAccessExpression& node ) {
					this->collectIdentifier( node, node.member );
					this->walkOptional( node.object );
				}

				void walkMethodCall( const MethodCallExpression& node ) {
					this->collectIdentifier( node, node.method );
					this->walkOptional( node.object );
					this->walkVector( node.typeArguments );
					this->argumentLines.clear();
					this->argumentCols.clear();
					this->recordArguments( node.arguments );
					this->walkVector( node.arguments );
					for( const uranite::ast::nodes::KeywordArgument& argument : node.keywordArguments ) {
						this->walkOptional( argument.value );
					}
					this->collectCall( node, node.method );
					this->argumentLines.clear();
					this->argumentCols.clear();
				}

				void walkRange( const RangeExpression& node ) {
					this->walkOptional( node.start );
					this->walkOptional( node.end );
				}

				void walkSetLiteral( const SetLiteralExpression& node ) {
					this->walkVector( node.elements );
				}

				void walkSubclassof( const SubclassofExpression& node ) {
					this->walkOptional( node.sourceType );
					this->walkOptional( node.targetType );
				}

				void walkTuple( const TupleExpression& node ) {
					this->walkVector( node.elements );
				}

				void walkUnary( const UnaryExpression& node ) {
					this->walkOptional( node.operand );
				}

				void walkYield( const YieldExpression& node ) {
					this->walkOptional( node.value );
				}

				void walkTypeReference( const TypeReferenceExpression& node ) {
					this->collectIdentifier( node, node.typeName );
				}

				void walkSelf( const SelfExpression& node ) {
					this->collectIdentifier( node, "self" );
				}

				void walkParent( const ParentExpression& node ) {
					this->collectIdentifier( node, "parent" );
				}

				void walkArrayType( const ArrayTypeNode& node ) {
					this->walkOptional( node.elementType );
					this->walkOptional( node.size );
				}

				void walkCallableType( const CallableTypeNode& node ) {
					this->walkVector( node.parameterTypes );
					this->walkOptional( node.returnType );
				}

				void walkFunctionType( const FunctionTypeNode& node ) {
					this->walkVector( node.parameterTypes );
					this->walkOptional( node.returnType );
				}

				void walkGenericType( const GenericTypeNode& node ) {
					this->collectTypeName( node, node.name );
					this->walkVector( node.typeArguments );
				}

				void walkMetaType( const MetaTypeNode& node ) {
					this->walkOptional( node.innerType );
				}

				void walkOptionalType( const OptionalTypeNode& node ) {
					this->walkOptional( node.innerType );
				}

				void walkPointerType( const PointerTypeNode& node ) {
					this->walkOptional( node.innerType );
				}

				void walkReferenceType( const ReferenceTypeNode& node ) {
					this->walkOptional( node.innerType );
				}

				void walkSimpleType( const SimpleTypeNode& node ) {
					this->collectTypeName( node, node.name );
				}

				void walkTupleType( const TupleTypeNode& node ) {
					this->walkVector( node.elements );
				}

				void walkUnionType( const UnionTypeNode& node ) {
					this->walkVector( node.types );
				}

				void walkEnumVariant( const EnumVariantNode& node ) {
					this->walkVector( node.associatedTypes );
					this->walkOptional( node.discriminant );
					this->walkOptional( node.backedValue );
					this->walkVector( node.fields );
					this->walkVector( node.methods );
				}

				void walkField( const FieldDeclarationNode& node ) {
					this->walkOptional( node.type );
					this->walkOptional( node.defaultValue );
				}

				void walkFunctionParameter( const FunctionParameterNode& node ) {
					this->walkOptional( node.type );
					this->walkOptional( node.defaultValue );
				}

				void walkGenericParameter( const GenericParameterNode& node ) {
					this->walkVector( node.constraints );
					this->walkOptional( node.defaultType );
				}
		};

	} // namespace

	SymbolIndex::SymbolIndex() = default;

	void SymbolIndex::addDefinition( const IndexedSymbol& symbol ) {
		const std::size_t position = this->defs.size();
		this->defs.push_back( symbol );
		this->definitionsByName[ symbol.qualifiedName ].push_back( position );
	}

	void SymbolIndex::addReference( const IndexedSymbol& symbol ) {
		const std::size_t position = this->refs.size();
		this->refs.push_back( symbol );
		this->referencesByName[ symbol.qualifiedName ].push_back( position );
	}

	void SymbolIndex::addCall( const IndexedCall& call ) {
		this->callSites.push_back( call );
	}

	void SymbolIndex::index( const Program& program,
	                         const std::unordered_map<std::string, std::vector<SymbolSharedPointer>>& accumulatedSymbols,
	                         const std::unordered_map<std::string, TypeSharedPointer>& accumulatedTypes,
	                         const ScopeSharedPointer& globalScope ) {
		this->defs.clear();
		this->refs.clear();
		this->callSites.clear();
		this->definitionsByName.clear();
		this->referencesByName.clear();

		std::set<const Symbol*> seen;

		if( globalScope != nullptr ) {
			const std::string container = containerFromScope( globalScope );
			for( const auto& entry : globalScope->symbols() ) {
				for( const SymbolSharedPointer& symbol : entry.second ) {
					if( symbol == nullptr ) continue;
					IndexedSymbol definition;
					definition.name = symbol->name;
					definition.qualifiedName = makeQualified( symbol->sourceModule, container, symbol->name );
					definition.kind = symbol->kind;
					definition.symbol = symbol;
					definition.type = symbol->typeref;
					definition.container = container;
					definition.isDefinition = true;
					toLsp( symbol->source, definition.line, definition.col, definition.endLine, definition.endCol,
						definition.pathname, symbol->name.size() );
					this->addDefinition( definition );
					seen.insert( symbol.get() );
				}
			}
		}

		for( const auto& entry : accumulatedSymbols ) {
			for( const SymbolSharedPointer& symbol : entry.second ) {
				if( symbol == nullptr || seen.find( symbol.get() ) != seen.end() ) continue;
				IndexedSymbol definition;
				definition.name = symbol->name;
				definition.qualifiedName = makeQualified( symbol->sourceModule.empty() ? entry.first : symbol->sourceModule, "", symbol->name );
				definition.kind = symbol->kind;
				definition.symbol = symbol;
				definition.type = symbol->typeref;
				definition.container = symbol->sourceModule.empty() ? entry.first : symbol->sourceModule;
				definition.isDefinition = true;
				toLsp( symbol->source, definition.line, definition.col, definition.endLine, definition.endCol,
					definition.pathname, symbol->name.size() );
				this->addDefinition( definition );
				seen.insert( symbol.get() );
			}
		}

		std::unordered_map<std::string, SymbolSharedPointer> typeSymbols;
		if( globalScope != nullptr ) {
			for( const auto& entry : globalScope->symbols() ) {
				for( const SymbolSharedPointer& symbol : entry.second ) {
					if( symbol == nullptr || symbol->kind != Symbol::Kind::Type ) continue;
					typeSymbols[ symbol->name ] = symbol;
				}
			}
		}
		for( const auto& entry : accumulatedSymbols ) {
			for( const SymbolSharedPointer& symbol : entry.second ) {
				if( symbol == nullptr || symbol->kind != Symbol::Kind::Type ) continue;
				if( typeSymbols.find( symbol->name ) == typeSymbols.end() ) typeSymbols[ symbol->name ] = symbol;
			}
		}

		NodeWalker walker( *this, seen, accumulatedTypes, typeSymbols );
		walker.walk( program );
	}

	const IndexedSymbol* SymbolIndex::symbolAt( uint32_t line, uint32_t col ) const {
		const IndexedSymbol* best = nullptr;
		uint64_t bestSpan = 0;
		for( const IndexedSymbol& symbol : this->defs ) {
			if( contains( line, col, symbol.line, symbol.col, symbol.endLine, symbol.endCol ) == false ) continue;
			const uint64_t current = span( symbol.line, symbol.col, symbol.endLine, symbol.endCol );
			if( best == nullptr || current < bestSpan ) {
				best = &symbol;
				bestSpan = current;
			}
		}
		for( const IndexedSymbol& symbol : this->refs ) {
			if( contains( line, col, symbol.line, symbol.col, symbol.endLine, symbol.endCol ) == false ) continue;
			const uint64_t current = span( symbol.line, symbol.col, symbol.endLine, symbol.endCol );
			if( best == nullptr || current < bestSpan ) {
				best = &symbol;
				bestSpan = current;
			}
		}
		return best;
	}

	const IndexedCall* SymbolIndex::callAt( uint32_t line, uint32_t col ) const {
		const IndexedCall* best = nullptr;
		uint64_t bestSpan = 0;
		for( const IndexedCall& call : this->callSites ) {
			if( contains( line, col, call.line, call.col, call.endLine, call.endCol ) == false ) continue;
			const uint64_t current = span( call.line, call.col, call.endLine, call.endCol );
			if( best == nullptr || current < bestSpan ) {
				best = &call;
				bestSpan = current;
			}
		}
		return best;
	}

	std::vector<const IndexedSymbol*> SymbolIndex::definitionsFor( const std::string& qualifiedName ) const {
		std::vector<const IndexedSymbol*> result;
		const auto found = this->definitionsByName.find( qualifiedName );
		if( found == this->definitionsByName.end() ) return result;
		for( std::size_t position : found->second ) {
			if( position < this->defs.size() ) result.push_back( &this->defs[ position ] );
		}
		return result;
	}

	std::vector<const IndexedSymbol*> SymbolIndex::referencesFor( const std::string& qualifiedName ) const {
		std::vector<const IndexedSymbol*> result;
		const auto found = this->referencesByName.find( qualifiedName );
		if( found == this->referencesByName.end() ) return result;
		for( std::size_t position : found->second ) {
			if( position < this->refs.size() ) result.push_back( &this->refs[ position ] );
		}
		return result;
	}

	std::vector<const IndexedSymbol*> SymbolIndex::allDefinitions() const {
		std::vector<const IndexedSymbol*> result;
		result.reserve( this->defs.size() );
		for( const IndexedSymbol& symbol : this->defs ) result.push_back( &symbol );
		return result;
	}

	std::vector<const IndexedSymbol*> SymbolIndex::allSymbols() const {
		std::vector<const IndexedSymbol*> result = this->allDefinitions();
		result.reserve( this->defs.size() + this->refs.size() );
		for( const IndexedSymbol& symbol : this->refs ) result.push_back( &symbol );
		return result;
	}

} // namespace uranite::lsp::semantic
