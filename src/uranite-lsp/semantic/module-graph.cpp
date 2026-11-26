
//
// @author hxAri (hxari)
// @create 2026-11-23 00:00
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
#include <map>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "uranite-lsp/cst/language.hpp"
#include "uranite-lsp/cst/parse-session.hpp"
#include "uranite-lsp/cst/query-set.hpp"
#include "uranite-lsp/cst/symbol-provider.hpp"
#include "uranite-lsp/semantic/module-graph.hpp"

namespace uranite::lsp::semantic {

	namespace {

		std::string trim( const std::string& value ) {
			const std::size_t first = value.find_first_not_of( " \t\r\n" );
			if( first == std::string::npos ) return "";
			const std::size_t last = value.find_last_not_of( " \t\r\n" );
			return value.substr( first, last - first + 1 );
		}

		/**
		 * @brief Locates the first occurrence of a name that is not on a comment
		 * line, so doc extraction anchors at the declaration, not the header.
		 */
		std::size_t definitionNamePos( const std::string& source, const std::string& name ) {
			std::size_t pos = 0;
			while( ( pos = source.find( name, pos ) ) != std::string::npos ) {
				std::size_t lineStart = pos;
				while( lineStart > 0 && source[ lineStart - 1 ] != '\n' ) lineStart--;
				std::size_t lineEnd = source.find( '\n', lineStart );
				if( lineEnd == std::string::npos ) lineEnd = source.size();
				const std::string t = trim( source.substr( lineStart, lineEnd - lineStart ) );
				if( t.rfind( "#", 0 ) != 0 ) return pos;
				pos += name.size();
			}
			return std::string::npos;
		}

		/**
		 * @brief Reads a file's own package declaration; it is the authoritative
		 * canonical module name (root-relative aliases differ per root).
		 */
		std::string packageDeclarationOf( const std::string& filePath ) {
			std::ifstream file( filePath );
			if( file.is_open() == false ) return "";
			std::string line;
			while( std::getline( file, line ) ) {
				const std::string t = trim( line );
				if( t.rfind( "package ", 0 ) == 0 ) return trim( t.substr( 8 ) );
			}
			return "";
		}

		/**
		 * @brief Process-wide cache of per-file export scans. The stdlib tree is
		 * immutable during a session, so completion requests must not re-parse
		 * every module for each keystroke. Entries are validated by file size.
		 */
		struct ExportInfoCacheEntry {
			std::uintmax_t fileSize = 0;
			std::vector<ModuleGraph::ExportInfo> infos;
		};

		std::mutex exportInfoCacheMutex;
		std::map<std::string, ExportInfoCacheEntry> exportInfoCache;

		/** @brief One process-wide compiled query set reused by every export scan. */
		cst::QuerySet& sharedQuerySet() {
			static cst::QuerySet queries;
			static std::once_flag once;
			std::call_once( once, [] { queries.load(); } );
			return queries;
		}

		bool exportInfoCacheLookup( const std::string& key, const std::string& filePath,
		                           std::vector<ModuleGraph::ExportInfo>& output ) {
			std::error_code error;
			const std::uintmax_t fileSize = std::filesystem::file_size( filePath, error );
			if( error ) return false;
			std::lock_guard<std::mutex> lock( exportInfoCacheMutex );
			const auto found = exportInfoCache.find( key );
			if( found == exportInfoCache.end() ) return false;
			if( found->second.fileSize != fileSize ) return false;
			output = found->second.infos;
			return true;
		}

		void exportInfoCacheStore( const std::string& key, const std::string& filePath,
		                           std::vector<ModuleGraph::ExportInfo>&& infos ) {
			std::error_code error;
			const std::uintmax_t fileSize = std::filesystem::file_size( filePath, error );
			if( error ) return;
			std::lock_guard<std::mutex> lock( exportInfoCacheMutex );
			exportInfoCache[ key ] = { fileSize, std::move( infos ) };
		}

