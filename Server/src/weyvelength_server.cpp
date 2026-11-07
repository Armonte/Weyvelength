#include "weyvelength_server.h"

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <asio.hpp>
#include <thirdparty/zpp_bits/zpp_bits.h>

#include "framing.h"

using asio::use_awaitable;

namespace Weyvelength {

	static void Enqueue(const std::shared_ptr<Connection>& conn, std::vector<std::byte> frame)
	{
		conn->out.push_back(std::move(frame));
		conn->wake.cancel();
	}

	// Slot plus generation, so a recycled slot yields a different id and stale
	// references miss in _connections instead of hitting the new owner.
	constexpr uint32_t id_slot_bits = 20;
	constexpr uint32_t id_max_slot = (1u << id_slot_bits) - 1; // 1M concurrent
	constexpr uint32_t id_generation_step = 1u << id_slot_bits;

	// Control characters go first: a newline in a name would forge log lines.
	static std::string CleanName(std::string name)
	{
		std::erase_if(name, [](unsigned char c) { return c < 0x20 || c == 0x7F; });

		if (name.size() > Proto::max_client_name) {
			name.resize(Proto::max_client_name);
			// Back off a cut inside a utf-8 sequence, so no half code point survives.
			while (!name.empty() && ((unsigned char)name.back() & 0xC0) == 0x80)
				name.pop_back();
			if (!name.empty() && ((unsigned char)name.back() & 0xC0) == 0xC0)
				name.pop_back(); // a lead byte with its continuation bytes cut off
		}

		return name;
	}

	// Passwords stay out of the log; the username says which batch is live.
	static void LogIceServers(const Proto::IceServers& ice)
	{
		if (ice.stun_host.empty())
			spdlog::info("Ice: no stun");
		else
			spdlog::info("Ice: stun {}:{}", ice.stun_host, ice.stun_port);

		for (const Proto::TurnServer& turn : ice.turn) {
			spdlog::info("Ice: turn {}:{} as {}", turn.host, turn.port, turn.username);
		}
	}

	static std::string MakeRoomCode(uint32_t length)
	{
		static std::mt19937 rng{ std::random_device{}() };
		static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O/1/I
		std::uniform_int_distribution<size_t> pick{ 0, sizeof(alphabet) - 2 };

		std::string code(length, '?');
		for (char& c : code)
			c = alphabet[pick(rng)];
		return code;
	}

	bool Server::Init(ServerConfig& config)
	{
		_config = config;
		if (_config.room_code_length == 0)
			_config.room_code_length = 8;
		if (_config.room_list_cooldown_ms == 0)
			_config.room_list_cooldown_ms = 1000;

		asio::error_code ec;

		asio::ip::tcp::endpoint endpoint{ asio::ip::tcp::v4(), config.port };

		_acceptor.open(endpoint.protocol(), ec);
		if (ec) return false;

		_acceptor.set_option(asio::socket_base::reuse_address(true), ec);
		if (ec) return false;

		_acceptor.bind(endpoint, ec);
		if (ec) return false;

		_acceptor.listen(asio::socket_base::max_listen_connections, ec);
		if (ec) return false;

		spdlog::info("Listening on port {}", config.port);
		LogIceServers(_config.ice);
		return true;
	}

	void Server::Run()
	{
		asio::co_spawn(_context, AcceptLoop(), asio::detached);
		_context.run();
	}

	void Server::Stop()
	{
		_context.stop();
	}

	void Server::SetIceServers(const Proto::IceServers& ice)
	{
		asio::post(_context, [this, ice]() {
			if (ice == _config.ice)
				return; // nothing moved, so nobody needs telling

			_config.ice = ice;
			for (const auto& [id, conn] : _connections) {
				SendTo(id, _config.ice);
			}

			spdlog::info("Ice servers swapped, told {} client(s)", _connections.size());
			LogIceServers(_config.ice);
		});
	}

