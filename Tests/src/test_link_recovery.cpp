#include <thirdparty/doctest/doctest.h>

#include <cstdint>

#include "link_recovery.h"
#include "weyvelength.h"

using namespace Weyvelength;

// The C enum is a plain cast of LinkState in weyvelength.cpp, so the two have
// to agree value for value; inserting a state in one only fails right here.
static_assert((int)WEYVE_LINK_NONE == (int)LinkState::None);
static_assert((int)WEYVE_LINK_OPENING == (int)LinkState::Opening);
static_assert((int)WEYVE_LINK_UP == (int)LinkState::Up);
static_assert((int)WEYVE_LINK_RECOVERING == (int)LinkState::Recovering);
static_assert((int)WEYVE_LINK_GAVE_UP == (int)LinkState::GaveUp);

// The budget the mesh spends is the one the policy hands out.
static_assert(default_recovery_policy.max_attempts == max_connection_attempts);
static_assert(default_recovery_policy.hold_ms < default_recovery_policy.stall_ms);
static_assert(default_recovery_policy.backoff_ms <= default_recovery_policy.backoff_max_ms);

namespace {
	constexpr RecoveryPolicy policy = default_recovery_policy;

	// A link that is down, ours to redial, with nothing spent on it yet.
	LinkStatus Down(uint64_t down_ms)
	{
		LinkStatus status;
		status.member = true;
		status.up = false;
		status.attempts = 0;
		status.dialer = true;
		status.down_ms = down_ms;
		return status;
	}
}

TEST_CASE("a healthy link is nobody's business")
{
	LinkStatus status = Down(0);
	status.up = true;
	CHECK(DecideLink(status, policy) == LinkAction::Healthy);

	// Even a link that spent its whole budget getting there is done recovering.
	status.attempts = policy.max_attempts;
	status.down_ms = 60'000;
	CHECK(DecideLink(status, policy) == LinkAction::Healthy);
}

TEST_CASE("a peer that left the room is dropped, whatever the link was doing")
{
	LinkStatus status = Down(0);
	status.member = false;
	CHECK(DecideLink(status, policy) == LinkAction::Drop);

	status.up = true;
	CHECK(DecideLink(status, policy) == LinkAction::Drop);

	status.up = false;
	status.attempts = policy.max_attempts;
	CHECK(DecideLink(status, policy) == LinkAction::Drop);
}

TEST_CASE("a fresh drop is held, not redialed")
{
	// The cheapest repair is the one juice makes by itself, so nothing is spent
	// until the hold window is over.
	CHECK(DecideLink(Down(0), policy) == LinkAction::Hold);
	CHECK(DecideLink(Down(policy.hold_ms - 1), policy) == LinkAction::Hold);
	CHECK(DecideLink(Down(policy.hold_ms), policy) != LinkAction::Hold);
}

TEST_CASE("the lower id dials and the higher id answers")
{
	CHECK(DialsFirst(7, 9));
	CHECK_FALSE(DialsFirst(9, 7));

	LinkStatus dialer = Down(policy.hold_ms);
	LinkStatus answerer = dialer;
	answerer.dialer = false;

	// Past the hold window the two sides part: one waits out its backoff, the
	// other deliberately stands down so the redial only happens once.
	CHECK(DecideLink(dialer, policy) == LinkAction::Wait);
	CHECK(DecideLink(answerer, policy) == LinkAction::Answer);

	dialer.down_ms = RedialDueMs(0, true, policy);
	CHECK(DecideLink(dialer, policy) == LinkAction::Redial);

	// The answerer is still standing down at the moment the dialer redials.
	answerer.down_ms = dialer.down_ms;
	CHECK(DecideLink(answerer, policy) != LinkAction::Redial);
}

TEST_CASE("the answering side redials anyway if the dial never comes")
{
	// Only one side may have seen the drop, so standing down cannot be forever.
	LinkStatus answerer = Down(0);
	answerer.dialer = false;

	answerer.down_ms = (uint64_t)policy.hold_ms + policy.answer_ms;
	CHECK(DecideLink(answerer, policy) == LinkAction::Wait);

	answerer.down_ms = RedialDueMs(0, false, policy);
	CHECK(DecideLink(answerer, policy) == LinkAction::Redial);
	CHECK(RedialDueMs(0, false, policy) > RedialDueMs(0, true, policy));
}

TEST_CASE("the backoff doubles per round spent and then stops")
{
	CHECK(RecoveryBackoffMs(0, policy) == policy.backoff_ms);
	CHECK(RecoveryBackoffMs(1, policy) == policy.backoff_ms * 2);

	uint32_t previous = 0;
	for (uint32_t attempts = 0; attempts < 40; ++attempts) {
		const uint32_t backoff = RecoveryBackoffMs(attempts, policy);
		CHECK(backoff >= previous); // monotonic, and no overflow past the cap
		CHECK(backoff <= policy.backoff_max_ms);
		previous = backoff;
	}
	CHECK(previous == policy.backoff_max_ms);
}

TEST_CASE("the budget is spent, then the peer is given up on")
{
	LinkStatus status = Down(60'000);
	status.attempts = policy.max_attempts - 1;
	CHECK(DecideLink(status, policy) == LinkAction::Redial);

	status.attempts = policy.max_attempts;
	CHECK(DecideLink(status, policy) == LinkAction::GiveUp);

	// Giving up outranks every wait: no amount of holding brings it back.
	status.down_ms = 0;
	CHECK(DecideLink(status, policy) == LinkAction::GiveUp);
}

TEST_CASE("recovery cannot run forever")
{
	// Walk the machine the way the mesh does: hold, wait, redial, repeat. It
	// has to reach GiveUp, and it may only redial max_attempts times to get there.
	const bool roles[] = { true, false };
	for (bool dialer : roles) {
		LinkStatus status = Down(0);
		status.dialer = dialer;

		uint32_t redials = 0;
		uint64_t elapsed = 0;
		LinkAction action = LinkAction::Hold;

		for (uint32_t tick = 0; tick < 4000 && action != LinkAction::GiveUp; ++tick) {
			status.down_ms = elapsed;
			action = DecideLink(status, policy);
			if (action == LinkAction::Redial) {
				++redials;
				status.attempts++;
				elapsed = 0; // a redial restarts the clock, exactly as CreateLink does
				continue;
			}
			elapsed += 100;
		}

		CHECK(action == LinkAction::GiveUp);
		CHECK(redials == policy.max_attempts);
	}
}

TEST_CASE("a quiet link is only called dead while we are talking to it")
{
	const uint64_t quiet = policy.stall_ms;
	const uint64_t recent = policy.stall_ms - 1;

	// Silence under our own traffic, from a peer that has answered before.
	CHECK(RouteStalled(quiet, recent, true, policy));

	// An application that sends nothing is not evidence of anything.
	CHECK_FALSE(RouteStalled(quiet, quiet, true, policy));

	// A peer that has never sent us a byte may simply be a listener; that one
	// keeps juice's own, slower verdict rather than ours.
	CHECK_FALSE(RouteStalled(quiet, recent, false, policy));

	// Traffic came back: the route is fine and the drop clock never starts.
	CHECK_FALSE(RouteStalled(recent, recent, true, policy));
}

TEST_CASE("every action has a name")
{
	const LinkAction actions[] = { LinkAction::Healthy, LinkAction::Hold, LinkAction::Answer,
		LinkAction::Wait, LinkAction::Redial, LinkAction::GiveUp, LinkAction::Drop };

	for (LinkAction action : actions) {
		CHECK(doctest::String(LinkActionName(action)) != doctest::String("?"));
	}
}
