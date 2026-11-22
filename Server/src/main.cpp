#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "weyvelength_server.h"

using namespace Weyvelength;

static void PrintUsage()
{
	std::cout <<
		"usage: weyvelength_server [options]\n"
		"  --port PORT                  default 5555\n"
		"  --room-code-length N         default 8\n"
		"  --room-list-cooldown-ms MS   default 1000\n"
		"  --stun-host HOST             default stun.l.google.com; empty disables it\n"
		"  --stun-port PORT             default 19302\n"
		"  --turn HOST:PORT:USER:PASS   repeatable\n"
		"  --builds-file PATH           the build gate: one line per gated game, `game build [build...]`;\n"
		"                               reread when the file changes (every 5 s); a game not listed is open\n"
		"  --ice-file PATH              read stun/turn from a file and reread it as it expires,\n"
		"                               replacing --stun-host, --stun-port and --turn\n";
}

// TURN servers arrive as one "host:port:user:pass" argument per --turn flag.
static bool ParseTurnServer(const std::string& arg, Proto::TurnServer& out)
{
	size_t a = arg.find(':');
	size_t b = arg.find(':', a == std::string::npos ? a : a + 1);
	size_t c = arg.find(':', b == std::string::npos ? b : b + 1);
	if (a == std::string::npos || b == std::string::npos || c == std::string::npos)
		return false;

	out.host = arg.substr(0, a);
	out.port = (uint16_t)std::strtoul(arg.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
	out.username = arg.substr(b + 1, c - b - 1);
	out.password = arg.substr(c + 1);
	return true;
}

// The whole ice config, as an external process writes it:
//
//   expires 1755500000
//   stun stun.l.google.com 19302
//   turn turn.example.net 3478 1755500000:alice aGVsbG8gd29ybGQ=
//
// Space separated, not colon separated like --turn: a coturn REST username is
// "<expiry>:<name>". The expiry never reaches a client, it only says when to
// reread; absent means poll.
struct IceFile {
	Proto::IceServers ice;
	int64_t expires = 0; // unix seconds; 0 = no expiry
};

// Splits on runs of whitespace; fewer fields than asked for is a parse failure.
static bool SplitFields(const std::string& line, size_t count, std::vector<std::string>& out)
{
	out.clear();
	for (size_t at = 0; out.size() < count;) {
		size_t start = line.find_first_not_of(" \t", at);
		if (start == std::string::npos)
			return false;

		size_t end = line.find_first_of(" \t", start);
		out.push_back(line.substr(start, end == std::string::npos ? end : end - start));
		if (end == std::string::npos)
			break;
		at = end;
	}
	return out.size() == count;
}

// Any malformed line fails the whole read, leaving the current set in place.
static bool ParseIceFile(std::istream& in, IceFile& out)
{
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back(); // written on windows, read anywhere

		size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos || line[start] == '#')
			continue;

		size_t split = line.find_first_of(" \t", start);
		if (split == std::string::npos)
			return false;

		std::string keyword = line.substr(start, split - start);
		std::string value = line.substr(split + 1);

		std::vector<std::string> fields;
		if (keyword == "expires") {
			if (!SplitFields(value, 1, fields))
				return false;
			out.expires = std::strtoll(fields[0].c_str(), nullptr, 10);
		} else if (keyword == "stun") {
			if (!SplitFields(value, 2, fields))
				return false;
			out.ice.stun_host = fields[0];
			out.ice.stun_port = (uint16_t)std::strtoul(fields[1].c_str(), nullptr, 10);
		} else if (keyword == "turn") {
			if (!SplitFields(value, 4, fields))
				return false;
			out.ice.turn.push_back({ fields[0], (uint16_t)std::strtoul(fields[1].c_str(), nullptr, 10), fields[2], fields[3] });
		} else {
			return false;
		}
	}
	return true;
}

static bool ReadIceFile(const std::string& path, IceFile& out)
{
	std::ifstream file{ path };
	if (!file)
		return false;

	IceFile parsed;
	if (!ParseIceFile(file, parsed))
		return false;

	// A file truncated on a line boundary parses cleanly and describes nothing,
	// which would otherwise be broadcast over working credentials.
	if (parsed.ice.stun_host.empty() && parsed.ice.turn.empty())
		return false;

	out = std::move(parsed);
	return true;
}

constexpr auto ice_margin = std::chrono::seconds(300); // reread this long before the credentials expire
constexpr auto ice_retry = std::chrono::seconds(30); // expiry has not advanced yet, or the read failed
constexpr auto ice_poll = std::chrono::seconds(300); // no expiry line: nothing to count down to
constexpr auto ice_max_wait = std::chrono::seconds(3600); // an expiry we cannot believe, e.g. written in milliseconds

static std::chrono::seconds NextIceRead(const IceFile& file)
{
	if (file.expires == 0)
		return ice_poll;

	auto left = std::chrono::seconds(file.expires - (int64_t)std::time(nullptr)) - ice_margin;
	if (left < ice_retry)
		return ice_retry; // already inside the margin: the minter has not run yet
	return left < ice_max_wait ? left : ice_max_wait; // never sleep so long that rotation quietly dies
}

