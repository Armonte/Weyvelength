#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "protocol.h"
#include "weyvelength.h"

// Marshalling between the C API's strings/events and the C++ client's types.
// Header-only and internal to the C wrapper; not part of the public C ABI.
namespace Weyvelength::Marshal {
	inline std::string Str(const char* str) // null is an empty string, not a crash
	{
		return str ? std::string(str) : std::string();
	}

	inline const char* Bytes(const std::string* value, uint32_t* len) // null stays null, empty is a valid pointer
	{
		if (!value) {
			if (len)
				*len = 0;
			return nullptr;
		}
		if (len)
			*len = (uint32_t)value->size();
		return value->data();
	}

	inline const std::string* Find(const std::map<std::string, std::string>& data, const std::string& key) // null if unset
	{
		auto it = data.find(key);
		return it == data.end() ? nullptr : &it->second;
	}

	inline const Proto::RoomInfo* RoomAt(const std::vector<Proto::RoomInfo>& rooms, uint32_t index) // null past the end
	{
		return index < rooms.size() ? &rooms[index] : nullptr;
	}

	// null array = no filters
	inline std::vector<Proto::RoomFilter> Filters(const WeyveRoomFilter* filters, uint32_t count)
	{
		std::vector<Proto::RoomFilter> out;
		if (!filters)
			return out;

		out.reserve(count);
		for (uint32_t i = 0; i < count; i++)
			out.push_back({ Str(filters[i].key), (Proto::RoomFilterOp)filters[i].op, Str(filters[i].value) });

		return out;
	}

	inline const char* KeyAt(const std::map<std::string, std::string>& data, uint32_t index, uint32_t* key_len)
	{
		if (index >= data.size()) {
			if (key_len)
				*key_len = 0;
			return nullptr;
		}
		auto it = data.begin();
		std::advance(it, index);
		if (key_len)
			*key_len = (uint32_t)it->first.size();
		return it->first.data();
	}

	// Translate one surfaced server message into the C event; false for the few
	// variants that never reach here, so weyve_next skips to the next.
	inline bool FillEvent(const Proto::ServerMessage& msg, WeyveEvent* out)
	{
		if (auto* beat = std::get_if<Proto::Heartbeat>(&msg)) {
			out->type = WEYVE_EVENT_HEARTBEAT;
			out->data.heartbeat.timestamp = beat->timestamp;
		}
		else if (auto* room = std::get_if<Proto::AssignRoomId>(&msg)) {
			out->type = WEYVE_EVENT_ROOM_ID_ASSIGNED;
			out->data.room_assigned.id = room->id.data();
			out->data.room_assigned.id_len = (uint32_t)room->id.size();
		}
		else if (auto* error = std::get_if<Proto::RoomError>(&msg)) {
			out->type = WEYVE_EVENT_ROOM_ERROR;
			out->data.room_error.code = (WeyveRoomError)error->code;
			out->data.room_error.context = error->context.data();
			out->data.room_error.context_len = (uint32_t)error->context.size();
		}
		else if (auto* chat = std::get_if<Proto::RoomChat>(&msg)) {
			out->type = WEYVE_EVENT_CHAT;
			out->data.chat.from = chat->from;
			out->data.chat.text = chat->text.data();
			out->data.chat.text_len = (uint32_t)chat->text.size();
		}
		else if (auto* joined = std::get_if<Proto::PeerJoined>(&msg)) {
			out->type = WEYVE_EVENT_PEER_JOINED;
			out->data.peer_joined.id = joined->id;
			out->data.peer_joined.name = joined->name.data();
			out->data.peer_joined.name_len = (uint32_t)joined->name.size();
		}
		else if (auto* left = std::get_if<Proto::PeerLeft>(&msg)) {
			out->type = WEYVE_EVENT_PEER_LEFT;
			out->data.peer_left.id = left->id;
		}
		else if (auto* host = std::get_if<Proto::HostChanged>(&msg)) {
			out->type = WEYVE_EVENT_HOST_CHANGED;
			out->data.host_changed.id = host->id;
		}
		else if (auto* data = std::get_if<Proto::RoomDataChanged>(&msg)) {
			out->type = WEYVE_EVENT_ROOM_DATA_CHANGED;
			out->data.room_data.key = data->key.data();
			out->data.room_data.key_len = (uint32_t)data->key.size();
			out->data.room_data.value = data->value.data();
			out->data.room_data.value_len = (uint32_t)data->value.size();
		}
		else if (auto* member = std::get_if<Proto::MemberDataChanged>(&msg)) {
			out->type = WEYVE_EVENT_MEMBER_DATA_CHANGED;
			out->data.member_data.id = member->id;
			out->data.member_data.key = member->key.data();
			out->data.member_data.key_len = (uint32_t)member->key.size();
			out->data.member_data.value = member->value.data();
			out->data.member_data.value_len = (uint32_t)member->value.size();
		}
		else if (std::get_if<Proto::KickedByHost>(&msg)) {
			out->type = WEYVE_EVENT_KICKED;
		}
		else if (std::get_if<Proto::BannedByHost>(&msg)) {
			out->type = WEYVE_EVENT_BANNED;
		}
		else if (auto* access = std::get_if<Proto::RoomAccessChanged>(&msg)) {
			out->type = WEYVE_EVENT_ROOM_ACCESS_CHANGED;
			out->data.room_access.open = access->open;
			out->data.room_access.passworded = access->passworded;
		}
		else if (auto* listed = std::get_if<Proto::RoomListedChanged>(&msg)) {
			out->type = WEYVE_EVENT_ROOM_LISTED_CHANGED;
			out->data.room_listed.listed = listed->listed;
		}
		else if (auto* listing = std::get_if<Proto::RoomListingChanged>(&msg)) {
			out->type = WEYVE_EVENT_ROOM_LISTING_CHANGED;
			out->data.room_listing.key = listing->key.data();
			out->data.room_listing.key_len = (uint32_t)listing->key.size();
			out->data.room_listing.value = listing->value.data();
			out->data.room_listing.value_len = (uint32_t)listing->value.size();
		}
		else if (auto* ext = std::get_if<Proto::Extension>(&msg)) {
			out->type = WEYVE_EVENT_EXTENSION;
			out->data.extension.kind = ext->kind;
			out->data.extension.body = (const uint8_t*)ext->body.data();
			out->data.extension.body_len = (uint32_t)ext->body.size();
		}
		else if (auto* list = std::get_if<Proto::RoomList>(&msg)) {
			// the rooms come from the client's cache; no union member could carry them
			out->type = WEYVE_EVENT_ROOM_LIST;
			out->data.room_list.count = (uint32_t)list->rooms.size();
		}
		else {
			return false; // a client->server variant we never receive
		}
		return true;
	}
}
