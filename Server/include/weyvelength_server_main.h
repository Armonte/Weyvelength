#pragma once

// The server executable's main, reusable: an application that plugs a
// ServerExtension in keeps every flag, the ice-file rotation and the logging
// setup of the stock server, and adds its own flags through extra_arg.

#include <functional>
#include <string>

namespace Weyvelength {
	struct ServerExtension;

	struct ServerMainHooks {
		const char* extra_usage = nullptr; // appended to --help
		// An option the stock parser does not know: 1 = consumed, 0 = not ours
		// (unknown option), -1 = ours but the value is bad. next() returns the
		// following argument, or null at the end.
		std::function<int(const std::string& arg, const std::function<const char*()>& next)> extra_arg;
		// Called once after the flags are parsed and logging is set up, before
		// Init; null aborts the start. The caller owns the extension.
		std::function<ServerExtension*()> make_extension;
		std::function<void()> after_run; // after Run() returns (the io thread is done)
	};

	int RunServerMain(int argc, char* argv[], const ServerMainHooks& hooks = {});
}
