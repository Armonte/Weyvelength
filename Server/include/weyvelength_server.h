#pragma once

#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#endif // _WIN32

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio.hpp>

#include "protocol.h"

namespace Weyvelength {

	struct Room;
	struct Server;

	// An application's policy, plugged into the server without forking it: the
	// Extension message channel plus the few decisions an application may need
	// to add to (who sees a listed room, who may join one). Every hook runs on
	// the server's single io thread, so an implementation needs no locking as
	// long as it only touches its own state from hooks or from Server::Post.
	// The defaults change nothing: a server with no extension, or with this
	// base class, behaves exactly as the plain server does.
	struct ServerExtension {
		virtual ~ServerExtension() = default;
		virtual void OnAttach(Server& server) { (void)server; } // from Init, once
		virtual void OnHello(uint32_t id) { (void)id; } // the connection sent the probe; send your first Extension now
		virtual void OnExtension(uint32_t id, const Proto::Extension& msg) { (void)id; (void)msg; }
		virtual void OnDisconnect(uint32_t id) { (void)id; } // after the room was left
		// A refusal code turns the join away exactly like the server's own checks; nullopt lets it through.
		virtual std::optional<Proto::RoomErrorCode> CheckJoin(uint32_t id, const Room& room) { (void)id; (void)room; return std::nullopt; }
		virtual bool CanSeeRoom(uint32_t viewer, const Room& room) { (void)viewer; (void)room; return true; } // ListRooms
		virtual void OnJoined(uint32_t id, const Room& room) { (void)id; (void)room; } // create or join, after the hydrate
		virtual void OnLeft(uint32_t id, const std::string& room) { (void)id; (void)room; } // any exit: leave, kick, ban, drop
		virtual void OnRoomClosed(const std::string& room) { (void)room; }
	};

	struct ServerConfig {
		uint16_t port = 0;
		uint32_t room_code_length = 0; // 0 = use the default (8)
		uint32_t room_list_cooldown_ms = 0; // 0 = use the default (1000)
		Proto::IceServers ice; // stun/turn handed to every client on connect
		ServerExtension* extension = nullptr; // not owned; must outlive the server
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
		bool extension = false; // sent the probe to a server with an extension: may be sent Extension messages

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
	};

	struct Server {
		bool Init(ServerConfig& config);
		void Run();
		void Stop();

		// Swaps the set and pushes it to everyone connected; a no-op swap sends
		// nothing. Safe from any thread. Existing links keep their old config.
		void SetIceServers(const Proto::IceServers& ice);

		// --- for a ServerExtension (io thread only, except Post) ---
		// Queues an Extension for a connection that sent the probe; false (nothing sent) for any other.
		bool SendExtension(uint32_t id, const Proto::Extension& msg);
		const Connection* FindConnection(uint32_t id) const; // null once gone
		const Room* FindRoom(const std::string& id) const; // null when no such room
		const std::unordered_map<std::string, Room>& Rooms() const { return _rooms; }
		size_t ConnectionCount() const { return _connections.size(); }
		uint16_t Port() const; // the bound port (useful after Init with port 0); 0 before Init
		asio::io_context& Context() { return _context; }
		void Post(std::function<void()> fn); // runs fn on the io thread; safe from any thread

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
	};
}
