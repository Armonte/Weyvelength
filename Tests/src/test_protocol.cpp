#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <thirdparty/doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <thirdparty/zpp_bits/zpp_bits.h>

#include "framing.h"
#include "protocol.h"

using namespace Weyvelength;

// The wire encodes a message as its variant index, so the order below IS the
// protocol. These pins turn the append-only comment in protocol.h into a
// compile error: inserting or reordering an alternative fails right here.
static_assert(std::variant_size_v<Proto::ServerMessage> == 32);
static_assert(std::is_same_v<std::variant_alternative_t<0, Proto::ServerMessage>, Proto::Heartbeat>);
static_assert(std::is_same_v<std::variant_alternative_t<1, Proto::ServerMessage>, Proto::AssignClientId>);
static_assert(std::is_same_v<std::variant_alternative_t<2, Proto::ServerMessage>, Proto::AssignRoomId>);
static_assert(std::is_same_v<std::variant_alternative_t<3, Proto::ServerMessage>, Proto::CreateRoom>);
static_assert(std::is_same_v<std::variant_alternative_t<4, Proto::ServerMessage>, Proto::JoinRoom>);
static_assert(std::is_same_v<std::variant_alternative_t<5, Proto::ServerMessage>, Proto::RoomError>);
static_assert(std::is_same_v<std::variant_alternative_t<6, Proto::ServerMessage>, Proto::RoomChat>);
static_assert(std::is_same_v<std::variant_alternative_t<7, Proto::ServerMessage>, Proto::LeaveRoom>);
static_assert(std::is_same_v<std::variant_alternative_t<8, Proto::ServerMessage>, Proto::PeerJoined>);
static_assert(std::is_same_v<std::variant_alternative_t<9, Proto::ServerMessage>, Proto::PeerLeft>);
static_assert(std::is_same_v<std::variant_alternative_t<10, Proto::ServerMessage>, Proto::HostChanged>);
static_assert(std::is_same_v<std::variant_alternative_t<11, Proto::ServerMessage>, Proto::SetRoomData>);
static_assert(std::is_same_v<std::variant_alternative_t<12, Proto::ServerMessage>, Proto::RoomDataChanged>);
static_assert(std::is_same_v<std::variant_alternative_t<13, Proto::ServerMessage>, Proto::SetMemberData>);
static_assert(std::is_same_v<std::variant_alternative_t<14, Proto::ServerMessage>, Proto::MemberDataChanged>);
static_assert(std::is_same_v<std::variant_alternative_t<15, Proto::ServerMessage>, Proto::KickMember>);
static_assert(std::is_same_v<std::variant_alternative_t<16, Proto::ServerMessage>, Proto::TransferHost>);
static_assert(std::is_same_v<std::variant_alternative_t<17, Proto::ServerMessage>, Proto::SetRoomJoinable>);
static_assert(std::is_same_v<std::variant_alternative_t<18, Proto::ServerMessage>, Proto::SetRoomPassword>);
static_assert(std::is_same_v<std::variant_alternative_t<19, Proto::ServerMessage>, Proto::KickedByHost>);
static_assert(std::is_same_v<std::variant_alternative_t<20, Proto::ServerMessage>, Proto::RoomAccessChanged>);
static_assert(std::is_same_v<std::variant_alternative_t<21, Proto::ServerMessage>, Proto::BanMember>);
static_assert(std::is_same_v<std::variant_alternative_t<22, Proto::ServerMessage>, Proto::BannedByHost>);
static_assert(std::is_same_v<std::variant_alternative_t<23, Proto::ServerMessage>, Proto::P2PSignal>);
static_assert(std::is_same_v<std::variant_alternative_t<24, Proto::ServerMessage>, Proto::IceServers>);
static_assert(std::is_same_v<std::variant_alternative_t<25, Proto::ServerMessage>, Proto::SetRoomListed>);
static_assert(std::is_same_v<std::variant_alternative_t<26, Proto::ServerMessage>, Proto::RoomListedChanged>);
static_assert(std::is_same_v<std::variant_alternative_t<27, Proto::ServerMessage>, Proto::SetRoomListing>);
static_assert(std::is_same_v<std::variant_alternative_t<28, Proto::ServerMessage>, Proto::RoomListingChanged>);
static_assert(std::is_same_v<std::variant_alternative_t<29, Proto::ServerMessage>, Proto::ListRooms>);
static_assert(std::is_same_v<std::variant_alternative_t<30, Proto::ServerMessage>, Proto::RoomList>);
static_assert(std::is_same_v<std::variant_alternative_t<31, Proto::ServerMessage>, Proto::SetName>);

