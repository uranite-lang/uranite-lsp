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

#ifndef _URANITE_LSP_WORKSPACE_ANALYSIS_SCHEDULER_HPP_
#define _URANITE_LSP_WORKSPACE_ANALYSIS_SCHEDULER_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace uranite::lsp::workspace {

	/** @brief Debounces per-document analysis and runs only the newest generation. */
	class AnalysisScheduler {
		public:
			using Task = std::function<void(uint64_t)>;
			using Duration = std::chrono::milliseconds;

			AnalysisScheduler();
			~AnalysisScheduler();

			AnalysisScheduler( const AnalysisScheduler& ) = delete;
			AnalysisScheduler& operator=( const AnalysisScheduler& ) = delete;

			/** @brief Replaces any pending task for the URI and returns its generation. */
			uint64_t schedule( const std::string& uri, Task task, Duration debounce = Duration( 150 ) );

			/** @brief Invalidates pending and in-flight work for the URI. */
			void cancel( const std::string& uri );

			uint64_t currentGeneration( const std::string& uri ) const;

			/** @brief Drains pending work; used by tests. Returns once the queue is empty. */
			void flush();

			/** @brief Cancels pending work, stops the worker and joins it. */
			void stop();

		private:
			struct PendingTask {
				std::string uri;
				uint64_t generation = 0;
				std::chrono::steady_clock::time_point due;
				Task task;
			};

			mutable std::mutex mutex;
			std::condition_variable condition;
			std::map<std::string, PendingTask> pending;
			std::map<std::string, std::shared_ptr<std::atomic<uint64_t>>> generations;
			std::thread worker;
			bool stopping = false;
			uint32_t activeCount = 0;

			void run();
			std::shared_ptr<std::atomic<uint64_t>> generationFor( const std::string& uri ) const;
	};

} // namespace uranite::lsp::workspace

#endif // end _URANITE_LSP_WORKSPACE_ANALYSIS_SCHEDULER_HPP_