	asio::awaitable<void> Server::AcceptLoop()
	{
		while (true) {
			asio::ip::tcp::socket socket = co_await _acceptor.async_accept(use_awaitable);

			uint32_t id = AllocateId();   // single-threaded io_context: no lock needed
			if (id == 0) {
				spdlog::warn("Refusing connection: no client ids left");
				asio::error_code ec;
				socket.close(ec);
				continue;
			}

			auto conn = std::make_shared<Connection>(id, std::move(socket));
			_connections.emplace(id, conn);

			spdlog::info("Client {} connected", id);
			asio::co_spawn(_context, Session(conn), asio::detached);
		}
	}

	asio::awaitable<void> Server::Session(std::shared_ptr<Connection> conn)
	{
		asio::co_spawn(conn->socket.get_executor(), WriteLoop(conn), asio::detached);

		SendTo(conn->id, Proto::AssignClientId{ conn->id, conn->name });
		SendTo(conn->id, _config.ice); // p2p infrastructure; empty fields = none

		try {
			co_await ReadLoop(conn);
		}
		catch (...) {
		}

		conn->closing = true;
		conn->wake.cancel();

		asio::error_code ec;

		conn->socket.close(ec);
		LeaveRoom(conn);
		_connections.erase(conn->id);
		ReleaseId(conn->id);

		spdlog::info("Client {} disconnected", conn->id);
	}

	asio::awaitable<void> Server::ReadLoop(std::shared_ptr<Connection> conn)
	{
		std::vector<std::byte> body; // fragments of the message being reassembled
		while (true) {
			std::array<std::byte, Proto::frame_header_size> header;
			co_await asio::async_read(conn->socket, asio::buffer(header), use_awaitable);

			uint32_t len;
			bool more;
			if (!Proto::DecodeFrameHeader(header, len, more))
				co_return;   // protocol error? end the session

			size_t base = body.size();
			if (base + len > Proto::max_reassembled_size)
				co_return;   // reassembled message too large
			body.resize(base + len);
			co_await asio::async_read(conn->socket, asio::buffer(body.data() + base, len), use_awaitable);

			if (more)
				continue;   // wait for the rest of the message

			Proto::ServerMessage msg;
			if (!Proto::DecodeMessage(body, msg))
				co_return;
			body.clear();

			HandleMessage(conn, msg);
		}
	}

	asio::awaitable<void> Server::WriteLoop(std::shared_ptr<Connection> conn)
	{
		try {
			while (!conn->closing) {
				if (conn->out.empty()) {
					asio::error_code ec;
					co_await conn->wake.async_wait(asio::redirect_error(use_awaitable, ec));
					continue;
				}

				std::vector<std::byte> frame = std::move(conn->out.front());
				conn->out.pop_front();
				co_await asio::async_write(conn->socket, asio::buffer(frame), use_awaitable);
			}
		}
		catch (...) {
		}
	}

	void Server::SendFrame(uint32_t id, std::vector<std::byte> frame)
	{
		auto it = _connections.find(id);
		if (it != _connections.end()) {
			Enqueue(it->second, std::move(frame));
		}
	}

