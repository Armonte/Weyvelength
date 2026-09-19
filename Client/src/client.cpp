#include "client.h"

#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#endif // _WIN32

#include <array>
#include <chrono>
#include <span>
#include <vector>

#include <asio.hpp>
#include <thirdparty/zpp_bits/zpp_bits.h>

#include "framing.h"
#include "p2p_mesh.h"

namespace Weyvelength {
	struct ClientAsioImpl {
		asio::io_context context;
		asio::ip::tcp::socket socket{ context };
		std::vector<std::byte> rx;   // bytes received but not yet consumed
		std::vector<std::byte> rx_msg; // fragments of the message being reassembled
		std::vector<std::byte> tx;   // framed bytes queued to send
	};

	namespace {
		// Monotonic, and never zero, so a stamp that was never set reads as long ago.
		uint64_t SteadyMs()
		{
			using namespace std::chrono;
			static const steady_clock::time_point base = steady_clock::now();
			return (uint64_t)duration_cast<milliseconds>(steady_clock::now() - base).count() + 1;
		}
	}

	Client::Client() :
		_asio(std::make_unique<ClientAsioImpl>()),
		_mesh(std::make_unique<P2PMesh>()) {
	}

	Client::~Client()
	{
		DestroyAllLinks();
	}

	bool Client::Connect(ClientConfig& config)
	{
		auto& impl = *_asio;
		asio::error_code ec;

		asio::ip::tcp::resolver resolver{ impl.context };
		auto endpoints = resolver.resolve(config.host, std::to_string(config.port), ec);
		if (ec) return false;

		asio::connect(impl.socket, endpoints, ec);
		if (ec) return false;

		impl.socket.non_blocking(true, ec);
		return !ec;
	}

	bool Client::Poll()
	{
		if (!_asio->socket.is_open())
			return false;

		_now_ms = SteadyMs(); // one timestamp for the whole poll; every link decision shares it

		if (!DrainServer() || !CarveServer())
			return false;

		PollPeers(); // may queue signal frames; the flush below sends them
		return FlushServer();
	}

	bool Client::Next(Proto::ServerMessage& out)
	{
		if (_inbox.empty())
			return false;

		out = std::move(_inbox.front());
		_inbox.pop();
		return true;
	}

	bool Client::SendServer(const Proto::ServerMessage& msg)
	{
		auto frame = Proto::FrameMessage(msg);
		_asio->tx.insert(_asio->tx.end(), frame.begin(), frame.end());
		return true;
	}

	bool Client::SetName(const std::string& name)
	{
		return SendServer(Proto::SetName{ name });
	}

	bool Client::CreateRoom()
	{
		return SendServer(Proto::CreateRoom{});
	}

	bool Client::JoinRoom(const std::string& id, const std::string& password)
	{
		return SendServer(Proto::JoinRoom{ id, password });
	}

	bool Client::LeaveRoom()
	{
		return SendServer(Proto::LeaveRoom{});
	}

	bool Client::KickMember(uint32_t id)
	{
		return SendServer(Proto::KickMember{ id });
	}

	bool Client::BanMember(uint32_t id)
	{
		return SendServer(Proto::BanMember{ id });
	}

	bool Client::TransferHost(uint32_t id)
	{
		return SendServer(Proto::TransferHost{ id });
	}

	bool Client::SetRoomJoinable(bool open)
	{
		return SendServer(Proto::SetRoomJoinable{ open });
	}

	bool Client::SetRoomPassword(const std::string& password)
	{
		return SendServer(Proto::SetRoomPassword{ password });
	}

	bool Client::SetRoomListed(bool listed)
	{
		return SendServer(Proto::SetRoomListed{ listed });
	}

	bool Client::SetRoomListing(const std::string& key, const std::string& value)
	{
		return SendServer(Proto::SetRoomListing{ key, value });
	}

	bool Client::DeleteRoomListing(const std::string& key)
	{
		return SendServer(Proto::SetRoomListing{ key, {} }); // empty value = clear
	}

	bool Client::ListRooms(const std::vector<Proto::RoomFilter>& filters)
	{
		return SendServer(Proto::ListRooms{ filters });
	}

	bool Client::SendChat(const std::string& text)
	{
		return SendServer(Proto::RoomChat{ 0, text }); // server fills in the sender id
	}

