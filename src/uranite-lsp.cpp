
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

#include <cstdio>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "uranite-lsp/server/server.hpp"

int main( int argumentCount, char* argumentVector[] ) {
	std::shared_ptr<spdlog::sinks::basic_file_sink_mt> fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>( "/tmp/uranite-lsp.log", true );
	std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
	std::shared_ptr<spdlog::logger> logger = std::make_shared<spdlog::logger>( "uranite-lsp", spdlog::sinks_init_list{ fileSink, stderrSink } );
	spdlog::set_default_logger( logger );
	spdlog::set_level( spdlog::level::info );
	spdlog::flush_every( std::chrono::seconds( 1 ) );
	spdlog::flush_on( spdlog::level::info );
	uranite::lsp::Server lspServer( stdin, stdout );
	lspServer.run();
	return 0;
}