		bool isDirectory( const std::string& path ) {
			std::error_code error;
			return std::filesystem::is_directory( path, error );
		}

		std::string resolvePath( const std::string& base, const std::string& value ) {
			if( value.empty() ) return "";
			std::filesystem::path candidate( value );
			if( candidate.is_absolute() ) return candidate.string();
			return ( std::filesystem::path( base ) / candidate ).string();
		}

		std::vector<std::string> parseInlineArray( const std::string& value ) {
			std::vector<std::string> result;
			std::string content = trim( value );
			if( content.size() < 2 || content.front() != '[' || content.back() != ']' ) return result;
			content = content.substr( 1, content.size() - 2 );
			std::string current;
			bool inQuotes = false;
			for( char character : content ) {
				if( character == '"' || character == '\'' ) {
					inQuotes = !inQuotes;
					current.push_back( character );
				}
				else if( character == ',' && inQuotes == false ) {
					std::string item = trim( current );
					if( item.size() >= 2 && ( ( item.front() == '"' && item.back() == '"' ) ||
					                           ( item.front() == '\'' && item.back() == '\'' ) ) ) {
						item = item.substr( 1, item.size() - 2 );
					}
					if( item.empty() == false ) result.push_back( item );
					current.clear();
				}
				else {
					current.push_back( character );
				}
			}
			std::string last = trim( current );
			if( last.empty() == false ) {
				if( last.size() >= 2 && ( ( last.front() == '"' && last.back() == '"' ) ||
				                          ( last.front() == '\'' && last.back() == '\'' ) ) ) {
					last = last.substr( 1, last.size() - 2 );
				}
				result.push_back( last );
			}
			return result;
		}

	} // namespace

	ModuleGraph::ModuleGraph() = default;

	void ModuleGraph::setWorkspaceRoot( const std::string& rootPath ) {
		this->workspaceRoot_ = rootPath;
		this->refresh();
	}

	void ModuleGraph::setModulesPathOverride( const std::string& path ) {
		this->modulesPathOverride_ = path;
		this->refresh();
	}

	void ModuleGraph::discoverProjectRoot() {
		this->projectRoot_.clear();
		this->manifestPath_.clear();
		if( this->workspaceRoot_.empty() ) return;
		std::filesystem::path current( this->workspaceRoot_ );
		for( int depth = 0; depth < 64 && current.has_parent_path(); depth++ ) {
			std::filesystem::path candidate = current / "uranite.yaml";
			std::error_code error;
			if( std::filesystem::exists( candidate, error ) ) {
				this->projectRoot_ = current.string();
				this->manifestPath_ = candidate.string();
				return;
			}
			if( current == current.parent_path() ) break;
			current = current.parent_path();
		}
	}