	bool Client::SetRoomData(const std::string& key, const std::string& value)
	{
		return SendServer(Proto::SetRoomData{ key, value });
	}

	bool Client::DeleteRoomData(const std::string& key)
	{
		return SendServer(Proto::SetRoomData{ key, {} }); // empty value = delete
	}

	bool Client::SetMemberData(const std::string& key, const std::string& value)
	{
		return SendServer(Proto::SetMemberData{ key, value });
	}

	bool Client::DeleteMemberData(const std::string& key)
	{
		return SendServer(Proto::SetMemberData{ key, {} }); // empty value = delete
	}

	uint32_t Client::Id() const
	{
		return _id;
	}

	const std::string& Client::Name() const
	{
		return _name;
	}

	const std::string* Client::PeerName(uint32_t id) const
	{
		auto it = _names.find(id);
		return it == _names.end() ? nullptr : &it->second;
	}

	const std::string& Client::RoomId() const
	{
		return _room;
	}

	uint32_t Client::HostId() const
	{
		return _host;
	}

	bool Client::IsHost() const
	{
		return _id != 0 && _id == _host;
	}

	bool Client::RoomJoinable() const
	{
		return _room_open;
	}

	bool Client::RoomPassworded() const
	{
		return _room_passworded;
	}

	bool Client::RoomListed() const
	{
		return _room_listed;
	}

	const std::map<std::string, std::string>& Client::RoomListing() const
	{
		return _listing;
	}

	const std::string* Client::RoomListing(const std::string& key) const
	{
		auto it = _listing.find(key);
		return it == _listing.end() ? nullptr : &it->second;
	}

	const std::vector<Proto::RoomInfo>& Client::RoomList() const
	{
		return _room_list;
	}

	const std::vector<uint32_t>& Client::Members() const
	{
		return _members;
	}

	const std::map<std::string, std::string>& Client::RoomData() const
	{
		return _data;
	}

	const std::string* Client::RoomData(const std::string& key) const
	{
		auto it = _data.find(key);
		return it == _data.end() ? nullptr : &it->second;
	}

	const std::map<std::string, std::string>* Client::MemberData(uint32_t id) const
	{
		auto it = _member_data.find(id);
		return it == _member_data.end() ? nullptr : &it->second;
	}

	const std::string* Client::MemberData(uint32_t id, const std::string& key) const
	{
		const auto* data = MemberData(id);
		if (!data)
			return nullptr;

		auto it = data->find(key);
		return it == data->end() ? nullptr : &it->second;
	}

	bool Client::DrainServer()
	{
		auto& impl = *_asio;

		std::array<std::byte, 4096> chunk;
		asio::error_code ec;

		while (true) {
			size_t chunk_len = impl.socket.read_some(asio::buffer(chunk), ec);
			if (ec) break;
			impl.rx.insert(impl.rx.end(), chunk.begin(), chunk.begin() + chunk_len);
		}

		if (ec != asio::error::would_block && ec != asio::error::try_again) {
			return DisconnectServer();   // eof or hard error
		}

		return true;
	}

	bool Client::CarveServer()
	{
		auto& impl = *_asio;
		constexpr size_t header_size = sizeof(uint32_t);

		std::span<std::byte> remaining{ impl.rx };
		while (remaining.size() >= header_size) {
			uint32_t len;
			bool more;
			if (!Proto::DecodeFrameHeader(remaining.first(header_size), len, more))
				return DisconnectServer();
			if (remaining.size() < header_size + len)
				break;   // full fragment hasn't arrived yet

			if (!Proto::AppendFragment(impl.rx_msg, remaining.subspan(header_size, len)))
				return DisconnectServer(); // reassembled message too large
			remaining = remaining.subspan(header_size + len);

			if (more)
				continue;   // wait for the rest of the message

			Proto::ServerMessage msg;
			bool bad = failure(zpp::bits::in{ impl.rx_msg }(msg));
			impl.rx_msg.clear();
			if (bad)
				return DisconnectServer();

			if (auto* assign = std::get_if<Proto::AssignClientId>(&msg)) {
				_id = assign->id;   // transport metadata; not surfaced via Next()
				_name = assign->name;   // what the server accepted, clamping included
			}
			else if (auto* ice = std::get_if<Proto::IceServers>(&msg)) {
				_ice = std::move(*ice);   // transport metadata; not surfaced via Next()
			}
			else if (auto* signal = std::get_if<Proto::P2PSignal>(&msg)) {
				HandleP2PSignal(*signal);   // ICE plumbing; not surfaced via Next()
			}
			else {
				CacheRoomState(msg); // cached for the accessors, but still surfaced via Next()
				_inbox.push(std::move(msg));
			}
		}

		size_t consumed = impl.rx.size() - remaining.size();
		impl.rx.erase(impl.rx.begin(), impl.rx.begin() + consumed);
		return true;
	}

