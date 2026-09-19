#include "weyvelength.h"

#include <string>

#include "client.h"
#include "marshal.h"

using namespace Weyvelength;

// The opaque handle: one C++ client plus scratch that keeps the bytes we hand
// back alive until the caller's next pull.
struct WeyveClient {
	Client client;
	Proto::ServerMessage event; // backs the string pointers in the last WeyveEvent
	Proto::P2PMessage p2p; // backs the buffer from the last weyve_next_p2p
};

// --- lifecycle ---

WeyveClient* weyve_client_create(void)
{
	return new WeyveClient();
}

void weyve_client_destroy(WeyveClient* client)
{
	delete client;
}

bool weyve_connect(WeyveClient* client, const char* host, uint16_t port)
{
	ClientConfig config;
	if (host)
		config.host = host;
	config.port = port;
	return client->client.Connect(config);
}

bool weyve_poll(WeyveClient* client)
{
	return client->client.Poll();
}

bool weyve_next(WeyveClient* client, WeyveEvent* out)
{
	while (client->client.Next(client->event)) {
		if (Marshal::FillEvent(client->event, out))
			return true;
	}
	return false;
}

bool weyve_send_heartbeat(WeyveClient* client, uint64_t timestamp)
{
	return client->client.SendServer(Proto::Heartbeat{ timestamp });
}

// --- identity ---

bool weyve_set_name(WeyveClient* client, const char* name)
{
	return client->client.SetName(Marshal::Str(name));
}

// --- rooms ---

bool weyve_create_room(WeyveClient* client)
{
	return client->client.CreateRoom();
}

bool weyve_join_room(WeyveClient* client, const char* id, const char* password)
{
	return client->client.JoinRoom(Marshal::Str(id), Marshal::Str(password));
}

bool weyve_leave_room(WeyveClient* client)
{
	return client->client.LeaveRoom();
}

bool weyve_kick_member(WeyveClient* client, uint32_t id)
{
	return client->client.KickMember(id);
}

bool weyve_ban_member(WeyveClient* client, uint32_t id)
{
	return client->client.BanMember(id);
}

bool weyve_transfer_host(WeyveClient* client, uint32_t id)
{
	return client->client.TransferHost(id);
}

bool weyve_set_room_joinable(WeyveClient* client, bool open)
{
	return client->client.SetRoomJoinable(open);
}

bool weyve_set_room_password(WeyveClient* client, const char* password)
{
	return client->client.SetRoomPassword(Marshal::Str(password));
}

bool weyve_set_room_listed(WeyveClient* client, bool listed)
{
	return client->client.SetRoomListed(listed);
}

bool weyve_set_room_listing(WeyveClient* client, const char* key, const char* value)
{
	return client->client.SetRoomListing(Marshal::Str(key), Marshal::Str(value));
}

bool weyve_delete_room_listing(WeyveClient* client, const char* key)
{
	return client->client.DeleteRoomListing(Marshal::Str(key));
}

bool weyve_list_rooms(WeyveClient* client, const WeyveRoomFilter* filters, uint32_t count)
{
	return client->client.ListRooms(Marshal::Filters(filters, count));
}

bool weyve_send_chat(WeyveClient* client, const char* text)
{
	return client->client.SendChat(Marshal::Str(text));
}

bool weyve_set_room_data(WeyveClient* client, const char* key, const char* value)
{
	return client->client.SetRoomData(Marshal::Str(key), Marshal::Str(value));
}

bool weyve_delete_room_data(WeyveClient* client, const char* key)
{
	return client->client.DeleteRoomData(Marshal::Str(key));
}

bool weyve_set_member_data(WeyveClient* client, const char* key, const char* value)
{
	return client->client.SetMemberData(Marshal::Str(key), Marshal::Str(value));
}

bool weyve_delete_member_data(WeyveClient* client, const char* key)
{
	return client->client.DeleteMemberData(Marshal::Str(key));
}

// --- peer to peer ---

bool weyve_send_p2p(WeyveClient* client, uint32_t id, const void* data, uint32_t len)
{
	auto* bytes = (const std::byte*)data;
	return client->client.SendP2P(id, { bytes, bytes + len });
}

const uint8_t* weyve_next_p2p(WeyveClient* client, uint32_t* from, uint32_t* len)
{
	uint32_t sender = 0;
	if (!client->client.NextP2P(sender, client->p2p))
		return nullptr;
	if (from)
		*from = sender;
	if (len)
		*len = (uint32_t)client->p2p.size();
	return (const uint8_t*)client->p2p.data();
}

bool weyve_peer_connected(WeyveClient* client, uint32_t id)
{
	return client->client.PeerConnectedP2P(id);
}