	void ModuleGraph::loadManifestAndLockfile() {
		this->requirementsModulesPath_.clear();
		this->buildModulesPath_.clear();
		this->includePaths_.clear();
		this->entrySourcePath_.clear();
		this->dependencies_.clear();
		if( this->manifestPath_.empty() ) return;

		std::ifstream file( this->manifestPath_ );
		if( file.is_open() == false ) return;
		std::string line;
		std::string section;
		while( std::getline( file, line ) ) {
			std::string trimmed = trim( line );
			if( trimmed.empty() || trimmed[0] == '#' ) continue;
			if( trimmed == "requirements:" ) { section = "requirements"; continue; }
			if( trimmed == "build:" ) { section = "build"; continue; }
			if( trimmed == "package:" ) { section = "package"; continue; }
			if( trimmed.rfind( "requirements.", 0 ) == 0 ) { section = "requirements"; continue; }
			if( trimmed.rfind( "build.", 0 ) == 0 ) { section = "build"; continue; }
			if( trimmed.rfind( "package.", 0 ) == 0 ) { section = "package"; continue; }

			std::size_t colon = trimmed.find( ':' );
			if( colon == std::string::npos ) continue;
			std::string key = trim( trimmed.substr( 0, colon ) );
			std::string value = trim( trimmed.substr( colon + 1 ) );
			if( value.size() >= 2 && value.front() == '"' && value.back() == '"' ) {
				value = value.substr( 1, value.size() - 2 );
			}
			else if( value.size() >= 2 && value.front() == '\'' && value.back() == '\'' ) {
				value = value.substr( 1, value.size() - 2 );
			}

			if( section == "package" ) {
				if( key == "entry" ) {
					std::string resolved = resolvePath( this->projectRoot_, value );
					if( resolved.empty() == false ) this->entrySourcePath_ = resolved;
				}
			}
			else if( section == "requirements" ) {
				if( key == "path" ) {
					std::string resolved = resolvePath( this->projectRoot_, value );
					if( resolved.empty() == false ) this->requirementsModulesPath_.push_back( resolved );
				}
				else if( key == "modules-path" ) {
					std::string resolved = resolvePath( this->projectRoot_, value );
					if( resolved.empty() == false ) this->requirementsModulesPath_.push_back( resolved );
				}
				else if( key == "dependencies" || key == "dev-dependencies" ) {
					std::vector<std::string> deps = parseInlineArray( value );
					for( const std::string& dep : deps ) {
						// dep like "name@version" or "owner/repo@version"
						std::string name = dep;
						std::string version;
						std::size_t at = dep.find( '@' );
						if( at != std::string::npos ) {
							name = dep.substr( 0, at );
							version = dep.substr( at + 1 );
						}
						std::size_t slash = name.find( '/' );
						if( slash != std::string::npos ) name = name.substr( slash + 1 );
						if( name.empty() ) continue;
						ModuleDependency dependency;
						dependency.packageName = name;
						dependency.version = version;
						this->dependencies_.push_back( dependency );
					}
				}
			}
			else if( section == "build" ) {
				if( key == "modules-path" || key == "modules_path" ) {
					std::string resolved = resolvePath( this->projectRoot_, value );
					if( resolved.empty() == false ) this->buildModulesPath_.push_back( resolved );
				}
				else if( key == "include-paths" ) {
					for( const std::string& item : parseInlineArray( value ) ) {
						std::string resolved = resolvePath( this->projectRoot_, item );
						if( resolved.empty() == false ) this->includePaths_.push_back( resolved );
					}
				}
			}
		}

		// Lockfile: resolve cached dependency module paths.
		std::filesystem::path lockPath = std::filesystem::path( this->projectRoot_ ) / "uranite.lock";
		std::ifstream lockFile( lockPath );
		if( lockFile.is_open() ) {
			std::string lockLine;
			std::string currentPackage;
			while( std::getline( lockFile, lockLine ) ) {
				std::string trimmed = trim( lockLine );
				if( trimmed.rfind( "package.", 0 ) == 0 ) {
					std::string name = trimmed.substr( 8 );
					if( name.size() > 0 && name.back() == ':' ) name.pop_back();
					currentPackage = name;
					continue;
				}
				if( currentPackage.empty() ) continue;
				if( trimmed.rfind( "version:", 0 ) == 0 ) {
					for( ModuleDependency& dep : this->dependencies_ ) {
						if( dep.packageName == currentPackage ) {
							dep.version = trim( trimmed.substr( 8 ) );
							if( dep.version.size() >= 2 && dep.version.front() == '"' && dep.version.back() == '"' ) {
								dep.version = dep.version.substr( 1, dep.version.size() - 2 );
							}
						}
					}
				}
			}
		}

		// Resolve dependency cache paths.
		const char* home = std::getenv( "HOME" );
		std::string cacheRoot = home != nullptr ? std::string( home ) + "/.uranite/cache" : "/tmp/.uranite/cache";
		for( ModuleDependency& dep : this->dependencies_ ) {
			if( dep.version.empty() ) continue;
			std::string packageDir = cacheRoot + "/" + dep.packageName + "-" + dep.version;
			std::string modulesPath = packageDir + "/modules";
			if( isDirectory( modulesPath ) ) {
				this->buildModulesPath_.push_back( modulesPath );
			}
			else if( isDirectory( packageDir ) ) {
				this->buildModulesPath_.push_back( packageDir );
			}
		}
	}

