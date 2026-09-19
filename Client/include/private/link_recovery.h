#pragma once

#include <cstdint>

// When a peer link stops carrying traffic it is usually not a lost peer: the
// room server still lists both of us on a TCP control channel that never went
// anywhere, and all that moved is one side's NAT mapping. This is the policy
// that decides what to do about that and when. It holds no juice types, no
// sockets and no clock, so Tests drives it directly.
//
// What libjuice 1.7.3 allows sets the shape of it. There is no ICE restart:
// a second juice_set_remote_description with fresh credentials is refused
// outright (thirdparty/libjuice/src/agent.c, "ICE restart is not supported"),
// juice_gather_candidates on an agent that already gathered returns without
// doing anything, and juice_add_remote_candidate is rejected once remote
// gathering is done. So a route that is really gone can only be replaced by a
// whole new agent, and the one cheap repair left is the one juice does by
// itself: a failed agent keeps reading its socket and keeps learning peer
// reflexive candidates from the peer's own checks, so it can pair up again
// with the peer's new mapping at no cost. Hold before spending a round.
namespace Weyvelength {
	// How a link to one room member is doing; mirrored by the C API as WeyveLinkState.
	enum class LinkState : uint8_t {
		None, // no link; the next SendP2P opens one
		Opening, // the first ICE round for this peer is running
		Up, // the route carries traffic
		Recovering, // it dropped; sends queue while the link is held, redialed or waited on
		GaveUp, // out of ICE rounds; SendP2P fails until the peer leaves and rejoins
	};

	// What the recovery sweep should do with one peer this poll.
	enum class LinkAction : uint8_t {
		Healthy, // the route carries traffic; forget any recovery in progress
		Hold, // it dropped, but the agent we have may still re-pair itself: the cheapest fix there is
		Answer, // the peer holds the lower id, so the redial is theirs; keep our agent and wait for it
		Wait, // our own redial is not due yet
		Redial, // our turn: drop the agent and start a fresh ICE round
		GiveUp, // out of ICE rounds
		Drop, // the peer is not a room member any more
	};

	inline const char* LinkActionName(LinkAction action)
	{
		switch (action) {
		case LinkAction::Healthy: return "healthy";
		case LinkAction::Hold: return "hold, juice may re-pair the agent";
		case LinkAction::Answer: return "answer, the peer redials";
		case LinkAction::Wait: return "wait, the redial is not due";
		case LinkAction::Redial: return "redial";
		case LinkAction::GiveUp: return "gave up, out of attempts";
		case LinkAction::Drop: return "drop, no longer a member";
		}
		return "?";
	}

	struct RecoveryPolicy {
		uint32_t max_attempts; // ICE rounds per peer before we give up on it
		uint32_t stall_ms; // traffic we sent has gone unanswered this long = the route is gone
		uint32_t hold_ms; // after a drop, let juice try to re-pair before spending a round
		uint32_t answer_ms; // the answering side waits this long for the dialer, then dials anyway
		uint32_t backoff_ms; // wait before our own first redial, doubled per round already spent
		uint32_t backoff_max_ms;
	};

	constexpr uint32_t max_connection_attempts = 3; // ICE rounds per peer before we give up

	constexpr RecoveryPolicy default_recovery_policy{
		.max_attempts = max_connection_attempts,
		.stall_ms = 6000,
		.hold_ms = 1500,
		.answer_ms = 2000,
		.backoff_ms = 4000,
		.backoff_max_ms = 16000,
	};

	// Both sides usually notice a dropped route at the same moment, and two
	// simultaneous redials cost two ICE rounds where one would do. Ids are
	// unique inside a room, so the lower one dials and the higher one answers.
	// A peer that predates this rule just dials anyway; the loser of that race
	// answers its description as it always did, so the two mix safely.
	inline bool DialsFirst(uint32_t local_id, uint32_t peer_id)
	{
		return local_id < peer_id;
	}

	// juice only abandons a route after 30 s of failed consent checks
	// (CONSENT_TIMEOUT in thirdparty/libjuice/src/agent.h), which is a long
	// time to stand still mid match. A link we are actively sending on, that
	// has answered us before and has answered nothing since, is already gone.
	// An idle application never arms this, and neither does a peer that has
	// never sent us anything: those keep juice's own, slower verdict.
	inline bool RouteStalled(uint64_t since_recv_ms, uint64_t since_send_ms, bool heard, const RecoveryPolicy& policy)
	{
		return heard && since_send_ms < policy.stall_ms && since_recv_ms >= policy.stall_ms;
	}

	// Full jitter is pointless here: the dialer is picked by id, so there is
	// nothing to collide with. The doubling is only there to stop a peer that
	// fails fast from burning its whole budget in one visible stall.
	inline uint32_t RecoveryBackoffMs(uint32_t attempts, const RecoveryPolicy& policy)
	{
		uint64_t ms = policy.backoff_ms;
		for (uint32_t spent = 0; spent < attempts && ms < policy.backoff_max_ms; ++spent) {
			ms *= 2;
		}
		return (uint32_t)(ms < policy.backoff_max_ms ? ms : policy.backoff_max_ms);
	}

	// How long a link has to be down before we spend a round on it.
	inline uint64_t RedialDueMs(uint32_t attempts, bool dialer, const RecoveryPolicy& policy)
	{
		return (uint64_t)policy.hold_ms + (dialer ? 0 : policy.answer_ms) + RecoveryBackoffMs(attempts, policy);
	}

	struct LinkStatus {
		bool member = false; // still a room member? the control channel knows, and ICE does not
		bool up = false; // a link exists, juice has it connected, and it has not stalled
		uint32_t attempts = 0; // ICE rounds already spent on this peer
		bool dialer = false; // DialsFirst(): our id decides who redials and who answers
		uint64_t down_ms = 0; // how long the link has been anything other than up
	};

	inline LinkAction DecideLink(const LinkStatus& status, const RecoveryPolicy& policy)
	{
		if (!status.member)
			return LinkAction::Drop; // nothing to recover to

		if (status.up)
			return LinkAction::Healthy;

		if (status.attempts >= policy.max_attempts)
			return LinkAction::GiveUp;

		if (status.down_ms < policy.hold_ms)
			return LinkAction::Hold; // free repair first: the agent we have may pair up again by itself

		if (!status.dialer && status.down_ms < (uint64_t)policy.hold_ms + policy.answer_ms)
			return LinkAction::Answer;

		if (status.down_ms < RedialDueMs(status.attempts, status.dialer, policy))
			return LinkAction::Wait;

		return LinkAction::Redial;
	}
}