// Same idea for the error enum: the values are wire bytes, append only.
static_assert((uint8_t)Proto::RoomErrorCode::AlreadyInRoom == 0);
static_assert((uint8_t)Proto::RoomErrorCode::NoSuchRoom == 1);
static_assert((uint8_t)Proto::RoomErrorCode::NotInRoom == 2);
static_assert((uint8_t)Proto::RoomErrorCode::NotHost == 3);
static_assert((uint8_t)Proto::RoomErrorCode::BadRoomData == 4);
static_assert((uint8_t)Proto::RoomErrorCode::NoSuchMember == 5);
static_assert((uint8_t)Proto::RoomErrorCode::RoomClosed == 6);
static_assert((uint8_t)Proto::RoomErrorCode::BadPassword == 7);
static_assert((uint8_t)Proto::RoomErrorCode::Banned == 8);
static_assert((uint8_t)Proto::RoomErrorCode::RateLimited == 9);

// And the p2p signal kinds.
static_assert((uint8_t)Proto::P2PSignalKind::Description == 0);
static_assert((uint8_t)Proto::P2PSignalKind::Candidate == 1);
static_assert((uint8_t)Proto::P2PSignalKind::GatheringDone == 2);

// And the room list filter ops.
static_assert((uint8_t)Proto::RoomFilterOp::Exists == 0);
static_assert((uint8_t)Proto::RoomFilterOp::Equals == 1);

// A reply carries every match, so the reassembly cap is the only ceiling.
// Pins roughly how many rooms fit, assuming ids stay under 64 bytes.
constexpr size_t worst_room_bytes = 14 + 64
	+ Proto::max_room_listing_keys * (8 + Proto::max_room_listing_key + Proto::max_room_listing_value);
static_assert(Proto::max_reassembled_size / worst_room_bytes >= 60);

namespace {
	// Frames a message, then walks the fragment stream and reassembles it the
	// way both peers do, verifying every header along the way.
	Proto::ServerMessage RoundTrip(const Proto::ServerMessage& msg)
	{
		std::vector<std::byte> frames = Proto::FrameMessage(msg);

		std::vector<std::byte> body;
		std::span<const std::byte> remaining{ frames };
		bool more = true;

		while (more) {
			REQUIRE(remaining.size() >= Proto::frame_header_size);

			uint32_t len = 0;
			REQUIRE(Proto::DecodeFrameHeader(remaining.first(Proto::frame_header_size), len, more));
			REQUIRE(remaining.size() >= Proto::frame_header_size + len);
			REQUIRE(Proto::AppendFragment(body, remaining.subspan(Proto::frame_header_size, len)));
			remaining = remaining.subspan(Proto::frame_header_size + len);
		}

		REQUIRE(remaining.empty()); // no bytes past the final fragment

		Proto::ServerMessage out;
		REQUIRE(!failure(zpp::bits::in{ body }(out)));
		REQUIRE(out.index() == msg.index());
		return out;
	}
}

TEST_CASE("heartbeat round trips with its timestamp")
{
	auto out = std::get<Proto::Heartbeat>(RoundTrip(Proto::Heartbeat{ 0x1122334455667788 }));
	CHECK(out.timestamp == 0x1122334455667788);
}

TEST_CASE("identity and room assignment round trip")
{
	CHECK(std::get<Proto::AssignClientId>(RoundTrip(Proto::AssignClientId{ 42 })).id == 42);
	CHECK(std::get<Proto::AssignRoomId>(RoundTrip(Proto::AssignRoomId{ "VY4C3NB9" })).id == "VY4C3NB9");
}

TEST_CASE("room requests round trip")
{
	RoundTrip(Proto::CreateRoom{}); // the index check inside is the whole test
	RoundTrip(Proto::LeaveRoom{});

	auto join = std::get<Proto::JoinRoom>(RoundTrip(Proto::JoinRoom{ "ROOMCODE", "hunter2" }));
	CHECK(join.id == "ROOMCODE");
	CHECK(join.password == "hunter2");

	CHECK(std::get<Proto::JoinRoom>(RoundTrip(Proto::JoinRoom{ "ROOMCODE" })).password.empty());
}