// Rereads the file as its credentials age. A failed read is never fatal and
// never blanks working credentials.
static void RefreshIce(std::stop_token stop, Server& server, std::string path)
{
	std::mutex mutex;
	std::condition_variable_any idle;

	while (!stop.stop_requested()) {
		IceFile file;
		std::chrono::seconds wait = ice_retry;

		if (ReadIceFile(path, file)) {
			server.SetIceServers(file.ice); // sends nothing if nothing moved
			wait = NextIceRead(file);
		} else {
			spdlog::warn("Could not read {}, keeping the current ice servers", path);
		}

		// Interruptible, so shutdown does not wait out the whole interval.
		std::unique_lock lock{ mutex };
		idle.wait_for(lock, stop, wait, [] { return false; });
	}
}

// --builds-file: `game build [build...]` per line, `#` comments. Rereads follow the file's mtime.
static bool ReadBuildsFile(const std::string& path, std::map<std::string, std::set<std::string>>& out)
{
	std::ifstream in(path);
	if (!in)
		return false;
	std::map<std::string, std::set<std::string>> builds;
	std::string line;
	while (std::getline(in, line)) {
		if (auto hash = line.find('#'); hash != std::string::npos)
			line.erase(hash);
		std::istringstream fields(line);
		std::string game;
		if (!(fields >> game))
			continue;
		auto& set = builds[game];
		for (std::string build; fields >> build;)
			set.insert(build);
	}
	out = std::move(builds);
	return true;
}

static void RefreshBuilds(std::stop_token stop, Server& server, std::string path)
{
	std::mutex mutex;
	std::condition_variable_any idle;
	std::filesystem::file_time_type seen{};
	while (!stop.stop_requested()) {
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(path, ec);
		if (!ec && stamp != seen) {
			std::map<std::string, std::set<std::string>> builds;
			if (ReadBuildsFile(path, builds)) {
				seen = stamp;
				server.SetAllowedBuilds(std::move(builds));
			} else {
				spdlog::warn("Could not read {}, keeping the current build gate", path);
			}
		}
		std::unique_lock lock{ mutex };
		idle.wait_for(lock, stop, std::chrono::seconds(5), [] { return false; });
	}
}

static std::string g_builds_path; // --builds-file

static bool ParseArgs(int argc, char* argv[], ServerConfig& config, std::string& ice_path)
{
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };

		if (arg == "--help" || arg == "-h") {
			PrintUsage();
			std::exit(0);
		} else if (arg == "--port") {
			config.port = (uint16_t)std::strtoul(next(), nullptr, 10);
		} else if (arg == "--room-code-length") {
			config.room_code_length = (uint32_t)std::strtoul(next(), nullptr, 10);
		} else if (arg == "--room-list-cooldown-ms") {
			config.room_list_cooldown_ms = (uint32_t)std::strtoul(next(), nullptr, 10);
		} else if (arg == "--stun-host") {
			config.ice.stun_host = next();
		} else if (arg == "--stun-port") {
			config.ice.stun_port = (uint16_t)std::strtoul(next(), nullptr, 10);
		} else if (arg == "--builds-file") {
			g_builds_path = next();
		} else if (arg == "--ice-file") {
			ice_path = next();
		} else if (arg == "--turn") {
			Proto::TurnServer turn;
			if (!ParseTurnServer(next(), turn)) {
				std::cout << "bad --turn value, expected HOST:PORT:USER:PASS\n";
				return false;
			}
			config.ice.turn.push_back(std::move(turn));
		} else {
			std::cout << "unknown option: " << arg << "\n";
			return false;
		}
	}
	return true;
}

int main(int argc, char* argv[])
{
	ServerConfig config{ .port = 5555, .ice = { .stun_host = "stun.l.google.com", .stun_port = 19302 } };
	std::string ice_path;
	if (!ParseArgs(argc, argv, config, ice_path)) {
		PrintUsage();
		return 1;
	}

	// A bad file at startup is worth failing on; later rereads fail quietly.
	IceFile ice;
	if (!ice_path.empty()) {
		if (!ReadIceFile(ice_path, ice)) {
			std::cout << "could not read --ice-file " << ice_path << "\n";
			return 1;
		}
		config.ice = ice.ice;
	}

	// The library logs through the default logger; the app decides what that
	// is. Here: async so logging never blocks the io thread, flushed per
	// message so redirected output stays live.
	spdlog::init_thread_pool(8192, 1);
	spdlog::set_default_logger(spdlog::create_async<spdlog::sinks::stdout_color_sink_mt>("weyvelength"));
	spdlog::set_level(spdlog::level::debug); // dev server: show the p2p signal traffic
	spdlog::flush_on(spdlog::level::debug);

	Server server;
	if (!server.Init(config)) {
		spdlog::error("Server failed to bind port {}", config.port);
		return 1;
	}

	// Joined on the way out: RefreshIce holds a reference to the server.
	std::jthread refresh;
	if (!ice_path.empty())
		refresh = std::jthread{ RefreshIce, std::ref(server), ice_path };

	// The build gate: a bad file at startup fails loudly, later rereads keep the previous gate.
	std::jthread builds_refresh;
	if (!g_builds_path.empty()) {
		std::map<std::string, std::set<std::string>> builds;
		if (!ReadBuildsFile(g_builds_path, builds)) {
			std::cout << "could not read --builds-file " << g_builds_path << "\n";
			return 1;
		}
		server.SetAllowedBuilds(std::move(builds));
		builds_refresh = std::jthread{ RefreshBuilds, std::ref(server), g_builds_path };
	}

	server.Run();   // blocks, driving the accept/ping/read coroutines
	return 0;
}
