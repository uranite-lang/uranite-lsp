
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

#ifndef _URANITE_LSP_SEMANTIC_MODULE_GRAPH_HPP_
#define _URANITE_LSP_SEMANTIC_MODULE_GRAPH_HPP_

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace uranite::lsp::semantic {

	/** @brief A resolved dependency's on-disk module root. */
	struct ModuleDependency {
		std::string packageName;
		std::string version;
		std::string modulePath;
	};

	/** @brief Discovers project roots and module search paths mirroring uranite-pkg/compiler rules. */
	class ModuleGraph {
		public:
			ModuleGraph();

			void setWorkspaceRoot( const std::string& rootPath );
			void setModulesPathOverride( const std::string& path );

			const std::string& workspaceRoot() const { return this->workspaceRoot_; }
			const std::string& projectRoot() const { return this->projectRoot_; }
			const std::string& manifestPath() const { return this->manifestPath_; }

			/** @brief Ordered project/package/workspace module roots (excludes stdlib). */
			std::vector<std::string> projectModuleRoots() const;

			/** @brief Resolved stdlib root (env / /usr/local/share / ../uranite/stdlibs). */
			std::string stdlibRoot() const;

			/** @brief All module roots in priority order (project roots then stdlib). */
			std::vector<std::string> allModuleRoots() const;

			/** @brief Discover module file for a dotted module path; returns empty if not found. */
			std::string findModule( const std::vector<std::string>& modulePath ) const;

			struct ExportInfo {
				std::string name;
				std::string module;
				int kind = 3;
				std::string detail;
				std::string documentation;
			};

			/** @brief List immediate module/package names under a root for completion. */
			std::vector<std::string> listModules( const std::string& rootPath ) const;

			/** @brief Exported symbols (name, CompletionItemKind) of a dotted module. */
			std::vector<std::pair<std::string, int>> exportedSymbols( const std::string& moduleDottedPath ) const;

			/** @brief Rich exported symbol metadata of a dotted module. */
			std::vector<ExportInfo> exportedSymbolInfos( const std::string& moduleDottedPath ) const;

			/** @brief All exported symbols as (symbolName, moduleDottedPath) across discoverable modules. */
			std::vector<std::pair<std::string, std::string>> allExportedSymbols() const;

			/** @brief Re-run discovery (manifest/lockfile/cache). */
			void refresh();

		private:
			void discoverProjectRoot();
			void loadManifestAndLockfile();
			std::string resolveStdlibRoot() const;

			std::string workspaceRoot_;
			std::string projectRoot_;
			std::string manifestPath_;
			std::string modulesPathOverride_;

			std::vector<std::string> requirementsModulesPath_;
			std::vector<std::string> buildModulesPath_;
			std::vector<std::string> includePaths_;
			std::string entrySourcePath_;
			std::vector<ModuleDependency> dependencies_;
	};

} // namespace uranite::lsp::semantic

#endif // end _URANITE_LSP_SEMANTIC_MODULE_GRAPH_HPP_