	void ModuleGraph::refresh() {
		this->discoverProjectRoot();
		this->loadManifestAndLockfile();
	}

	std::string ModuleGraph::resolveStdlibRoot() const {
		const char* env = std::getenv( "URANITE_MODULES_PATH" );
		if( env != nullptr ) {
			std::string path( env );
			if( path.empty() == false && isDirectory( path ) ) return path;
		}
		if( isDirectory( "/usr/local/lib/uranite/stdlibs" ) ) return "/usr/local/lib/uranite/stdlibs";
		if( isDirectory( "/usr/local/share/uranite/stdlibs" ) ) return "/usr/local/share/uranite/stdlibs";
		if( this->workspaceRoot_.empty() == false ) {
			const std::filesystem::path workspace( this->workspaceRoot_ );
			const std::filesystem::path sibling = ( workspace / "../uranite/stdlibs" ).lexically_normal();
			if( isDirectory( sibling.string() ) ) return sibling.string();
			const std::filesystem::path local = workspace / "stdlibs";
			if( isDirectory( local.string() ) ) return local.string();
		}
		if( isDirectory( "../uranite/stdlibs" ) ) return "../uranite/stdlibs";
		if( isDirectory( "./stdlibs" ) ) return "./stdlibs";
		return "";
	}

	std::vector<std::string> ModuleGraph::projectModuleRoots() const {
		std::vector<std::string> roots;
		auto add = [ &roots ]( const std::string& path ) {
			if( path.empty() ) return;
			std::string resolved = path;
			std::error_code error;
			if( std::filesystem::is_regular_file( resolved, error ) ) {
				resolved = std::filesystem::path( resolved ).parent_path().string();
			}
			if( resolved.empty() || isDirectory( resolved ) == false ) return;
			if( std::find( roots.begin(), roots.end(), resolved ) == roots.end() ) roots.push_back( resolved );
		};
		add( this->entrySourcePath_ );
		for( const std::string& path : this->requirementsModulesPath_ ) add( path );
		for( const std::string& path : this->buildModulesPath_ ) add( path );
		for( const std::string& path : this->includePaths_ ) add( path );
		return roots;
	}

	std::string ModuleGraph::stdlibRoot() const {
		if( this->modulesPathOverride_.empty() == false && isDirectory( this->modulesPathOverride_ ) ) {
			return this->modulesPathOverride_;
		}
		return this->resolveStdlibRoot();
	}

	std::vector<std::string> ModuleGraph::allModuleRoots() const {
		std::vector<std::string> roots = this->projectModuleRoots();
		std::string stdlib = this->stdlibRoot();
		if( stdlib.empty() == false && std::find( roots.begin(), roots.end(), stdlib ) == roots.end() ) {
			roots.push_back( stdlib );
		}
		return roots;
	}

