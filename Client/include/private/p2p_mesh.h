#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <juice/juice.h>

#include "link_recovery.h"

// Client-internal state behind the p2p mesh; keeps juice types out of the API.
namespace Weyvelength {
	struct JuiceEvent { // one callback crossing from juice's threads into Poll
		enum class Kind { State, Candidate, GatheringDone, Recv };
		Kind kind{};
		juice_agent_t* agent = nullptr; // which agent queued it; a rebuilt link ignores strays
		uint32_t peer = 0;
		juice_state_t state = JUICE_STATE_DISCONNECTED; // Kind::State only
		std::vector<std::byte> payload; // candidate sdp or Kind::Recv datagram
	};

	struct JuiceCallbackContext { // one agent's user_ptr; freed after juice_destroy
		struct P2PMesh* mesh = nullptr;
		uint32_t peer = 0;
	};

	struct PeerLink { // one lazily built direct connection
		juice_agent_t* agent = nullptr;
		std::unique_ptr<JuiceCallbackContext> ctx;
		bool remote_set = false; // juice_set_remote_description may only run once
		bool connected = false;
		bool heard = false; // the peer has sent us at least one datagram over this agent
		uint64_t recv_ms = 0; // last datagram in, and last out; the two together spot a dead route
		uint64_t send_ms = 0; // before juice does, see RouteStalled
		std::deque<std::vector<std::byte>> outbox; // datagrams queued until the link connects
	};

	struct LinkRecovery { // one peer's ICE budget and drop clock; outlives the link, so a redial keeps both
		uint32_t attempts = 0; // ICE rounds spent on this peer
		uint64_t since_ms = 0; // when the current round, or the drop, started
		LinkState state = LinkState::Recovering; // Opening, Recovering or GaveUp; the record is erased once the link is up
	};

	struct P2PMesh {
		std::mutex mutex; // guards events only; links belong to the poll thread
		std::vector<JuiceEvent> events; // juice callbacks push, PollPeers drains
		std::vector<JuiceEvent> scratch; // the drained batch; reused so capacity sticks
		std::map<uint32_t, PeerLink> links;
		std::map<uint32_t, LinkRecovery> recovery; // exactly the peers whose link is not simply up
		std::vector<uint32_t> sweep; // the ids RecoverLinks walks; reused so capacity sticks
	};
}