TEST_CASE("room errors round trip with code and context")
{
	auto out = std::get<Proto::RoomError>(RoundTrip(Proto::RoomError{ Proto::RoomErrorCode::NoSuchRoom, "ROOMCODE" }));
	CHECK(out.code == Proto::RoomErrorCode::NoSuchRoom);
	CHECK(out.context == "ROOMCODE");
}

TEST_CASE("chat round trips, spaces and empty text included")
{
	auto out = std::get<Proto::RoomChat>(RoundTrip(Proto::RoomChat{ 7, "two words  and spaces" }));
	CHECK(out.from == 7);
	CHECK(out.text == "two words  and spaces");

	CHECK(std::get<Proto::RoomChat>(RoundTrip(Proto::RoomChat{ 7, "" })).text.empty());
}

TEST_CASE("room events round trip")
{
	CHECK(std::get<Proto::PeerJoined>(RoundTrip(Proto::PeerJoined{ 3 })).id == 3);
	CHECK(std::get<Proto::PeerLeft>(RoundTrip(Proto::PeerLeft{ 3 })).id == 3);
	CHECK(std::get<Proto::HostChanged>(RoundTrip(Proto::HostChanged{ 3 })).id == 3);
}

TEST_CASE("host actions round trip")
{
	CHECK(std::get<Proto::KickMember>(RoundTrip(Proto::KickMember{ 3 })).id == 3);
	CHECK(std::get<Proto::BanMember>(RoundTrip(Proto::BanMember{ 3 })).id == 3);
	CHECK(std::get<Proto::TransferHost>(RoundTrip(Proto::TransferHost{ 3 })).id == 3);
	CHECK(std::get<Proto::SetRoomJoinable>(RoundTrip(Proto::SetRoomJoinable{ false })).open == false);

	CHECK(std::get<Proto::SetRoomPassword>(RoundTrip(Proto::SetRoomPassword{ "hunter2" })).password == "hunter2");
	CHECK(std::get<Proto::SetRoomPassword>(RoundTrip(Proto::SetRoomPassword{ "" })).password.empty()); // empty = clear
}

TEST_CASE("room access events round trip")
{
	RoundTrip(Proto::KickedByHost{}); // the index check inside is the whole test
	RoundTrip(Proto::BannedByHost{});

	auto access = std::get<Proto::RoomAccessChanged>(RoundTrip(Proto::RoomAccessChanged{ false, true }));
	CHECK(access.open == false);
	CHECK(access.passworded == true);
}

TEST_CASE("client names round trip on every message that carries one")
{
	CHECK(std::get<Proto::SetName>(RoundTrip(Proto::SetName{ "Ada" })).name == "Ada");
	CHECK(std::get<Proto::SetName>(RoundTrip(Proto::SetName{ "" })).name.empty()); // empty = reset to the default

	auto self = std::get<Proto::AssignClientId>(RoundTrip(Proto::AssignClientId{ 7, "Ada" }));
	CHECK(self.id == 7);
	CHECK(self.name == "Ada");

	auto peer = std::get<Proto::PeerJoined>(RoundTrip(Proto::PeerJoined{ 3, Proto::default_client_name }));
	CHECK(peer.id == 3);
	CHECK(peer.name == "WEYVE_PLAYER");
}

TEST_CASE("room listing requests and notices round trip")
{
	CHECK(std::get<Proto::SetRoomListed>(RoundTrip(Proto::SetRoomListed{ true })).listed == true);
	CHECK(std::get<Proto::RoomListedChanged>(RoundTrip(Proto::RoomListedChanged{ true })).listed == true);
	CHECK(std::get<Proto::RoomListedChanged>(RoundTrip(Proto::RoomListedChanged{ false })).listed == false);

	auto slot = std::get<Proto::SetRoomListing>(RoundTrip(Proto::SetRoomListing{ "mode", "ranked" }));
	CHECK(slot.key == "mode");
	CHECK(slot.value == "ranked");

	auto cleared = std::get<Proto::RoomListingChanged>(RoundTrip(Proto::RoomListingChanged{ "mode", "" }));
	CHECK(cleared.key == "mode");
	CHECK(cleared.value.empty()); // empty value = cleared

	Proto::ListRooms query{ { { "mode", Proto::RoomFilterOp::Equals, "ranked" },
							  { "stage", Proto::RoomFilterOp::Exists, "" } } };

	auto out = std::get<Proto::ListRooms>(RoundTrip(query));
	REQUIRE(out.filters.size() == 2);
	CHECK(out.filters[0].key == "mode");
	CHECK(out.filters[0].op == Proto::RoomFilterOp::Equals);
	CHECK(out.filters[0].value == "ranked");
	CHECK(out.filters[1].op == Proto::RoomFilterOp::Exists);

	CHECK(std::get<Proto::ListRooms>(RoundTrip(Proto::ListRooms{})).filters.empty()); // no filters = everything listed
}