	std::string ModuleGraph::findModule( const std::vector<std::string>& modulePath ) const {
		if( modulePath.empty() ) return "";
		std::vector<std::vector<std::string>> candidateSets;
		candidateSets.push_back( modulePath );
		if( modulePath.size() > 1 && modulePath[ 0 ] == "uranite" ) {
			std::vector<std::string> stripped( modulePath.begin() + 1, modulePath.end() );
			candidateSets.push_back( stripped );
		}
		auto tryVariants = []( const std::filesystem::path& base, const std::vector<std::string>& segments ) -> std::string {
			std::string rel;
			for( std::size_t i = 0; i < segments.size(); i++ ) {
				if( i > 0 ) rel += "/";
				rel += segments[ i ];
			}
			std::filesystem::path candidate = base / ( rel + ".urn" );
			std::error_code error;
			if( std::filesystem::exists( candidate, error ) ) return candidate.string();
			if( segments.empty() == false ) {
				std::string last = segments.back();
				if( last.empty() == false ) last[ 0 ] = static_cast<char>( std::toupper( static_cast<unsigned char>( last[ 0 ] ) ) );
				candidate = base / rel / ( last + ".urn" );
				if( std::filesystem::exists( candidate, error ) ) return candidate.string();
				candidate = base / rel / "__mod__.urn";
				if( std::filesystem::exists( candidate, error ) ) return candidate.string();
			}
			return "";
		};
		for( const std::vector<std::string>& segments : candidateSets ) {
			for( const std::string& root : this->allModuleRoots() ) {
				// __mod__.urn package-prefix stripping parity with Driver.
				std::vector<std::string> effective = segments;
				std::ifstream modFile( std::filesystem::path( root ) / "__mod__.urn" );
				if( modFile.is_open() ) {
					std::string line;
					while( std::getline( modFile, line ) ) {
						std::size_t pos = line.find( "package " );
						if( pos != std::string::npos ) {
							std::string pkg = trim( line.substr( pos + 8 ) );
							std::size_t dot = pkg.find( '.' );
							std::string rootPkg = ( dot != std::string::npos ) ? pkg.substr( 0, dot ) : pkg;
							if( effective.empty() == false && effective[ 0 ] == rootPkg ) {
								std::vector<std::string> stripped( effective.begin() + 1, effective.end() );
								if( stripped.empty() == false ) effective = stripped;
							}
							break;
						}
					}
				}
				std::string found = tryVariants( root, effective );
				if( found.empty() == false ) return found;
			}
		}
		return "";
	}

	std::vector<std::string> ModuleGraph::listModules( const std::string& rootPath ) const {
		std::vector<std::string> result;
		std::error_code error;
		if( rootPath.empty() || std::filesystem::is_directory( rootPath, error ) == false ) return result;
		for( const auto& entry : std::filesystem::directory_iterator( rootPath, error ) ) {
			if( entry.is_directory( error ) ) {
				result.push_back( entry.path().filename().string() );
			}
			else if( entry.path().extension() == ".urn" ) {
				std::string stem = entry.path().stem().string();
				if( stem != "__mod__" ) result.push_back( stem );
			}
		}
		std::sort( result.begin(), result.end() );
		result.erase( std::unique( result.begin(), result.end() ), result.end() );
		return result;
	}



	namespace {

		std::string dottedModuleForPath( const std::string& root, const std::filesystem::path& filePath ) {
			std::filesystem::path relative = std::filesystem::relative( filePath, root );
			std::string result;
			for( const auto& part : relative ) {
				if( result.empty() == false ) result += ".";
				result += part.string();
			}
			if( result.size() > 4 && result.compare( result.size() - 4, 4, ".urn" ) == 0 ) {
				result = result.substr( 0, result.size() - 4 );
			}
			std::string modSuffix = ".__mod__";
			if( result.size() > modSuffix.size() && result.compare( result.size() - modSuffix.size(), modSuffix.size(), modSuffix ) == 0 ) {
				result = result.substr( 0, result.size() - modSuffix.size() );
			}
			else if( result == "__mod__" ) {
				result.clear();
			}
			return result;
		}

