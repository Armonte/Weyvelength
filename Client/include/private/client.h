#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "link_recovery.h"
#include "protocol.h"

namespace Weyvelength {
	struct ClientAsioImpl;
	struct P2PMesh;
	struct PeerLink;
	struct JuiceEvent;

	struct ClientConfig {
		std::string host = "127.0.0.1";
		uint16_t port = 0;
	};

	struct Client {
		Client();
		~Client();

		bool Connect(ClientConfig& config);

		bool Poll();

		bool Next(Proto::ServerMessage& out);

		bool SendServer(const Proto::ServerMessage& msg);

		bool SetName(const std::string& name); // rejected while in a room; empty resets to the default

		bool CreateRoom(); // server replies AssignRoomId or RoomError
		bool JoinRoom(const std::string& id, const std::string& password = {}); // server replies AssignRoomId or RoomError
		bool LeaveRoom(); // server replies PeerLeft carrying our own id, or RoomError

		bool KickMember(uint32_t id); // host-only; the target gets KickedByHost, the room gets PeerLeft
		bool BanMember(uint32_t id); // host-only; the target gets BannedByHost and is barred from rejoining
		bool TransferHost(uint32_t id); // host-only; server replies HostChanged to the room

		bool SetRoomJoinable(bool open); // host-only; server replies RoomAccessChanged to the room
		bool SetRoomPassword(const std::string& password); // host-only; empty clears it
		bool SetRoomListed(bool listed); // host-only; server replies RoomListedChanged to the room
		bool SetRoomListing(const std::string& key, const std::string& value); // host-only; one of the room's listing slots
		bool DeleteRoomListing(const std::string& key); // host-only; sugar for an empty-value SetRoomListing

		bool ListRooms(const std::vector<Proto::RoomFilter>& filters = {}); // server replies RoomList, or RoomError if asked too often

		bool SendChat(const std::string& text); // broadcast to everyone in the current room
		bool SetRoomData(const std::string& key, const std::string& value); // host-only; server replies RoomDataChanged or RoomError
		bool DeleteRoomData(const std::string& key); // host-only; sugar for an empty-value SetRoomData
		bool SetMemberData(const std::string& key, const std::string& value); // our own slots; server replies MemberDataChanged or RoomError
		bool DeleteMemberData(const std::string& key); // sugar for an empty-value SetMemberData

		bool SendP2P(uint32_t id, const Proto::P2PMessage& msg); // direct to a room member; the first send builds the link, queued until it connects
		bool NextP2P(uint32_t& from, Proto::P2PMessage& out); // one received datagram per call
		bool PeerConnectedP2P(uint32_t id) const; // is a direct link to this member up right now?
		LinkState PeerLinkState(uint32_t id) const; // the same question with the reason a link is not up
		bool PeerRoute(uint32_t id, std::string& local, std::string& remote) const; // selected candidate types

		uint32_t Id() const;  // 0 until the server has assigned one
		const std::string& Name() const; // our own name, as the server accepted it
		const std::string* PeerName(uint32_t id) const; // null once they are no longer a member
		const std::string& RoomId() const; // empty until a room has been joined
		uint32_t HostId() const; // 0 until a room has been joined
		bool IsHost() const;

		bool RoomJoinable() const; // can others join right now?
		bool RoomPassworded() const; // the flag only; the password itself never reaches clients
		bool RoomListed() const;

		const std::map<std::string, std::string>& RoomListing() const; // what browsers see of our room
		const std::string* RoomListing(const std::string& key) const; // null if the slot is empty

		// Browse results, not room state: they outlive joining and leaving.
		const std::vector<Proto::RoomInfo>& RoomList() const;

		const std::vector<uint32_t>& Members() const; // everyone in the room, ourselves included
		const std::map<std::string, std::string>& RoomData() const;
		const std::string* RoomData(const std::string& key) const; // null if the key is not set
		const std::map<std::string, std::string>* MemberData(uint32_t id) const; // null if the member has set nothing
		const std::string* MemberData(uint32_t id, const std::string& key) const; // null if the key is not set

	private:
		bool DrainServer();
		bool CarveServer();
		bool FlushServer();
		bool DisconnectServer();

		void CacheRoomState(const Proto::ServerMessage& msg);
		void ClearRoomState();

		void PollPeers();
		void RecoverLinks(); // applies link_recovery.h to every peer whose link is not up

		PeerLink* FindLink(uint32_t id);
		PeerLink* CreateLink(uint32_t id, LinkState state);

		bool ShareLink(PeerLink& link, uint32_t id);
		void FlushLink(PeerLink& link);
		bool LinkStalled(const PeerLink& link) const; // the route is gone, sooner than juice says so
		void MarkLinkDown(uint32_t id); // starts the drop clock, once

		void RedialLink(uint32_t id);
		void DestroyLink(uint32_t id);
		void DestroyAllLinks();

		void HandleP2PSignal(const Proto::P2PSignal& sig);
		void HandleP2PDescription(PeerLink* link, const Proto::P2PSignal& sig);
		void HandleJuiceEvent(JuiceEvent& ev);
		void HandleLinkState(PeerLink& link, const JuiceEvent& ev);

		std::unique_ptr<ClientAsioImpl> _asio;
		std::unique_ptr<P2PMesh> _mesh;

		uint64_t _now_ms = 0; // stamped once per Poll, so every link decision shares one clock

		Proto::IceServers _ice; // handed to us by the server; feeds every new link

		std::queue<Proto::ServerMessage> _inbox;
		std::queue<std::pair<uint32_t, Proto::P2PMessage>> _p2p_inbox;

		uint32_t _id = 0;
		std::string _name = Proto::default_client_name;
		std::string _room;
		uint32_t _host = 0;
		bool _room_open = true;
		bool _room_passworded = false;
		bool _room_listed = false;
		std::map<std::string, std::string> _listing;
		std::vector<Proto::RoomInfo> _room_list;
		std::vector<uint32_t> _members;
		std::map<uint32_t, std::string> _names; // members' names, for exactly as long as they are members
		std::map<std::string, std::string> _data;
		std::map<uint32_t, std::map<std::string, std::string>> _member_data;
	};
}
