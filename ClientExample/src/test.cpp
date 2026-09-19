#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "weyvelength.h" // the C API only; this example never touches the C++ client

// Typed lines cross from the blocking console reader to the poll loop here.
static std::mutex input_mutex;
static std::queue<std::string> input_lines;

static void StartInputThread()
{
	std::thread([] {
		std::string line;
		while (std::getline(std::cin, line)) {
			std::lock_guard lock(input_mutex);
			input_lines.push(std::move(line));
		}
		}).detach();
}

static bool NextInputLine(std::string& out)
{
	std::lock_guard lock(input_mutex);
	if (input_lines.empty())
		return false;

	out = std::move(input_lines.front());
	input_lines.pop();
	return true;
}

static void PrintPong(WeyveClient* client, uint64_t timestamp)
{
	uint64_t now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
	uint64_t rtt_ticks = now_ticks - timestamp;

	auto rtt = std::chrono::steady_clock::duration(rtt_ticks);
	auto rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(rtt).count();

	std::cout << "Pong: " << rtt_ms << " ms (i am client " << weyve_id(client) << ")\n";
}

static void PrintRoomError(const WeyveEvent& event)
{
	std::cout << "Room error " << (int)event.data.room_error.code;
	if (event.data.room_error.context_len)
		std::cout << " (" << std::string(event.data.room_error.context, event.data.room_error.context_len) << ")";
	std::cout << "\n";
}

// "/kick 3" style numeric argument; 0 (never a valid id) on garbage.
static uint32_t ParseId(const std::string& arg)
{
	return (uint32_t)std::strtoul(arg.c_str(), nullptr, 10);
}

// Borrowed room id as an owned string; empty means we are not in a room.
static std::string RoomId(WeyveClient* client)
{
	uint32_t len = 0;
	const char* id = weyve_room_id(client, &len);
	return std::string(id, len);
}

static void PrintRoomInfo(WeyveClient* client)
{
	std::cout << "Room " << RoomId(client) << ", host " << weyve_host_id(client) << (weyve_is_host(client) ? " (you)" : "") << "\n";
	std::cout << (weyve_room_joinable(client) ? "Open to join" : "Closed") << (weyve_room_passworded(client) ? ", password required" : "");
	std::cout << (weyve_room_listed(client) ? ", listed" : ", unlisted") << "\n";

	uint32_t count = 0;
	const uint32_t* members = weyve_members(client, &count);

	std::cout << "Members:";
	for (uint32_t i = 0; i < count; i++) {
		uint32_t name_len = 0;
		const char* name = weyve_peer_name(client, members[i], &name_len); // null once they are not a member
		std::cout << " " << members[i] << "(" << (name ? std::string(name, name_len) : std::string()) << ")"; // names collide, the id does not
	}
	std::cout << "\n";

	for (uint32_t i = 0, keys = weyve_room_data_count(client); i < keys; i++) {
		uint32_t key_len = 0, value_len = 0;
		const char* key_bytes = weyve_room_data_key_at(client, i, &key_len); // the call has to run before key_len is read
		std::string key(key_bytes, key_len);
		const char* value = weyve_room_data(client, key.c_str(), &value_len);
		std::cout << "  " << key << " = " << std::string(value, value_len) << "\n";
	}

	for (uint32_t i = 0, keys = weyve_room_listing_count(client); i < keys; i++) {
		uint32_t key_len = 0, value_len = 0;
		const char* key_bytes = weyve_room_listing_key_at(client, i, &key_len);
		std::string key(key_bytes, key_len);
		const char* value = weyve_room_listing(client, key.c_str(), &value_len);
		std::cout << "  listing: " << key << " = " << std::string(value, value_len) << "\n";
	}

	for (uint32_t i = 0; i < count; i++) {
		uint32_t id = members[i];
		for (uint32_t j = 0, keys = weyve_member_data_count(client, id); j < keys; j++) {
			uint32_t key_len = 0, value_len = 0;
			const char* key_bytes = weyve_member_data_key_at(client, id, j, &key_len);
			std::string key(key_bytes, key_len);
			const char* value = weyve_member_data(client, id, key.c_str(), &value_len);
			std::cout << "  client " << id << ": " << key << " = " << std::string(value, value_len) << "\n";
		}
	}
}