	void Server::HandleMessage(std::shared_ptr<Connection> conn, const Proto::ServerMessage& msg)
	{
		if (auto* ping = std::get_if<Proto::Heartbeat>(&msg)) {
			SendTo(conn->id, Proto::Heartbeat{ ping->timestamp });
		}
		else if (std::get_if<Proto::CreateRoom>(&msg)) {
			HandleCreateRoom(conn);
		}
		else if (auto* join = std::get_if<Proto::JoinRoom>(&msg)) {
			HandleJoinRoom(conn, *join);
		}
		else if (std::get_if<Proto::LeaveRoom>(&msg)) {
			HandleLeaveRoom(conn);
		}
		else if (auto* chat = std::get_if<Proto::RoomChat>(&msg)) {
			HandleRoomChat(conn, *chat);
		}
		else if (auto* signal = std::get_if<Proto::P2PSignal>(&msg)) {
			HandleP2PSignal(conn, *signal);
		}
		else if (auto* name = std::get_if<Proto::SetName>(&msg)) {
			HandleSetName(conn, *name);
		}
		else if (auto* set = std::get_if<Proto::SetRoomData>(&msg)) {
			HandleSetRoomData(conn, *set);
		}
		else if (auto* setMember = std::get_if<Proto::SetMemberData>(&msg)) {
			HandleSetMemberData(conn, *setMember);
		}
		else if (auto* kick = std::get_if<Proto::KickMember>(&msg)) {
			HandleKickMember(conn, *kick);
		}
		else if (auto* ban = std::get_if<Proto::BanMember>(&msg)) {
			HandleBanMember(conn, *ban);
		}
		else if (auto* transfer = std::get_if<Proto::TransferHost>(&msg)) {
			HandleTransferHost(conn, *transfer);
		}
		else if (auto* joinable = std::get_if<Proto::SetRoomJoinable>(&msg)) {
			HandleSetRoomJoinable(conn, *joinable);
		}
		else if (auto* password = std::get_if<Proto::SetRoomPassword>(&msg)) {
			HandleSetRoomPassword(conn, *password);
		}
		else if (auto* listed = std::get_if<Proto::SetRoomListed>(&msg)) {
			HandleSetRoomListed(conn, *listed);
		}
		else if (auto* listing = std::get_if<Proto::SetRoomListing>(&msg)) {
			HandleSetRoomListing(conn, *listing);
		}
		else if (auto* list = std::get_if<Proto::ListRooms>(&msg)) {
			HandleListRooms(conn, *list);
		}
	}

	// A member always has a live connection; the default guards against a miss.
	const std::string& Server::MemberName(uint32_t id) const
	{
		static const std::string fallback = Proto::default_client_name;

		auto it = _connections.find(id);
		return it == _connections.end() ? fallback : it->second->name;
	}

