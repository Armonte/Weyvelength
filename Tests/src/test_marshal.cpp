#include <thirdparty/doctest/doctest.h>

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "marshal.h"

using namespace Weyvelength;

// A new alternative that reaches the client needs a case in FillEvent, or it
// falls through to false; these pins flag a variant or error-enum change.
static_assert(std::variant_size_v<Proto::ServerMessage> == 34);
static_assert(WEYVE_EXTENSION_PROBE == Proto::extension_probe_timestamp);
static_assert((int)WEYVE_ROOM_ERROR_ALREADY_IN_ROOM == (int)Proto::RoomErrorCode::AlreadyInRoom);

// The C header restates these rather than including protocol.h, so pin the copies.
static_assert(WEYVE_MAX_NAME == Proto::max_client_name);
static_assert(std::string_view(WEYVE_DEFAULT_NAME) == Proto::default_client_name);
static_assert((int)WEYVE_ROOM_ERROR_BANNED == (int)Proto::RoomErrorCode::Banned);
static_assert((int)WEYVE_ROOM_ERROR_RATE_LIMITED == (int)Proto::RoomErrorCode::RateLimited);

// Filter ops cross the ABI as a plain cast too, so they map value for value.
static_assert((int)WEYVE_FILTER_EXISTS == (int)Proto::RoomFilterOp::Exists);
static_assert((int)WEYVE_FILTER_EQUALS == (int)Proto::RoomFilterOp::Equals);

namespace {
	bool Surfaced(const Proto::ServerMessage& msg)
	{
		WeyveEvent event{};
		return Marshal::FillEvent(msg, &event);
	}
}

TEST_CASE("heartbeat maps to a pong event")
{
	Proto::ServerMessage msg = Proto::Heartbeat{ 0x1122334455667788 };
	WeyveEvent e{};
	REQUIRE(Marshal::FillEvent(msg, &e));
	CHECK(e.type == WEYVE_EVENT_HEARTBEAT);
	CHECK(e.data.heartbeat.timestamp == 0x1122334455667788);
}