static void PrintRoomList(WeyveClient* client)
{
	uint32_t count = weyve_room_list_count(client);
	std::cout << count << " room(s)\n";

	for (uint32_t i = 0; i < count; i++) {
		uint32_t id_len = 0;
		const char* id = weyve_room_list_id(client, i, &id_len);
		std::cout << "  " << std::string(id, id_len);
		std::cout << " (" << weyve_room_list_members(client, i) << " in"
			<< (weyve_room_list_joinable(client, i) ? "" : ", closed")
			<< (weyve_room_list_passworded(client, i) ? ", password" : "") << ")";

		for (uint32_t j = 0, keys = weyve_room_list_listing_count(client, i); j < keys; j++) {
			uint32_t key_len = 0, value_len = 0;
			const char* key_bytes = weyve_room_list_listing_key_at(client, i, j, &key_len);
			std::string key(key_bytes, key_len);
			const char* value = weyve_room_list_listing(client, i, key.c_str(), &value_len);
			std::cout << " " << key << "=" << std::string(value, value_len);
		}
		std::cout << "\n";
	}
}

// "/list", or "/list mode ranked" for an equals filter on a listing slot.
static void SendListCommand(WeyveClient* client, const std::string& args)
{
	if (args.empty()) {
		weyve_list_rooms(client, nullptr, 0);
		return;
	}

	size_t space = args.find(' ');
	std::string key = args.substr(0, space);
	std::string value = space == std::string::npos ? "" : args.substr(space + 1);

	WeyveRoomFilter filter{ key.c_str(), value.empty() ? WEYVE_FILTER_EXISTS : WEYVE_FILTER_EQUALS, value.c_str() };
	weyve_list_rooms(client, &filter, 1);
}

// "/p2p 3 hello"; sends the text bytes to one peer over the mesh.
static void SendP2PCommand(WeyveClient* client, const std::string& args)
{
	size_t space = args.find(' ');
	uint32_t id = ParseId(args.substr(0, space));
	if (space == std::string::npos || id == 0) {
		std::cout << "usage: /p2p ID TEXT\n";
		return;
	}

	std::string text = args.substr(space + 1);
	if (!weyve_send_p2p(client, id, text.data(), (uint32_t)text.size()))
		std::cout << "p2p send to client " << id << " failed\n";
}

// "/links"; what every member's direct link is doing right now.
static void PrintLinksCommand(WeyveClient* client)
{
	uint32_t count = 0;
	const uint32_t* members = weyve_members(client, &count);
	const uint32_t self = weyve_id(client);

	for (uint32_t i = 0; i < count; ++i) {
		if (members[i] == self)
			continue;

		const char* state = "?";
		switch (weyve_peer_link_state(client, members[i])) {
		case WEYVE_LINK_NONE: state = "none, nothing sent yet"; break;
		case WEYVE_LINK_OPENING: state = "opening"; break;
		case WEYVE_LINK_UP: state = "up"; break;
		case WEYVE_LINK_RECOVERING: state = "recovering, sends are queued"; break;
		case WEYVE_LINK_GAVE_UP: state = "gave up"; break;
		}

		std::cout << "client " << members[i] << ": " << state << "\n";
	}
}

// "KEY VALUE" split in two; the value may contain spaces.
static bool SplitKeyValue(const std::string& args, std::string& key, std::string& value)
{
	size_t space = args.find(' ');
	if (space == std::string::npos || space == 0)
		return false;

	key = args.substr(0, space);
	value = args.substr(space + 1);
	return true;
}

// "/set KEY VALUE" or "/setme KEY VALUE".
static void SendSetCommand(WeyveClient* client, const std::string& args, bool own)
{
	std::string key, value;
	if (!SplitKeyValue(args, key, value)) {
		std::cout << "usage: " << (own ? "/setme" : "/set") << " KEY VALUE\n";
		return;
	}

	if (own)
		weyve_set_member_data(client, key.c_str(), value.c_str());
	else
		weyve_set_room_data(client, key.c_str(), value.c_str());
}

// "/pub KEY VALUE"; fills one listing slot, the part of the room browsers see.
static void SendPubCommand(WeyveClient* client, const std::string& args)
{
	std::string key, value;
	if (!SplitKeyValue(args, key, value)) {
		std::cout << "usage: /pub KEY VALUE\n";
		return;
	}

	weyve_set_room_listing(client, key.c_str(), value.c_str());
}