TEST_CASE("room lists round trip with their listing slots")
{
	Proto::RoomList list{ { { "VY4C3NB9", 3, false, true, { { "mode", "ranked" }, { "stage", "training" } } },
							{ "ZK7QD2XM", 1, true, false, {} } } };

	auto out = std::get<Proto::RoomList>(RoundTrip(list));
	REQUIRE(out.rooms.size() == 2);

	CHECK(out.rooms[0].id == "VY4C3NB9");
	CHECK(out.rooms[0].members == 3);
	CHECK(out.rooms[0].open == false);
	CHECK(out.rooms[0].passworded == true);
	REQUIRE(out.rooms[0].listing.size() == 2);
	CHECK(out.rooms[0].listing.at("mode") == "ranked");
	CHECK(out.rooms[0].listing.at("stage") == "training");

	CHECK(out.rooms[1].members == 1);
	CHECK(out.rooms[1].open == true);
	CHECK(out.rooms[1].passworded == false);
	CHECK(out.rooms[1].listing.empty());

	CHECK(std::get<Proto::RoomList>(RoundTrip(Proto::RoomList{})).rooms.empty()); // nothing matched
}

TEST_CASE("a reply of maxed-out rooms fits what a peer will reassemble")
{
	// the static_assert above, against the real encoder
	Proto::RoomInfo entry{ std::string(64, 'R'), 0xFFFFFFFF, false, true, {} };
	for (uint32_t i = 0; i < Proto::max_room_listing_keys; i++) {
		entry.listing.emplace(std::to_string(i) + std::string(Proto::max_room_listing_key - 1, 'k'),
			std::string(Proto::max_room_listing_value, 'v'));
	}

	constexpr size_t rooms = Proto::max_reassembled_size / worst_room_bytes;
	Proto::RoomList list{ std::vector<Proto::RoomInfo>(rooms, entry) };

	std::vector<std::byte> body;
	zpp::bits::out{ body }(Proto::ServerMessage{ list }).or_throw();
	CHECK(body.size() <= Proto::max_reassembled_size); // the per-room estimate is not an under-count

	auto out = std::get<Proto::RoomList>(RoundTrip(list)); // fragments and comes back whole
	CHECK(out.rooms.size() == rooms);
}

TEST_CASE("p2p signaling round trips")
{
	auto sig = std::get<Proto::P2PSignal>(RoundTrip(Proto::P2PSignal{ 3, Proto::P2PSignalKind::Candidate, "a=candidate:1 1 UDP 2122317823 192.168.0.10 50000 typ host" }));
	CHECK(sig.id == 3);
	CHECK(sig.kind == Proto::P2PSignalKind::Candidate);
	CHECK(sig.payload.starts_with("a=candidate"));

	auto ice = std::get<Proto::IceServers>(RoundTrip(Proto::IceServers{ "stun.example.net", 19302, { { "turn.example.net", 3478, "user", "pass" } } }));
	CHECK(ice.stun_host == "stun.example.net");
	CHECK(ice.stun_port == 19302);
	REQUIRE(ice.turn.size() == 1);
	CHECK(ice.turn[0].host == "turn.example.net");
	CHECK(ice.turn[0].port == 3478);
	CHECK(ice.turn[0].username == "user");
	CHECK(ice.turn[0].password == "pass");
}

TEST_CASE("data messages round trip, empty value (= delete) included")
{
	auto set = std::get<Proto::SetRoomData>(RoundTrip(Proto::SetRoomData{ "stage", "training" }));
	CHECK(set.key == "stage");
	CHECK(set.value == "training");

	auto del = std::get<Proto::RoomDataChanged>(RoundTrip(Proto::RoomDataChanged{ "stage", "" }));
	CHECK(del.key == "stage");
	CHECK(del.value.empty());

	auto mine = std::get<Proto::SetMemberData>(RoundTrip(Proto::SetMemberData{ "char", "ryu" }));
	CHECK(mine.key == "char");
	CHECK(mine.value == "ryu");

	auto member = std::get<Proto::MemberDataChanged>(RoundTrip(Proto::MemberDataChanged{ 5, "char", "akuma" }));
	CHECK(member.id == 5);
	CHECK(member.key == "char");
	CHECK(member.value == "akuma");
}