WeyveLinkState weyve_peer_link_state(const WeyveClient* client, uint32_t id)
{
	return (WeyveLinkState)client->client.PeerLinkState(id);
}

// --- cached room state ---

uint32_t weyve_id(const WeyveClient* client)
{
	return client->client.Id();
}

const char* weyve_name(const WeyveClient* client, uint32_t* len)
{
	return Marshal::Bytes(&client->client.Name(), len);
}

const char* weyve_peer_name(const WeyveClient* client, uint32_t id, uint32_t* len)
{
	return Marshal::Bytes(client->client.PeerName(id), len);
}

uint32_t weyve_host_id(const WeyveClient* client)
{
	return client->client.HostId();
}

bool weyve_is_host(const WeyveClient* client)
{
	return client->client.IsHost();
}

bool weyve_room_joinable(const WeyveClient* client)
{
	return client->client.RoomJoinable();
}

bool weyve_room_passworded(const WeyveClient* client)
{
	return client->client.RoomPassworded();
}

bool weyve_room_listed(const WeyveClient* client)
{
	return client->client.RoomListed();
}

const char* weyve_room_id(const WeyveClient* client, uint32_t* len)
{
	const std::string& room = client->client.RoomId();
	if (len)
		*len = (uint32_t)room.size();
	return room.data();
}

const uint32_t* weyve_members(const WeyveClient* client, uint32_t* count)
{
	const std::vector<uint32_t>& members = client->client.Members();
	if (count)
		*count = (uint32_t)members.size();
	return members.data();
}

const char* weyve_room_data(const WeyveClient* client, const char* key, uint32_t* value_len)
{
	return Marshal::Bytes(client->client.RoomData(Marshal::Str(key)), value_len);
}

uint32_t weyve_room_data_count(const WeyveClient* client)
{
	return (uint32_t)client->client.RoomData().size();
}

const char* weyve_room_data_key_at(const WeyveClient* client, uint32_t index, uint32_t* key_len)
{
	return Marshal::KeyAt(client->client.RoomData(), index, key_len);
}

const char* weyve_member_data(const WeyveClient* client, uint32_t id, const char* key, uint32_t* value_len)
{
	return Marshal::Bytes(client->client.MemberData(id, Marshal::Str(key)), value_len);
}

uint32_t weyve_member_data_count(const WeyveClient* client, uint32_t id)
{
	const auto* data = client->client.MemberData(id);
	return data ? (uint32_t)data->size() : 0;
}

const char* weyve_member_data_key_at(const WeyveClient* client, uint32_t id, uint32_t index, uint32_t* key_len)
{
	const auto* data = client->client.MemberData(id);
	if (!data) {
		if (key_len)
			*key_len = 0;
		return nullptr;
	}
	return Marshal::KeyAt(*data, index, key_len);
}

const char* weyve_room_listing(const WeyveClient* client, const char* key, uint32_t* value_len)
{
	return Marshal::Bytes(client->client.RoomListing(Marshal::Str(key)), value_len);
}

uint32_t weyve_room_listing_count(const WeyveClient* client)
{
	return (uint32_t)client->client.RoomListing().size();
}

const char* weyve_room_listing_key_at(const WeyveClient* client, uint32_t index, uint32_t* key_len)
{
	return Marshal::KeyAt(client->client.RoomListing(), index, key_len);
}

// --- browsed rooms ---

uint32_t weyve_room_list_count(const WeyveClient* client)
{
	return (uint32_t)client->client.RoomList().size();
}

const char* weyve_room_list_id(const WeyveClient* client, uint32_t room, uint32_t* len)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	return Marshal::Bytes(info ? &info->id : nullptr, len);
}

uint32_t weyve_room_list_members(const WeyveClient* client, uint32_t room)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	return info ? info->members : 0;
}

bool weyve_room_list_joinable(const WeyveClient* client, uint32_t room)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	return info && info->open;
}

bool weyve_room_list_passworded(const WeyveClient* client, uint32_t room)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	return info && info->passworded;
}

const char* weyve_room_list_listing(const WeyveClient* client, uint32_t room, const char* key, uint32_t* value_len)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	return Marshal::Bytes(info ? Marshal::Find(info->listing, Marshal::Str(key)) : nullptr, value_len);
}

uint32_t weyve_room_list_listing_count(const WeyveClient* client, uint32_t room)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	return info ? (uint32_t)info->listing.size() : 0;
}

const char* weyve_room_list_listing_key_at(const WeyveClient* client, uint32_t room, uint32_t index, uint32_t* key_len)
{
	const auto* info = Marshal::RoomAt(client->client.RoomList(), room);
	if (!info) {
		if (key_len)
			*key_len = 0;
		return nullptr;
	}
	return Marshal::KeyAt(info->listing, index, key_len);
}