	bool Client::FlushServer()
	{
		auto& impl = *_asio;
		asio::error_code ec;

		while (!impl.tx.empty()) {
			size_t sent = impl.socket.write_some(asio::buffer(impl.tx), ec);
			if (ec) break;
			impl.tx.erase(impl.tx.begin(), impl.tx.begin() + sent);
		}

		if (ec && ec != asio::error::would_block && ec != asio::error::try_again) {
			return DisconnectServer();   // hard error (empty tx leaves ec unset)
		}

		return true;
	}

	// Mirrors room events into the local caches as frames are carved, so the
	// accessors are already current when Next() hands the event to the app.
	void Client::CacheRoomState(const Proto::ServerMessage& msg)
	{
		if (auto* room = std::get_if<Proto::AssignRoomId>(&msg)) {
			_room = room->id;
			_host = 0;
			_room_open = true;
			_room_passworded = false;
			_room_listed = false;
			_listing.clear();
			_members.assign(1, _id); // events only ever announce the others
			_names.clear();
			_names[_id] = _name;
			_data.clear();
			_member_data.clear();
		}
		else if (auto* joined = std::get_if<Proto::PeerJoined>(&msg)) {
			_members.push_back(joined->id);
			_names[joined->id] = joined->name;
		}
		else if (auto* left = std::get_if<Proto::PeerLeft>(&msg)) {
			if (left->id == _id) {
				ClearRoomState(); // our own id = our LeaveRoom went through
			}
			else {
				std::erase(_members, left->id);
				_names.erase(left->id);
				_member_data.erase(left->id);
				DestroyLink(left->id); // no member, no mesh link
				_mesh->recovery.erase(left->id); // and no grudge if they rejoin
			}
		}
		else if (auto* host = std::get_if<Proto::HostChanged>(&msg)) {
			_host = host->id;
		}
		else if (auto* data = std::get_if<Proto::RoomDataChanged>(&msg)) {
			if (data->value.empty())
				_data.erase(data->key);
			else
				_data[data->key] = data->value;
		}
		else if (auto* member = std::get_if<Proto::MemberDataChanged>(&msg)) {
			if (member->value.empty()) {
				auto it = _member_data.find(member->id);
				if (it != _member_data.end()) {
					it->second.erase(member->key);
					if (it->second.empty())
						_member_data.erase(it);
				}
			}
			else {
				_member_data[member->id][member->key] = member->value;
			}
		}
		else if (std::get_if<Proto::KickedByHost>(&msg)) {
			ClearRoomState(); // removed by the host; same cleanup as leaving
		}
		else if (std::get_if<Proto::BannedByHost>(&msg)) {
			ClearRoomState(); // removed and barred; same cleanup as a kick
		}
		else if (auto* access = std::get_if<Proto::RoomAccessChanged>(&msg)) {
			_room_open = access->open;
			_room_passworded = access->passworded;
		}
		else if (auto* listed = std::get_if<Proto::RoomListedChanged>(&msg)) {
			_room_listed = listed->listed;
		}
		else if (auto* listing = std::get_if<Proto::RoomListingChanged>(&msg)) {
			if (listing->value.empty())
				_listing.erase(listing->key);
			else
				_listing[listing->key] = listing->value;
		}
		else if (auto* list = std::get_if<Proto::RoomList>(&msg)) {
			_room_list = list->rooms; // copied, not moved: the event still carries it to Next()
		}
	}

	void Client::ClearRoomState()
	{
		DestroyAllLinks(); // the mesh only spans the current room
		_room.clear();
		_host = 0;
		_room_open = true;
		_room_passworded = false;
		_room_listed = false;
		_listing.clear();
		_members.clear();
		_names.clear();
		_data.clear();
		_member_data.clear();
	}

	bool Client::DisconnectServer()
	{
		_asio->socket.close();
		ClearRoomState();
		return false;
	}
}