		std::string extractDocComment( const std::string& source, uint32_t startByte ) {
			if( startByte > source.size() ) startByte = static_cast<uint32_t>( source.size() );
			std::size_t i = startByte;
			// walk backwards over whitespace
			while( i > 0 && ( source[ i - 1 ] == ' ' || source[ i - 1 ] == '\t' || source[ i - 1 ] == '\n' || source[ i - 1 ] == '\r' ) ) i--;
			if( i == 0 ) return "";

			// G13: Uranite docstrings follow the declaration header inside the body.
			{
				std::size_t headerEnd = source.find( '\n', startByte );
				if( headerEnd == std::string::npos ) headerEnd = source.size();
				std::size_t cursor = headerEnd;
				while( cursor <= source.size() ) {
					std::size_t lineEnd = source.find( '\n', cursor );
					if( lineEnd == std::string::npos ) lineEnd = source.size();
					const std::string line = trim( source.substr( cursor, lineEnd - cursor ) );
					if( line.empty() == false ) {
						if( line.rfind( "\"\"\"", 0 ) == 0 ) {
							const std::string head = trim( line.substr( 3 ) );
							const std::size_t closed = head.find( "\"\"\"" );
							if( closed != std::string::npos ) return trim( head.substr( 0, closed ) );
							std::string result = head;
							std::size_t scan = lineEnd + 1;
							while( scan <= source.size() ) {
								std::size_t end = source.find( '\n', scan );
								if( end == std::string::npos ) end = source.size();
								const std::string next = source.substr( scan, end - scan );
								const std::size_t stop = next.find( "\"\"\"" );
								if( stop != std::string::npos ) {
									result += "\n" + trim( next.substr( 0, stop ) );
									return trim( result );
								}
								result += "\n" + trim( next );
								scan = end + 1;
							}
							return trim( result );
						}
						break;
					}
					if( lineEnd >= source.size() ) break;
					cursor = lineEnd + 1;
				}
			}

			// G13: `#` line comments directly above the declaration.
			{
				std::vector<std::pair<std::size_t, std::size_t>> lines;
				std::size_t start = 0;
				for( std::size_t p = 0; p <= source.size(); p++ ) {
					if( p == source.size() || source[ p ] == '\n' ) {
						lines.push_back( { start, p } );
						start = p + 1;
					}
				}
				std::size_t lineIndex = lines.size() > 0 ? lines.size() - 1 : 0;
				for( std::size_t l = 0; l < lines.size(); l++ ) {
					if( i >= lines[ l ].first && i <= lines[ l ].second ) { lineIndex = l; break; }
				}
				std::vector<std::string> collected;
				for( std::size_t l = lineIndex; l > 0; l-- ) {
					const std::string text = trim( source.substr( lines[ l - 1 ].first, lines[ l - 1 ].second - lines[ l - 1 ].first ) );
					if( text.rfind( "#", 0 ) != 0 ) break;
					std::string content = text.substr( 1 );
					if( content.empty() == false && content[ 0 ] == ' ' ) content = content.substr( 1 );
					collected.push_back( content );
				}
				if( collected.empty() == false ) {
					std::reverse( collected.begin(), collected.end() );
					std::string result;
					for( const std::string& line : collected ) {
						if( result.empty() == false ) result += "\n";
						result += line;
					}
					return trim( result );
				}
			}

			// G13: `#{ ... }#` block comments directly above the declaration.
			if( i >= 2 && source[ i - 1 ] == '#' && source[ i - 2 ] == '}' ) {
				const std::size_t begin = source.rfind( "#{", i - 2 );
				if( begin != std::string::npos ) {
					return trim( source.substr( begin + 2, i - 2 - ( begin + 2 ) ) );
				}
			}
			return "";
		}

		std::string extractSignature( const std::string& source, uint32_t startByte ) {
			if( startByte >= source.size() ) return "";
			std::size_t end = startByte;
			while( end < source.size() && source[ end ] != '\n' && source[ end ] != '{' ) end++;
			std::string sig = trim( source.substr( startByte, end - startByte ) );
			if( sig.size() > 120 ) sig = sig.substr( 0, 120 ) + "...";
			return sig;
		}

		int kindForName( const std::string& capture, const std::string& name ) {
			if( capture == "definition.function" ) return 3;
			if( capture == "definition.class" ) {
				if( name.empty() == false && name.find( "Error" ) != std::string::npos ) return 7;
				return 7;
			}
			if( capture == "definition.interface" ) return 8;
			if( capture == "definition.type" || capture == "definition.struct" ) return 22;
			if( capture == "definition.enum" ) return 13;
			if( capture == "definition.variable" ) return 6;
			if( capture == "definition.constant" ) return 21;
			if( capture == "definition.trait" ) return 8;
			return 6;
		}

	} // namespace