TEST_CASE("assigned room id is surfaced as a borrowed byte range")
{
	Proto::ServerMessage msg = Proto::AssignRoomId{ "VY4C3NB9" };
	WeyveEvent e{};
	REQUIRE(Marshal::FillEvent(msg, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_ID_ASSIGNED);
	CHECK(std::string(e.data.room_assigned.id, e.data.room_assigned.id_len) == "VY4C3NB9");
	CHECK(e.data.room_assigned.id == std::get<Proto::AssignRoomId>(msg).id.data());
}

TEST_CASE("room error carries the mapped code and context")
{
	Proto::ServerMessage msg = Proto::RoomError{ Proto::RoomErrorCode::NoSuchRoom, "ROOMCODE" };
	WeyveEvent e{};
	REQUIRE(Marshal::FillEvent(msg, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_ERROR);
	CHECK(e.data.room_error.code == WEYVE_ROOM_ERROR_NO_SUCH_ROOM);
	CHECK(std::string(e.data.room_error.context, e.data.room_error.context_len) == "ROOMCODE");
}

TEST_CASE("chat carries the sender and text, spaces included")
{
	Proto::ServerMessage msg = Proto::RoomChat{ 7, "two words  and spaces" };
	WeyveEvent e{};
	REQUIRE(Marshal::FillEvent(msg, &e));
	CHECK(e.type == WEYVE_EVENT_CHAT);
	CHECK(e.data.chat.from == 7);
	CHECK(std::string(e.data.chat.text, e.data.chat.text_len) == "two words  and spaces");
}

TEST_CASE("peer membership events carry the client id")
{
	Proto::ServerMessage joined = Proto::PeerJoined{ 3, "Ada" };
	Proto::ServerMessage left = Proto::PeerLeft{ 3 };
	Proto::ServerMessage host = Proto::HostChanged{ 3 };
	WeyveEvent e{};

	REQUIRE(Marshal::FillEvent(joined, &e));
	CHECK(e.type == WEYVE_EVENT_PEER_JOINED);
	CHECK(e.data.peer_joined.id == 3);
	CHECK(std::string(e.data.peer_joined.name, e.data.peer_joined.name_len) == "Ada");
	CHECK(e.data.peer_joined.name == std::get<Proto::PeerJoined>(joined).name.data());

	REQUIRE(Marshal::FillEvent(left, &e));
	CHECK(e.type == WEYVE_EVENT_PEER_LEFT);
	CHECK(e.data.peer_left.id == 3);

	REQUIRE(Marshal::FillEvent(host, &e));
	CHECK(e.type == WEYVE_EVENT_HOST_CHANGED);
	CHECK(e.data.host_changed.id == 3);
}

TEST_CASE("room data change carries key/value, empty value means delete")
{
	Proto::ServerMessage set = Proto::RoomDataChanged{ "stage", "training" };
	WeyveEvent e{};
	REQUIRE(Marshal::FillEvent(set, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_DATA_CHANGED);
	CHECK(std::string(e.data.room_data.key, e.data.room_data.key_len) == "stage");
	CHECK(std::string(e.data.room_data.value, e.data.room_data.value_len) == "training");

	Proto::ServerMessage del = Proto::RoomDataChanged{ "stage", "" };
	REQUIRE(Marshal::FillEvent(del, &e));
	CHECK(e.data.room_data.value_len == 0);
}

TEST_CASE("member data change carries the owner id, key and value")
{
	Proto::ServerMessage msg = Proto::MemberDataChanged{ 5, "char", "akuma" };
	WeyveEvent e{};
	REQUIRE(Marshal::FillEvent(msg, &e));
	CHECK(e.type == WEYVE_EVENT_MEMBER_DATA_CHANGED);
	CHECK(e.data.member_data.id == 5);
	CHECK(std::string(e.data.member_data.key, e.data.member_data.key_len) == "char");
	CHECK(std::string(e.data.member_data.value, e.data.member_data.value_len) == "akuma");
}

TEST_CASE("kick, ban and access notices map to their data-less events")
{
	Proto::ServerMessage kicked = Proto::KickedByHost{};
	Proto::ServerMessage banned = Proto::BannedByHost{};
	Proto::ServerMessage access = Proto::RoomAccessChanged{ false, true };
	WeyveEvent e{};

	REQUIRE(Marshal::FillEvent(kicked, &e));
	CHECK(e.type == WEYVE_EVENT_KICKED);

	REQUIRE(Marshal::FillEvent(banned, &e));
	CHECK(e.type == WEYVE_EVENT_BANNED);

	REQUIRE(Marshal::FillEvent(access, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_ACCESS_CHANGED);
	CHECK(e.data.room_access.open == false);
	CHECK(e.data.room_access.passworded == true);
}

TEST_CASE("room listing notices map to their events")
{
	Proto::ServerMessage listed = Proto::RoomListedChanged{ true };
	WeyveEvent e{};

	REQUIRE(Marshal::FillEvent(listed, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_LISTED_CHANGED);
	CHECK(e.data.room_listed.listed == true);

	Proto::ServerMessage slot = Proto::RoomListingChanged{ "mode", "ranked" };
	REQUIRE(Marshal::FillEvent(slot, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_LISTING_CHANGED);
	CHECK(std::string(e.data.room_listing.key, e.data.room_listing.key_len) == "mode");
	CHECK(std::string(e.data.room_listing.value, e.data.room_listing.value_len) == "ranked");

	Proto::ServerMessage cleared = Proto::RoomListingChanged{ "mode", "" };
	REQUIRE(Marshal::FillEvent(cleared, &e));
	CHECK(e.data.room_listing.value_len == 0);

	// The reply event is a summary; the rooms themselves live in the client cache.
	Proto::ServerMessage list = Proto::RoomList{ { { "VY4C3NB9", 3, false, true, { { "mode", "ranked" } } } } };
	REQUIRE(Marshal::FillEvent(list, &e));
	CHECK(e.type == WEYVE_EVENT_ROOM_LIST);
	CHECK(e.data.room_list.count == 1);

	Proto::ServerMessage empty = Proto::RoomList{};
	REQUIRE(Marshal::FillEvent(empty, &e));
	CHECK(e.data.room_list.count == 0);
}

TEST_CASE("an extension message surfaces with its kind and bytes")
{
	WeyveEvent e{};
	Proto::ServerMessage ext = Proto::Extension{ 7, "body" };
	REQUIRE(Marshal::FillEvent(ext, &e));
	CHECK(e.type == WEYVE_EVENT_EXTENSION);
	CHECK(e.data.extension.kind == 7);
	CHECK(std::string_view((const char*)e.data.extension.body, e.data.extension.body_len) == "body");
}

TEST_CASE("client->server and transport variants are not surfaced")
{
	CHECK(!Surfaced(Proto::AssignClientId{ 5, "Ada" }));
	CHECK(!Surfaced(Proto::SetName{ "Ada" }));
	CHECK(!Surfaced(Proto::CreateRoom{}));
	CHECK(!Surfaced(Proto::JoinRoom{ "ROOMCODE" }));
	CHECK(!Surfaced(Proto::SetRoomData{ "k", "v" }));
	CHECK(!Surfaced(Proto::P2PSignal{ 1, Proto::P2PSignalKind::Candidate, "x" }));
	CHECK(!Surfaced(Proto::IceServers{ "stun", 3478 }));
	CHECK(!Surfaced(Proto::SetRoomListed{ true }));
	CHECK(!Surfaced(Proto::SetRoomListing{ "mode", "ranked" }));
	CHECK(!Surfaced(Proto::ListRooms{}));
}

TEST_CASE("Str turns a C string into std::string, null into empty")
{
	CHECK(Marshal::Str(nullptr).empty());
	CHECK(Marshal::Str("") == "");
	CHECK(Marshal::Str("abc") == "abc");
}

TEST_CASE("Bytes borrows the value, null stays null, empty is a valid pointer")
{
	uint32_t len = 123;
	CHECK(Marshal::Bytes(nullptr, &len) == nullptr);
	CHECK(len == 0);

	std::string value = "payload";
	CHECK(Marshal::Bytes(&value, &len) == value.data());
	CHECK(len == value.size());

	std::string empty;
	CHECK(Marshal::Bytes(&empty, &len) != nullptr);
	CHECK(len == 0);
}

TEST_CASE("Find returns the value, an unset key is null")
{
	std::map<std::string, std::string> data{ { "mode", "ranked" } };

	const std::string* value = Marshal::Find(data, "mode");
	REQUIRE(value != nullptr);
	CHECK(*value == "ranked");
	CHECK(Marshal::Find(data, "stage") == nullptr);
}

TEST_CASE("RoomAt indexes the browsed rooms, past the end is null")
{
	std::vector<Proto::RoomInfo> rooms{ { "VY4C3NB9", 3, false, true, {} }, { "ZK7QD2XM", 1, true, false, {} } };

	REQUIRE(Marshal::RoomAt(rooms, 1) != nullptr);
	CHECK(Marshal::RoomAt(rooms, 1)->id == "ZK7QD2XM");
	CHECK(Marshal::RoomAt(rooms, 0)->open == false);
	CHECK(Marshal::RoomAt(rooms, 1)->open == true);
	CHECK(Marshal::RoomAt(rooms, 2) == nullptr);
	CHECK(Marshal::RoomAt({}, 0) == nullptr);
}

TEST_CASE("Filters convert across the ABI, a null array is no filters")
{
	WeyveRoomFilter filters[]{ { "mode", WEYVE_FILTER_EQUALS, "ranked" },
							   { "stage", WEYVE_FILTER_EXISTS, nullptr } };

	auto out = Marshal::Filters(filters, 2);
	REQUIRE(out.size() == 2);
	CHECK(out[0].key == "mode");
	CHECK(out[0].op == Proto::RoomFilterOp::Equals);
	CHECK(out[0].value == "ranked");
	CHECK(out[1].op == Proto::RoomFilterOp::Exists);
	CHECK(out[1].value.empty()); // null value, not a crash

	CHECK(Marshal::Filters(nullptr, 3).empty());
	CHECK(Marshal::Filters(filters, 0).empty());
}

TEST_CASE("KeyAt walks the keys in order, past the end is null")
{
	std::map<std::string, std::string> data{ { "alpha", "1" }, { "beta", "2" } };
	uint32_t len = 0;

	const char* first = Marshal::KeyAt(data, 0, &len);
	CHECK(std::string(first, len) == "alpha");

	const char* second = Marshal::KeyAt(data, 1, &len);
	CHECK(std::string(second, len) == "beta");

	CHECK(Marshal::KeyAt(data, 2, &len) == nullptr);
	CHECK(len == 0);
}