	// Shared preamble of every host-only action: resolves the sender's room
	// and checks host status, sending the appropriate error on failure.
	Room* Server::HostRoom(const std::shared_ptr<Connection>& conn)
	{
		auto it = _rooms.find(conn->room);
		if (it == _rooms.end()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NotInRoom, {} });
			return nullptr;
		}

		if (it->second.host != conn->id) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NotHost, {} });
			return nullptr;
		}

		return &it->second;
	}

	void Server::HandleCreateRoom(const std::shared_ptr<Connection>& conn)
	{
		if (!conn->room.empty()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::AlreadyInRoom, conn->room });
			return;
		}

		std::string code = MakeRoomCode(_config.room_code_length);
		while (_rooms.contains(code))
			code = MakeRoomCode(_config.room_code_length);

		_rooms.emplace(code, Room{ code, conn->id, { conn->id } });
		conn->room = code;
		SendTo(conn->id, Proto::AssignRoomId{ code });
		SendTo(conn->id, Proto::HostChanged{ conn->id }); // the host cache has a single source: this event

		spdlog::info("Client {} created room {}", conn->id, code);
	}

	void Server::HandleJoinRoom(const std::shared_ptr<Connection>& conn, const Proto::JoinRoom& msg)
	{
		if (!conn->room.empty()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::AlreadyInRoom, conn->room });
			return;
		}

		auto it = _rooms.find(msg.id);
		if (it == _rooms.end()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NoSuchRoom, msg.id });
			return;
		}

		Room& room = it->second;
		if (std::ranges::find(room.banned_members, conn->id) != room.banned_members.end()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::Banned, msg.id });
			return;
		}

		if (!room.open) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::RoomClosed, msg.id });
			return;
		}

		if (!room.password.empty() && msg.password != room.password) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadPassword, msg.id });
			return;
		}

		SendToMany(room.members, Proto::PeerJoined{ conn->id, conn->name });

		// hydrate the joiner with the same events everyone else already
		// understands: one per existing member, the host, one per data key
		SendTo(conn->id, Proto::AssignRoomId{ msg.id });

		for (uint32_t member : room.members) {
			SendTo(conn->id, Proto::PeerJoined{ member, MemberName(member) });
		}

		SendTo(conn->id, Proto::HostChanged{ room.host });
		SendTo(conn->id, Proto::RoomAccessChanged{ room.open, !room.password.empty() });
		SendTo(conn->id, Proto::RoomListedChanged{ room.listed });

		for (const auto& [key, value] : room.data) {
			SendTo(conn->id, Proto::RoomDataChanged{ key, value });
		}

		for (const auto& [key, value] : room.listing) {
			SendTo(conn->id, Proto::RoomListingChanged{ key, value });
		}

		for (const auto& [member, data] : room.member_data) {
			for (const auto& [key, value] : data) {
				SendTo(conn->id, Proto::MemberDataChanged{ member, key, value });
			}
		}

		room.members.push_back(conn->id);
		conn->room = msg.id;

		spdlog::info("Client {} joined room {}", conn->id, msg.id);
	}

	void Server::HandleLeaveRoom(const std::shared_ptr<Connection>& conn)
	{
		if (conn->room.empty()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NotInRoom, {} });
			return;
		}

		SendTo(conn->id, Proto::PeerLeft{ conn->id }); // your own id = you left
		LeaveRoom(conn);
	}

	void Server::HandleRoomChat(const std::shared_ptr<Connection>& conn, const Proto::RoomChat& msg)
	{
		auto it = _rooms.find(conn->room);
		if (it == _rooms.end()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NotInRoom, {} });
			return;
		}

		// sender included: everyone in the room sees the same stream
		SendToMany(it->second.members, Proto::RoomChat{ conn->id, msg.text });
	}

	static const char* P2PSignalKindName(Proto::P2PSignalKind kind)
	{
		switch (kind) {
		case Proto::P2PSignalKind::Description: return "description";
		case Proto::P2PSignalKind::Candidate: return "candidate";
		case Proto::P2PSignalKind::GatheringDone: return "gathering done";
		}
		return "unknown";
	}

	// Relays ICE signaling between room members without reading the sdp. Bad
	// targets are dropped, not errored: a candidate can race the target's departure.
	void Server::HandleP2PSignal(const std::shared_ptr<Connection>& conn, const Proto::P2PSignal& msg)
	{
		auto it = _rooms.find(conn->room);
		if (it == _rooms.end()) {
			spdlog::debug("c{} p2p {} dropped: no room", conn->id, P2PSignalKindName(msg.kind));
			return;
		}

		if (msg.id == conn->id || std::ranges::find(it->second.members, msg.id) == it->second.members.end()) {
			spdlog::debug("c{} p2p {} dropped: {} not a room-{} member", conn->id, P2PSignalKindName(msg.kind), msg.id, it->second.id);
			return;
		}

		// a size summary at info; the payload (ice creds, local ips) only at debug
		if (msg.kind == Proto::P2PSignalKind::Description)
			spdlog::info("c{} -> c{} p2p description ({} bytes)", conn->id, msg.id, msg.payload.size());
		spdlog::debug("c{} -> c{} p2p {}: {}", conn->id, msg.id, P2PSignalKindName(msg.kind), msg.payload);
		SendTo(msg.id, Proto::P2PSignal{ conn->id, msg.kind, msg.payload }); // forwarded carrying the sender's id
	}

	// Refused while in a room: nothing re-announces a name, so it has to hold
	// still for as long as peers can see it.
	void Server::HandleSetName(const std::shared_ptr<Connection>& conn, const Proto::SetName& msg)
	{
		if (!conn->room.empty()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::AlreadyInRoom, conn->room });
			return;
		}

		std::string name = CleanName(msg.name);
		conn->name = name.empty() ? Proto::default_client_name : std::move(name);
		SendTo(conn->id, Proto::AssignClientId{ conn->id, conn->name }); // the ack is the new pairing

		spdlog::info("Client {} is now {}", conn->id, conn->name);
	}

	void Server::HandleSetRoomData(const std::shared_ptr<Connection>& conn, const Proto::SetRoomData& msg)
	{
		Room* hosted = HostRoom(conn);
		if (!hosted)
			return;

		Room& room = *hosted;
		if (msg.key.empty() || msg.key.size() > Proto::max_room_data_key || msg.value.size() > Proto::max_room_data_value) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, msg.key });
			return;
		}

		if (msg.value.empty()) { // empty value = delete
			if (room.data.erase(msg.key) == 0)
				return; // nothing deleted, nothing to announce
		}
		else {
			auto [entry, inserted] = room.data.try_emplace(msg.key, msg.value);
			if (inserted && room.data.size() > Proto::max_room_data_keys) {
				room.data.erase(entry);
				SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, msg.key });
				return;
			}
			if (!inserted) {
				if (entry->second == msg.value)
					return; // unchanged, nothing to announce
				entry->second = msg.value;
			}
		}

		// the setter applies the write when the broadcast comes back, like
		// everyone else, so all members see changes in the same order
		SendToMany(room.members, Proto::RoomDataChanged{ msg.key, msg.value });
	}

	void Server::HandleSetMemberData(const std::shared_ptr<Connection>& conn, const Proto::SetMemberData& msg)
	{
		auto it = _rooms.find(conn->room);
		if (it == _rooms.end()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NotInRoom, {} });
			return;
		}

		if (msg.key.empty() || msg.key.size() > Proto::max_room_data_key || msg.value.size() > Proto::max_room_data_value) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, msg.key });
			return;
		}

		Room& room = it->second;
		if (msg.value.empty()) { // empty value = delete
			auto member = room.member_data.find(conn->id);
			if (member == room.member_data.end() || member->second.erase(msg.key) == 0)
				return; // nothing deleted, nothing to announce
			if (member->second.empty())
				room.member_data.erase(member);
		}
		else {
			auto& data = room.member_data[conn->id];
			auto [entry, inserted] = data.try_emplace(msg.key, msg.value);
			if (inserted && data.size() > Proto::max_member_data_keys) {
				data.erase(entry);
				SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, msg.key });
				return;
			}
			if (!inserted) {
				if (entry->second == msg.value)
					return; // unchanged, nothing to announce
				entry->second = msg.value;
			}
		}

		SendToMany(room.members, Proto::MemberDataChanged{ conn->id, msg.key, msg.value });
	}

	void Server::HandleKickMember(const std::shared_ptr<Connection>& conn, const Proto::KickMember& msg)
	{
		Room* room = HostRoom(conn);
		if (!room)
			return;

		auto target = _connections.find(msg.id);
		if (msg.id == room->host || target == _connections.end() || target->second->room != room->id) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NoSuchMember, std::to_string(msg.id) });
			return;
		}

		SendTo(msg.id, Proto::KickedByHost{});
		LeaveRoom(target->second); // removal + PeerLeft broadcast, same as any other exit

		spdlog::info("Client {} kicked from room {} by client {}", msg.id, room->id, conn->id);
	}

	void Server::HandleBanMember(const std::shared_ptr<Connection>& conn, const Proto::BanMember& msg)
	{
		Room* room = HostRoom(conn);
		if (!room)
			return;

		auto target = _connections.find(msg.id);
		if (msg.id == room->host || target == _connections.end() || target->second->room != room->id) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NoSuchMember, std::to_string(msg.id) });
			return;
		}

		if (std::ranges::find(room->banned_members, msg.id) == room->banned_members.end())
			room->banned_members.push_back(msg.id); // barred until the room closes; join now rejects them

		SendTo(msg.id, Proto::BannedByHost{});
		LeaveRoom(target->second); // removal + PeerLeft broadcast, same as a kick

		spdlog::info("Client {} banned from room {} by client {}", msg.id, room->id, conn->id);
	}

	void Server::HandleTransferHost(const std::shared_ptr<Connection>& conn, const Proto::TransferHost& msg)
	{
		Room* room = HostRoom(conn);
		if (!room)
			return;

		if (msg.id == conn->id)
			return; // already the host, nothing to announce

		if (std::ranges::find(room->members, msg.id) == room->members.end()) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::NoSuchMember, std::to_string(msg.id) });
			return;
		}

		room->host = msg.id;
		SendToMany(room->members, Proto::HostChanged{ room->host });

		spdlog::info("Client {} now hosts room {} (transferred)", room->host, room->id);
	}

	void Server::HandleSetRoomJoinable(const std::shared_ptr<Connection>& conn, const Proto::SetRoomJoinable& msg)
	{
		Room* room = HostRoom(conn);
		if (!room)
			return;

		if (room->open == msg.open)
			return; // unchanged, nothing to announce

		room->open = msg.open;
		SendToMany(room->members, Proto::RoomAccessChanged{ room->open, !room->password.empty() });

		spdlog::info("Room {} is now {}", room->id, room->open ? "open" : "closed");
	}

	void Server::HandleSetRoomPassword(const std::shared_ptr<Connection>& conn, const Proto::SetRoomPassword& msg)
	{
		Room* room = HostRoom(conn);
		if (!room)
			return;

		if (msg.password.size() > Proto::max_room_password) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadPassword, {} });
			return;
		}

		bool was_passworded = !room->password.empty();
		if (room->password == msg.password)
			return; // unchanged, nothing to announce

		room->password = msg.password;
		if (was_passworded != !room->password.empty()) // members only learn the flag, never the password
			SendToMany(room->members, Proto::RoomAccessChanged{ room->open, !room->password.empty() });
	}

	void Server::HandleSetRoomListed(const std::shared_ptr<Connection>& conn, const Proto::SetRoomListed& msg)
	{
		Room* room = HostRoom(conn);
		if (!room)
			return;

		if (room->listed == msg.listed)
			return; // unchanged, nothing to announce

		room->listed = msg.listed;
		SendToMany(room->members, Proto::RoomListedChanged{ room->listed });

		spdlog::info("Room {} is now {}", room->id, room->listed ? "listed" : "unlisted");
	}

	static bool ValidListingSlot(const std::string& key, const std::string& value)
	{
		return !key.empty() && key.size() <= Proto::max_room_listing_key && value.size() <= Proto::max_room_listing_value;
	}

	// Same shape as room data; slots survive unlisting.
	void Server::HandleSetRoomListing(const std::shared_ptr<Connection>& conn, const Proto::SetRoomListing& msg)
	{
		Room* hosted = HostRoom(conn);
		if (!hosted)
			return;

		Room& room = *hosted;
		if (!ValidListingSlot(msg.key, msg.value)) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, msg.key });
			return;
		}

		if (msg.value.empty()) { // empty value = clear
			if (room.listing.erase(msg.key) == 0)
				return; // nothing cleared, nothing to announce
		}
		else {
			auto [entry, inserted] = room.listing.try_emplace(msg.key, msg.value);
			if (inserted && room.listing.size() > Proto::max_room_listing_keys) {
				room.listing.erase(entry);
				SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, msg.key });
				return;
			}
			if (!inserted) {
				if (entry->second == msg.value)
					return; // unchanged, nothing to announce
				entry->second = msg.value;
			}
		}

		SendToMany(room.members, Proto::RoomListingChanged{ msg.key, msg.value });
	}

	// All a non-member sees; room data and the password stay server-side.
	static Proto::RoomInfo DescribeRoom(const Room& room)
	{
		return { room.id, (uint32_t)room.members.size(), room.open, !room.password.empty(), room.listing };
	}

	// Slots only: a value not in a slot is unsearchable, not just unpublished.
	static bool MatchesFilter(const std::map<std::string, std::string>& listing, const Proto::RoomFilter& filter)
	{
		auto it = listing.find(filter.key);
		const std::string* value = it == listing.end() ? nullptr : &it->second;

		switch (filter.op) {
		case Proto::RoomFilterOp::Exists: return value != nullptr;
		case Proto::RoomFilterOp::Equals: return value && *value == filter.value;
		}

		return false; // an op from a newer client
	}

	static bool ValidListRooms(const Proto::ListRooms& msg)
	{
		if (msg.filters.size() > Proto::max_room_list_filters)
			return false;

		for (const auto& filter : msg.filters) {
			if (!ValidListingSlot(filter.key, filter.value))
				return false;
		}

		return true;
	}

	void Server::HandleListRooms(const std::shared_ptr<Connection>& conn, const Proto::ListRooms& msg)
	{
		// the cooldown comes first so malformed queries cost the same as good ones
		auto now = std::chrono::steady_clock::now();
		if (now - conn->last_list < std::chrono::milliseconds(_config.room_list_cooldown_ms)) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::RateLimited, {} });
			return;
		}
		conn->last_list = now;

		if (!ValidListRooms(msg)) {
			SendTo(conn->id, Proto::RoomError{ Proto::RoomErrorCode::BadRoomData, {} });
			return;
		}

		Proto::RoomList list;

		for (const auto& [code, room] : _rooms) {
			if (!room.listed)
				continue;

			// filter first: a miss never pays for its own copy
			if (!std::ranges::all_of(msg.filters, [&](const Proto::RoomFilter& filter) { return MatchesFilter(room.listing, filter); }))
				continue;

			list.rooms.push_back(DescribeRoom(room));
		}

		spdlog::debug("Client {} listed {} rooms", conn->id, list.rooms.size());
		SendTo(conn->id, list);
	}

	uint32_t Server::AllocateId()
	{
		if (!_free_ids.empty()) {
			uint32_t id = _free_ids.front();
			_free_ids.pop_front();
			return id;
		}

		if (_next_slot > id_max_slot)
			return 0; // every slot is live; the caller turns the connection away
		return _next_slot++; // a fresh slot starts at generation 0
	}

	void Server::ReleaseId(uint32_t id)
	{
		// Wraps to generation 0 after 4096 reuses; nothing that old still holds it.
		_free_ids.push_back(id + id_generation_step);
	}

	void Server::LeaveRoom(const std::shared_ptr<Connection>& conn)
	{
		if (conn->room.empty())
			return;

		auto it = _rooms.find(conn->room);
		if (it != _rooms.end()) {
			Room& room = it->second;
			std::erase(room.members, conn->id);
			room.member_data.erase(conn->id);
			if (room.members.empty()) {
				_rooms.erase(it);
				spdlog::info("Room {} closed", conn->room);
			}
			else {
				SendToMany(room.members, Proto::PeerLeft{ conn->id });
				if (room.host == conn->id) {
					room.host = room.members.front(); // oldest remaining member
					SendToMany(room.members, Proto::HostChanged{ room.host });
					spdlog::info("Client {} now hosts room {}", room.host, conn->room);
				}
			}
		}
		conn->room.clear();
	}

	void Server::SendTo(uint32_t id, const Proto::ServerMessage& msg)
	{
		SendFrame(id, Proto::FrameMessage(msg));
	}

	void Server::SendToMany(const std::vector<uint32_t>& ids, const Proto::ServerMessage& msg)
	{
		std::vector<std::byte> frame = Proto::FrameMessage(msg);   // serialize once

		for (uint32_t id : ids) {
			SendFrame(id, frame);
		}
	}
}
