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

#include "uranite-lsp/workspace/analysis-scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace uranite::lsp::workspace {

	AnalysisScheduler::AnalysisScheduler() : worker( &AnalysisScheduler::run, this ) {
	}

	AnalysisScheduler::~AnalysisScheduler() {
		this->stop();
	}

	void AnalysisScheduler::stop() {
		{
			std::lock_guard<std::mutex> lock( this->mutex );
			if( this->stopping == false ) {
				this->stopping = true;
				this->pending.clear();
			}
		}
		this->condition.notify_all();
		if( this->worker.joinable() ) this->worker.join();
	}

	// Callers hold the scheduler mutex.
	std::shared_ptr<std::atomic<uint64_t>> AnalysisScheduler::generationFor( const std::string& uri ) const {
		const auto found = this->generations.find( uri );
		if( found != this->generations.end() ) return found->second;
		return nullptr;
	}

	uint64_t AnalysisScheduler::schedule( const std::string& uri, Task task, Duration debounce ) {
		std::shared_ptr<std::atomic<uint64_t>> generation;
		{
			std::lock_guard<std::mutex> lock( this->mutex );
			auto found = this->generations.find( uri );
			if( found == this->generations.end() ) {
				generation = std::make_shared<std::atomic<uint64_t>>( 0 );
				this->generations[ uri ] = generation;
			}
			else generation = found->second;
		}

		const uint64_t next = generation->fetch_add( 1 ) + 1;
		{
			std::lock_guard<std::mutex> lock( this->mutex );
			PendingTask pendingTask;
			pendingTask.uri = uri;
			pendingTask.generation = next;
			pendingTask.due = std::chrono::steady_clock::now() + debounce;
			pendingTask.task = std::move( task );
			this->pending[ uri ] = std::move( pendingTask );
		}
		this->condition.notify_all();
		return next;
	}

	void AnalysisScheduler::cancel( const std::string& uri ) {
		std::shared_ptr<std::atomic<uint64_t>> generation;
		{
			std::lock_guard<std::mutex> lock( this->mutex );
			this->pending.erase( uri );
			generation = this->generationFor( uri );
		}
		if( generation != nullptr ) generation->store( generation->load() + 1 );
		this->condition.notify_all();
	}

	uint64_t AnalysisScheduler::currentGeneration( const std::string& uri ) const {
		std::lock_guard<std::mutex> lock( this->mutex );
		const std::shared_ptr<std::atomic<uint64_t>> generation = this->generationFor( uri );
		if( generation == nullptr ) return 0;
		return generation->load();
	}

	void AnalysisScheduler::flush() {
		std::unique_lock<std::mutex> lock( this->mutex );
		this->condition.wait( lock, [ this ]() {
			return this->stopping || ( this->pending.empty() && this->activeCount == 0 );
		} );
	}

	void AnalysisScheduler::run() {
		while( true ) {
			PendingTask selected;
			std::shared_ptr<std::atomic<uint64_t>> generation;
			bool haveTask = false;

			{
				std::unique_lock<std::mutex> lock( this->mutex );
				while( this->stopping == false ) {
					const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
					auto due = this->pending.end();
					for( auto it = this->pending.begin(); it != this->pending.end(); ++it ) {
						if( it->second.due <= now ) {
							due = it;
							break;
						}
					}

					if( due != this->pending.end() ) {
						selected = std::move( due->second );
						this->pending.erase( due );
						generation = this->generationFor( selected.uri );
						this->activeCount++;
						haveTask = true;
						break;
					}

					if( this->pending.empty() ) {
						this->condition.wait( lock, [ this ]() {
							return this->stopping || this->pending.empty() == false;
						} );
					}
					else {
						auto earliest = std::min_element( this->pending.begin(), this->pending.end(),
							[]( const auto& left, const auto& right ) {
								return left.second.due < right.second.due;
							} );
						this->condition.wait_until( lock, earliest->second.due, [ this ]() {
							return this->stopping;
						} );
					}
				}
				if( this->stopping ) break;
			}

			if( haveTask ) {
				const bool current = generation != nullptr &&
					generation->load() == selected.generation;
				if( current && selected.task ) {
					try {
						selected.task( selected.generation );
					}
					catch( ... ) {
						// A cancelled analysis must never terminate the server.
					}
				}
				{
					std::lock_guard<std::mutex> lock( this->mutex );
					if( this->activeCount > 0 ) this->activeCount--;
				}
				this->condition.notify_all();
			}
		}
	}

} // namespace uranite::lsp::workspace