// Ping the server once a second; it replies with a pong.
static int RunPing(WeyveClient* client)
{
	auto last = std::chrono::steady_clock::now();
	while (weyve_poll(client)) {
		WeyveEvent event;
		while (weyve_next(client, &event)) {
			if (event.type == WEYVE_EVENT_HEARTBEAT)
				PrintPong(client, event.data.heartbeat.timestamp);
		}

		auto now = std::chrono::steady_clock::now();
		if (now - last >= std::chrono::seconds(1)) {
			weyve_send_heartbeat(client, (uint64_t)now.time_since_epoch().count());
			last = now;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::cout << "Connection closed\n";
	return 0;
}

// Create a room (empty code) or join one, then send typed lines to everyone in it.
static int RunChat(WeyveClient* client, const std::string& code, const std::string& password, const std::string& name)
{
	if (!name.empty())
		weyve_set_name(client, name.c_str()); // has to happen before we are in a room

	if (code.empty())
		weyve_create_room(client);
	else
		weyve_join_room(client, code.c_str(), password.c_str());

	StartInputThread();

	while (weyve_poll(client)) {
		WeyveEvent event;
		while (weyve_next(client, &event)) {
			switch (event.type) {
			case WEYVE_EVENT_ROOM_ID_ASSIGNED: {
				std::string id(event.data.room_assigned.id, event.data.room_assigned.id_len);
				std::cout << "In room " << id << " (join it: clientexample chat " << id << ")\n";
				std::cout << "Commands: /who, /set KEY VALUE, /del KEY, /setme KEY VALUE, /delme KEY\n";
				std::cout << "          /open, /close, /pass [PASSWORD], /kick ID, /ban ID, /host ID, /leave\n";
				std::cout << "          /list [KEY [VALUE]], /listed, /unlisted, /pub KEY VALUE, /unpub KEY\n";
				std::cout << "          /p2p ID TEXT (direct, over the mesh), /links\n";
				break;
			}
			case WEYVE_EVENT_ROOM_ERROR:
				PrintRoomError(event);
				if (RoomId(client).empty())
					return 1; // create/join failed; nothing to chat in
				break;
			case WEYVE_EVENT_CHAT:
				std::cout << "[client " << event.data.chat.from << "] " << std::string(event.data.chat.text, event.data.chat.text_len) << "\n";
				break;
			case WEYVE_EVENT_PEER_JOINED:
				std::cout << "* client " << event.data.peer_joined.id << " ("
					<< std::string(event.data.peer_joined.name, event.data.peer_joined.name_len) << ") is here\n";
				break;
			case WEYVE_EVENT_PEER_LEFT:
				if (event.data.peer_left.id == weyve_id(client)) {
					std::cout << "Left the room\n";
					return 0;
				}
				std::cout << "* client " << event.data.peer_left.id << " left\n";
				break;
			case WEYVE_EVENT_HOST_CHANGED:
				std::cout << "* client " << event.data.host_changed.id << " is the host" << (event.data.host_changed.id == weyve_id(client) ? " (you)" : "") << "\n";
				break;
			case WEYVE_EVENT_ROOM_DATA_CHANGED: {
				std::string key(event.data.room_data.key, event.data.room_data.key_len);
				if (event.data.room_data.value_len == 0)
					std::cout << "* room data: " << key << " deleted\n";
				else
					std::cout << "* room data: " << key << " = " << std::string(event.data.room_data.value, event.data.room_data.value_len) << "\n";
				break;
			}
			case WEYVE_EVENT_MEMBER_DATA_CHANGED: {
				std::string key(event.data.member_data.key, event.data.member_data.key_len);
				if (event.data.member_data.value_len == 0)
					std::cout << "* client " << event.data.member_data.id << " data: " << key << " deleted\n";
				else
					std::cout << "* client " << event.data.member_data.id << " data: " << key << " = " << std::string(event.data.member_data.value, event.data.member_data.value_len) << "\n";
				break;
			}
			case WEYVE_EVENT_KICKED:
				std::cout << "Kicked from the room\n";
				return 0;
			case WEYVE_EVENT_BANNED:
				std::cout << "Banned from the room\n";
				return 0;
			case WEYVE_EVENT_ROOM_ACCESS_CHANGED:
				std::cout << "* room is now " << (event.data.room_access.open ? "open" : "closed") << (event.data.room_access.passworded ? " (password required)" : "") << "\n";
				break;
			case WEYVE_EVENT_ROOM_LISTED_CHANGED:
				std::cout << "* room is now " << (event.data.room_listed.listed ? "listed" : "unlisted") << "\n";
				break;
			case WEYVE_EVENT_ROOM_LISTING_CHANGED: {
				std::string key(event.data.room_listing.key, event.data.room_listing.key_len);
				if (event.data.room_listing.value_len == 0)
					std::cout << "* listing: " << key << " cleared\n";
				else
					std::cout << "* listing: " << key << " = " << std::string(event.data.room_listing.value, event.data.room_listing.value_len) << "\n";
				break;
			}
			case WEYVE_EVENT_ROOM_LIST:
				PrintRoomList(client);
				break;
			default:
				break;
			}
		}

		uint32_t from = 0, len = 0;
		while (const uint8_t* data = weyve_next_p2p(client, &from, &len)) {
			std::string text{ (const char*)data, len };
			std::cout << "[p2p client " << from << "] " << text << "\n";
		}

		// Lines typed before the room is joined stay queued until it is.
		std::string line;
		while (!RoomId(client).empty() && NextInputLine(line)) {
			if (line.empty())
				continue;
			if (line.rfind("/name ", 0) == 0)
				weyve_set_name(client, line.substr(6).c_str()); // always refused here: we are in a room
			else if (line == "/who")
				PrintRoomInfo(client);
			else if (line == "/leave")
				weyve_leave_room(client);
			else if (line.rfind("/setme ", 0) == 0)
				SendSetCommand(client, line.substr(7), true);
			else if (line.rfind("/delme ", 0) == 0)
				weyve_delete_member_data(client, line.substr(7).c_str());
			else if (line.rfind("/set ", 0) == 0)
				SendSetCommand(client, line.substr(5), false);
			else if (line.rfind("/del ", 0) == 0)
				weyve_delete_room_data(client, line.substr(5).c_str());
			else if (line.rfind("/pub ", 0) == 0)
				SendPubCommand(client, line.substr(5));
			else if (line.rfind("/unpub ", 0) == 0)
				weyve_delete_room_listing(client, line.substr(7).c_str());
			else if (line == "/list")
				SendListCommand(client, "");
			else if (line.rfind("/list ", 0) == 0)
				SendListCommand(client, line.substr(6));
			else if (line == "/listed")
				weyve_set_room_listed(client, true);
			else if (line == "/unlisted")
				weyve_set_room_listed(client, false);
			else if (line == "/open")
				weyve_set_room_joinable(client, true);
			else if (line == "/close")
				weyve_set_room_joinable(client, false);
			else if (line == "/pass")
				weyve_set_room_password(client, "");
			else if (line.rfind("/pass ", 0) == 0)
				weyve_set_room_password(client, line.substr(6).c_str());
			else if (line.rfind("/kick ", 0) == 0)
				weyve_kick_member(client, ParseId(line.substr(6)));
			else if (line.rfind("/ban ", 0) == 0)
				weyve_ban_member(client, ParseId(line.substr(5)));
			else if (line.rfind("/host ", 0) == 0)
				weyve_transfer_host(client, ParseId(line.substr(6)));
			else if (line.rfind("/p2p ", 0) == 0)
				SendP2PCommand(client, line.substr(5));
			else if (line == "/links")
				PrintLinksCommand(client);
			else
				weyve_send_chat(client, line.c_str());
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::cout << "Connection closed\n";
	return 0;
}

int main(int argc, char* argv[])
{
	std::cout << std::unitbuf; // flush every print so redirected output is live

	std::string mode = argc > 1 ? argv[1] : "";
	if (mode != "ping" && mode != "chat") {
		std::cout << "usage: clientexample ping        heartbeat/rtt demo\n";
		std::cout << "       clientexample chat        create a room and chat in it\n";
		std::cout << "       clientexample chat CODE [PASSWORD]   join a room and chat in it\n";
		std::cout << "       clientexample chat CODE PASSWORD NAME   ... under a chosen name\n";
		std::cout << "       (pass \"\" for CODE and PASSWORD to create a room under a name)\n";
		return 1;
	}

	const char* host = "127.0.0.1";
	uint16_t port = 5555;

	WeyveClient* client = weyve_client_create();
	if (!weyve_connect(client, host, port)) {
		std::cout << "Connect to " << host << ":" << port << " failed\n";
		weyve_client_destroy(client);
		return 1;
	}

	std::cout << "Connected to " << host << ":" << port << "\n";

	int result = mode == "ping" ? RunPing(client)
		: RunChat(client, argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");

	weyve_client_destroy(client);
	return result;
}
