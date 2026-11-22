#pragma once

#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#endif // _WIN32

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio.hpp>

#include "protocol.h"

namespace Weyvelength {

	struct ServerConfig {
		uint16_t port = 0;
		uint32_t room_code_length = 0; // 0 = use the default (8)
		uint32_t room_list_cooldown_ms = 0; // 0 = use the default (1000)
		Proto::IceServers ice; // stun/turn handed to every client on connect
	};

	struct Connection {
		uint32_t id = 0;
		std::string room; // empty = not in a room
		std::string name = Proto::default_client_name; // never empty; fixed while in a room
		asio::ip::tcp::socket socket;

		std::deque<std::vector<std::byte>> out; // outbound queue; WriteLoop is the sole writer
		asio::steady_timer wake; // cancel() signals "out has work"
		bool closing = false;
		std::chrono::steady_clock::time_point last_list{}; // epoch = never asked
		std::string game, build; // DeclareBuild; empty = never declared

		Connection(uint32_t id, asio::ip::tcp::socket socket)
			: id(id), socket(std::move(socket)), wake(this->socket.get_executor()) {
			// maybe this isnt the smartest thing to do but its ok for now
			wake.expires_at(std::chrono::steady_clock::time_point::max());
		}
	};

	struct Room {
		std::string id;
		uint32_t host = 0; // the creator, until they leave or hand it over
		std::vector<uint32_t> members;
		std::map<std::string, std::string> data; // metadata, mirrored to every member as RoomDataChanged events
		std::map<uint32_t, std::map<std::string, std::string>> member_data; // per-member metadata, dropped when the member leaves
		bool open = true; // joinable right now?
		std::string password; // empty = none; checked on join, never sent to clients
		std::vector<uint32_t> banned_members; // ids barred from joining; checked on join
		bool listed = false;
		std::map<std::string, std::string> listing; // what non-members can read and filter on
		std::string game; // the host's declared game at creation (the build gate on joins to unlisted rooms)
	};

	struct Server {
		bool Init(ServerConfig& config);
		void Run();
		void Stop();

		// Swaps the set and pushes it to everyone connected; a no-op swap sends
		// nothing. Safe from any thread. Existing links keep their old config.
		void SetIceServers(const Proto::IceServers& ice);
		// The build gate: game -> the builds allowed online. A game absent from the map is open to every build.
		// Safe from any thread; applies to the next request of every connection.
		void SetAllowedBuilds(std::map<std::string, std::set<std::string>> builds);

	private:
		void SendTo(uint32_t id, const Proto::ServerMessage& msg);
		void SendToMany(const std::vector<uint32_t>& ids, const Proto::ServerMessage& msg);
		void SendFrame(uint32_t id, std::vector<std::byte> frame);

		void HandleMessage(std::shared_ptr<Connection> conn, const Proto::ServerMessage& msg);
		void HandleCreateRoom(const std::shared_ptr<Connection>& conn);
		void HandleJoinRoom(const std::shared_ptr<Connection>& conn, const Proto::JoinRoom& msg);
		void HandleLeaveRoom(const std::shared_ptr<Connection>& conn);
		void HandleRoomChat(const std::shared_ptr<Connection>& conn, const Proto::RoomChat& msg);
		void HandleP2PSignal(const std::shared_ptr<Connection>& conn, const Proto::P2PSignal& msg);
		void HandleSetName(const std::shared_ptr<Connection>& conn, const Proto::SetName& msg);
		void HandleDeclareBuild(const std::shared_ptr<Connection>& conn, const Proto::DeclareBuild& msg);
		bool BuildAllowed(const Connection& conn, const std::string& game) const; // false = the game is gated and this connection's declared build is not on its list
		void HandleSetRoomData(const std::shared_ptr<Connection>& conn, const Proto::SetRoomData& msg);
		void HandleSetMemberData(const std::shared_ptr<Connection>& conn, const Proto::SetMemberData& msg);
		void HandleKickMember(const std::shared_ptr<Connection>& conn, const Proto::KickMember& msg);
		void HandleBanMember(const std::shared_ptr<Connection>& conn, const Proto::BanMember& msg);
		void HandleTransferHost(const std::shared_ptr<Connection>& conn, const Proto::TransferHost& msg);
		void HandleSetRoomJoinable(const std::shared_ptr<Connection>& conn, const Proto::SetRoomJoinable& msg);
		void HandleSetRoomPassword(const std::shared_ptr<Connection>& conn, const Proto::SetRoomPassword& msg);
		void HandleSetRoomListed(const std::shared_ptr<Connection>& conn, const Proto::SetRoomListed& msg);
		void HandleSetRoomListing(const std::shared_ptr<Connection>& conn, const Proto::SetRoomListing& msg);
		void HandleListRooms(const std::shared_ptr<Connection>& conn, const Proto::ListRooms& msg);

		void LeaveRoom(const std::shared_ptr<Connection>& conn);
		const std::string& MemberName(uint32_t id) const; // the default if the connection is gone
		Room* HostRoom(const std::shared_ptr<Connection>& conn); // the sender's room if they host it, else null after sending the error

		uint32_t AllocateId(); // 0 when the slot space is exhausted
		void ReleaseId(uint32_t id);

		asio::awaitable<void> AcceptLoop();
		asio::awaitable<void> Session(std::shared_ptr<Connection> conn);
		asio::awaitable<void> ReadLoop(std::shared_ptr<Connection> conn);
		asio::awaitable<void> WriteLoop(std::shared_ptr<Connection> conn);

		asio::io_context _context;
		asio::ip::tcp::acceptor _acceptor{ _context };

		std::unordered_map<uint32_t, std::shared_ptr<Connection>> _connections;
		std::unordered_map<std::string, Room> _rooms;

		std::deque<uint32_t> _free_ids; // released ids, generation already bumped; FIFO so reuse is delayed
		uint32_t _next_slot = 1;   // slots never yet handed out; 0 reserved as "none"

		ServerConfig _config;
		std::map<std::string, std::set<std::string>> _builds; // the build gate (SetAllowedBuilds)
	};
}