TEST_CASE("the largest allowed data pair fits under the frame cap")
{
	std::string key(Proto::max_room_data_key, 'k');
	std::string value(Proto::max_room_data_value, 'v');

	// MemberDataChanged is the biggest data message on the wire: it carries
	// the owner id on top of the maxed key/value pair.
	std::vector<std::byte> frame = Proto::FrameMessage(Proto::MemberDataChanged{ 0xFFFFFFFF, key, value });
	CHECK(frame.size() - Proto::frame_header_size <= Proto::max_message_size);

	frame = Proto::FrameMessage(Proto::RoomDataChanged{ key, value });
	CHECK(frame.size() - Proto::frame_header_size <= Proto::max_message_size);

	RoundTrip(Proto::MemberDataChanged{ 0xFFFFFFFF, key, value });
}

TEST_CASE("oversized messages fragment and reassemble")
{
	std::string sdp(3 * Proto::max_message_size, 's'); // forces a multi-fragment stream
	auto out = std::get<Proto::P2PSignal>(RoundTrip(Proto::P2PSignal{ 7, Proto::P2PSignalKind::Description, sdp }));
	CHECK(out.payload == sdp); // RoundTrip walked headers and reassembled along the way
}

TEST_CASE("single-fragment frames keep the plain length header")
{
	std::vector<std::byte> frame = Proto::FrameMessage(Proto::Heartbeat{ 1 });

	uint32_t len = 0;
	bool more = true;
	REQUIRE(Proto::DecodeFrameHeader(std::span<const std::byte>{ frame }.first(Proto::frame_header_size), len, more));
	CHECK(!more); // top bit clear: byte-identical to the pre-fragmentation framing
	CHECK(len == frame.size() - Proto::frame_header_size);
}

TEST_CASE("frame headers over the cap are rejected, reassembly is capped")
{
	std::vector<std::byte> header;
	zpp::bits::out{ header }(uint32_t{ Proto::max_message_size + 1 }).or_throw();

	uint32_t len = 0;
	bool more = false;
	CHECK(!Proto::DecodeFrameHeader(header, len, more));

	header.clear();
	zpp::bits::out{ header }(Proto::max_message_size | Proto::frame_more_flag).or_throw();
	CHECK(Proto::DecodeFrameHeader(header, len, more));
	CHECK(more);
	CHECK(len == Proto::max_message_size);

	std::vector<std::byte> body(Proto::max_reassembled_size - 10);
	std::array<std::byte, 20> extra{};
	CHECK(!Proto::AppendFragment(body, extra)); // one byte over the reassembly cap fails
}

// A frame that claims a huge string length must be refused before anything is
// allocated (zpp_bits resizes to the wire length before reading the bytes).
TEST_CASE("DecodeMessage refuses a container length the message cannot hold")
{
	Proto::ServerMessage in = Proto::AssignRoomId{ "abc" };
	std::vector<std::byte> body;
	zpp::bits::out{ body }(in).or_throw();

	// locate the string's u32 length prefix (3) right before "abc" and forge it
	size_t at = 0;
	for (size_t i = 0; i + 7 <= body.size(); ++i)
		if (body[i] == std::byte{ 3 } && body[i + 1] == std::byte{ 0 } && body[i + 2] == std::byte{ 0 } && body[i + 3] == std::byte{ 0 } &&
		    body[i + 4] == std::byte{ 'a' })
			at = i + 1;
	REQUIRE(at != 0);
	--at;
	for (int b = 0; b < 4; ++b)
		body[at + b] = std::byte{ 0xF0 };   // 0xF0F0F0F0 bytes

	Proto::ServerMessage out;
	CHECK(!Proto::DecodeMessage(body, out));

	// the honest message still decodes
	std::vector<std::byte> good;
	zpp::bits::out{ good }(in).or_throw();
	REQUIRE(Proto::DecodeMessage(good, out));
	CHECK(std::get<Proto::AssignRoomId>(out).id == "abc");
}