	std::vector<ModuleGraph::ExportInfo> ModuleGraph::exportedSymbolInfos( const std::string& moduleDottedPath ) const {
		std::vector<ExportInfo> result;
		std::vector<std::string> segs;
		std::string cur;
		for( char ch : moduleDottedPath ) {
			if( ch == '.' ) { if( cur.empty() == false ) { segs.push_back( cur ); cur.clear(); } }
			else cur.push_back( ch );
		}
		if( cur.empty() == false ) segs.push_back( cur );
		std::string filePath = this->findModule( segs );
		if( filePath.empty() ) return result;

		// Immutable-file cache: avoids re-parsing the whole stdlib per completion.
		const std::string cacheKey = filePath + "|" + moduleDottedPath;
		if( exportInfoCacheLookup( cacheKey, filePath, result ) ) return result;

		std::ifstream file( filePath );
		if( file.is_open() == false ) return result;
		std::ostringstream buffer;
		buffer << file.rdbuf();
		std::string source = buffer.str();

		// __mod__.urn uses `export { ... }` blocks.
		if( filePath.size() >= 11 && filePath.compare( filePath.size() - 11, 11, "__mod__.urn" ) == 0 ) {
			std::vector<std::string> names;
			std::size_t pos = source.find( "export" );
			while( pos != std::string::npos ) {
				std::size_t brace = source.find( '{', pos );
				if( brace == std::string::npos ) break;
				std::size_t end = source.find( '}', brace );
				if( end == std::string::npos ) break;
				std::string block = source.substr( brace + 1, end - brace - 1 );
				std::string current;
				for( char ch : block ) {
					if( ch == ',' || ch == '\n' ) {
						std::string name = trim( current );
						if( name.empty() == false ) names.push_back( name );
						current.clear();
					}
					else current.push_back( ch );
				}
				std::string last = trim( current );
				if( last.empty() == false ) names.push_back( last );
				pos = source.find( "export", end );
			}
			// Try to resolve each name in sibling files for kind/detail/doc.
			std::filesystem::path modDir = std::filesystem::path( filePath ).parent_path();
			std::map<std::string, ExportInfo> infoByName;
			std::error_code error;
			for( auto it = std::filesystem::directory_iterator( modDir, error ); it != std::filesystem::directory_iterator(); it.increment( error ) ) {
				if( error ) break;
				if( it->is_regular_file() == false ) continue;
				if( it->path().extension() != ".urn" ) continue;
				std::ifstream sf( it->path().string() );
				if( sf.is_open() == false ) continue;
				std::ostringstream sb; sb << sf.rdbuf();
				std::string ssrc = sb.str();
				cst::ParseSession session;
				session.parse( ssrc );
				cst::QuerySet& queries = sharedQuerySet();
				cst::SymbolProvider provider;
				auto symbols = provider.provide( ssrc, session.rootNode(), queries );
				for( const auto& symbol : symbols ) {
					if( symbol.role != cst::SymbolRole::Definition ) continue;
					// Class fields and locals parse as variable declarations; they
					// are never importable module symbols.
					if( symbol.capture == "definition.variable" ) continue;
					if( infoByName.count( symbol.name ) ) continue;
					ExportInfo info;
					info.name = symbol.name;
					info.module = moduleDottedPath;
					info.kind = kindForName( symbol.capture, symbol.name );
					// Find the declaration occurrence (skip comment-header hits).
					std::size_t namePos = definitionNamePos( ssrc, symbol.name );
					if( namePos != std::string::npos ) {
						info.documentation = extractDocComment( ssrc, static_cast<uint32_t>( namePos ) );
						// signature: go back to line start
						std::size_t lineStart = namePos;
						while( lineStart > 0 && ssrc[ lineStart - 1 ] != '\n' ) lineStart--;
						info.detail = extractSignature( ssrc, static_cast<uint32_t>( lineStart ) );
					}
					infoByName[ symbol.name ] = info;
				}
			}
			for( const std::string& name : names ) {
				auto it = infoByName.find( name );
				if( it != infoByName.end() ) { result.push_back( it->second ); }
				else {
					ExportInfo info;
					info.name = name;
					info.module = moduleDottedPath;
					info.kind = 6;
					info.detail = name;
					result.push_back( info );
				}
			}
			exportInfoCacheStore( cacheKey, filePath, std::vector<ExportInfo>( result ) );
			return result;
		}

		cst::ParseSession session;
		session.parse( source );
		cst::QuerySet& queries = sharedQuerySet();
		cst::SymbolProvider provider;
		auto symbols = provider.provide( source, session.rootNode(), queries );
		for( const auto& symbol : symbols ) {
			if( symbol.role != cst::SymbolRole::Definition ) continue;
			// Class fields and locals parse as variable declarations; they are
			// never importable module symbols.
			if( symbol.capture == "definition.variable" ) continue;
			ExportInfo info;
			info.name = symbol.name;
			info.module = moduleDottedPath;
			info.kind = kindForName( symbol.capture, symbol.name );
			// approximate location by searching the declaration occurrence
			std::size_t namePos = definitionNamePos( source, symbol.name );
			if( namePos != std::string::npos ) {
				info.documentation = extractDocComment( source, static_cast<uint32_t>( namePos ) );
				std::size_t lineStart = namePos;
				while( lineStart > 0 && source[ lineStart - 1 ] != '\n' ) lineStart--;
				info.detail = extractSignature( source, static_cast<uint32_t>( lineStart ) );
			}
			if( info.detail.empty() ) info.detail = symbol.name;
			result.push_back( info );
		}
		std::sort( result.begin(), result.end(), []( const ExportInfo& a, const ExportInfo& b ) { return a.name < b.name; } );
		result.erase( std::unique( result.begin(), result.end(), []( const ExportInfo& a, const ExportInfo& b ) { return a.name == b.name; } ), result.end() );
		exportInfoCacheStore( cacheKey, filePath, std::vector<ExportInfo>( result ) );
		return result;
	}

	std::vector<std::pair<std::string, int>> ModuleGraph::exportedSymbols( const std::string& moduleDottedPath ) const {
		std::vector<std::pair<std::string, int>> result;
		for( const ExportInfo& info : this->exportedSymbolInfos( moduleDottedPath ) ) {
			result.push_back( { info.name, info.kind } );
		}
		return result;
	}

	std::vector<std::pair<std::string, std::string>> ModuleGraph::allExportedSymbols() const {
		std::vector<std::pair<std::string, std::string>> result;
		// The same symbol is reachable through several roots and through __mod__
		// re-exports. Canonicalize to the file's own package declaration and
		// prefer the shortest dotted module (aggregate re-exports) so
		// auto-imports merge into existing aggregate import lines.
		std::map<std::string, std::string> best;
		for( const std::string& root : this->allModuleRoots() ) {
			std::error_code error;
			for( auto it = std::filesystem::recursive_directory_iterator( root, error ); it != std::filesystem::recursive_directory_iterator(); it.increment( error ) ) {
				if( error ) break;
				if( it->is_regular_file() == false ) continue;
				if( it->path().extension() != ".urn" ) continue;
				std::string modulePath = dottedModuleForPath( root, it->path() );
				if( modulePath.empty() ) continue;
				std::string canonical = packageDeclarationOf( it->path().string() );
				if( canonical.empty() ) canonical = modulePath;
				if( root == this->stdlibRoot() && canonical.rfind( "uranite.", 0 ) != 0 ) {
					canonical = "uranite." + canonical;
				}
				auto exports = this->exportedSymbols( canonical );
				for( const auto& item : exports ) {
					const auto found = best.find( item.first );
					if( found == best.end() || canonical.size() < found->second.size() ) {
						best[ item.first ] = canonical;
					}
				}
			}
		}
		result.reserve( best.size() );
		for( const auto& entry : best ) {
			result.push_back( { entry.first, entry.second } );
		}
		return result;
	}

} // namespace uranite::lsp::semantic
