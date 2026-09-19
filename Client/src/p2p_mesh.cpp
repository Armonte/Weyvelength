#include "client.h"
#include "p2p_mesh.h"

#include <algorithm>
#include <cstring>

// The p2p half of the client: a lazily built mesh of libjuice links,
// signaled through the server as P2PSignal frames. A link that drops is
// recovered rather than mourned; link_recovery.h holds the rules and the
// reasoning, RecoverLinks below applies them once per poll.
namespace Weyvelength {

	// A link that is down still takes sends, and a redial can take seconds, so
	// the queue needs a lid: coming back up with a minute of stale traffic to
	// flush is worse for a real time application than having lost it.
	constexpr size_t max_queued_messages = 256;

	// Juice callbacks run on juice's threads; they only queue, Poll does the rest.
	static void PushJuiceEvent(juice_agent_t* agent, void* user_ptr, JuiceEvent ev)
	{
		auto* ctx = static_cast<JuiceCallbackContext*>(user_ptr);
		ev.agent = agent;
		ev.peer = ctx->peer;

		std::lock_guard lock(ctx->mesh->mutex);
		ctx->mesh->events.push_back(std::move(ev));
	}

	static void OnJuiceState(juice_agent_t* agent, juice_state_t state, void* user_ptr)
	{
		PushJuiceEvent(agent, user_ptr, { .kind = JuiceEvent::Kind::State, .state = state });
	}

	static void OnJuiceCandidate(juice_agent_t* agent, const char* sdp, void* user_ptr)
	{
		auto* bytes = (const std::byte*)sdp;
		PushJuiceEvent(agent, user_ptr, { .kind = JuiceEvent::Kind::Candidate, .payload = { bytes, bytes + std::strlen(sdp) } });
	}

	static void OnJuiceGatheringDone(juice_agent_t* agent, void* user_ptr)
	{
		PushJuiceEvent(agent, user_ptr, { .kind = JuiceEvent::Kind::GatheringDone });
	}

	static void OnJuiceRecv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr)
	{
		auto* bytes = (const std::byte*)data;
		PushJuiceEvent(agent, user_ptr, { .kind = JuiceEvent::Kind::Recv, .payload = { bytes, bytes + size } });
	}

	bool Client::SendP2P(uint32_t id, const Proto::P2PMessage& msg)
	{
		if (msg.empty() || msg.size() > Proto::max_p2p_message_size)
			return false;

		if (id == _id || std::ranges::find(_members, id) == _members.end())
			return false;

		PeerLink* link = FindLink(id);
		if (!link) {
			link = CreateLink(id, LinkState::Opening); // lazy: the first message to a peer starts ICE
			if (!link)
				return false;
			if (!ShareLink(*link, id)) {
				DestroyLink(id);
				return false;
			}
		}

		link->send_ms = _now_ms; // half of the stall rule: we only call a silent route dead while we talk to it

		if (!link->connected) {
			if (link->outbox.size() >= max_queued_messages)
				link->outbox.pop_front(); // keep the newest; a real time application wants the recent frames
			link->outbox.push_back(msg); // flushed when the link comes up
			return true;
		}

		return juice_send(link->agent, (const char*)msg.data(), msg.size()) == JUICE_ERR_SUCCESS;
	}

	bool Client::NextP2P(uint32_t& from, Proto::P2PMessage& out)
	{
		if (_p2p_inbox.empty())
			return false;

		from = _p2p_inbox.front().first;
		out = std::move(_p2p_inbox.front().second);
		_p2p_inbox.pop();
		return true;
	}

	bool Client::PeerConnectedP2P(uint32_t id) const
	{
		return PeerLinkState(id) == LinkState::Up;
	}

	// The recovery record exists for exactly as long as the link is not simply
	// up, and carries the reason, so the two answers come from one place.
	LinkState Client::PeerLinkState(uint32_t id) const
	{
		auto it = _mesh->recovery.find(id);
		if (it != _mesh->recovery.end())
			return it->second.state;

		return _mesh->links.contains(id) ? LinkState::Up : LinkState::None;
	}

	PeerLink* Client::FindLink(uint32_t id)
	{
		auto it = _mesh->links.find(id);
		return it == _mesh->links.end() ? nullptr : &it->second;
	}

	PeerLink* Client::CreateLink(uint32_t id, LinkState state)
	{
		LinkRecovery& recovery = _mesh->recovery[id]; // outlives the link, so the budget survives a redial
		if (recovery.attempts >= default_recovery_policy.max_attempts) {
			recovery.state = LinkState::GaveUp; // gave up on this peer until it reconnects or the room resets
			return nullptr;
		}

		recovery.state = state;
		recovery.since_ms = _now_ms; // the policy times this round out from here

		PeerLink link;
		link.ctx = std::make_unique<JuiceCallbackContext>();
		link.ctx->mesh = _mesh.get();
		link.ctx->peer = id;

		std::vector<juice_turn_server_t> turn;
		for (const Proto::TurnServer& relay : _ice.turn) {
			turn.push_back({ relay.host.c_str(), relay.username.c_str(), relay.password.c_str(), relay.port });
		}

		juice_config_t config{};
		config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL; // every link shares one juice thread
		if (!_ice.stun_host.empty()) {
			config.stun_server_host = _ice.stun_host.c_str();
			config.stun_server_port = _ice.stun_port;
		}
		config.turn_servers = turn.empty() ? nullptr : turn.data();
		config.turn_servers_count = (int)turn.size();
		config.cb_state_changed = OnJuiceState;
		config.cb_candidate = OnJuiceCandidate;
		config.cb_gathering_done = OnJuiceGatheringDone;
		config.cb_recv = OnJuiceRecv;
		config.user_ptr = link.ctx.get();

		link.agent = juice_create(&config);
		if (!link.agent)
			return nullptr;

		recovery.attempts++; // count only agents that actually started an ICE round
		return &_mesh->links.emplace(id, std::move(link)).first->second;
	}

	// Sends the local description; gathering then trickles the candidates.
	bool Client::ShareLink(PeerLink& link, uint32_t id)
	{
		char sdp[JUICE_MAX_SDP_STRING_LEN];
		if (juice_get_local_description(link.agent, sdp, sizeof(sdp)) != JUICE_ERR_SUCCESS)
			return false;

		SendServer(Proto::P2PSignal{ id, Proto::P2PSignalKind::Description, sdp });
		return juice_gather_candidates(link.agent) == JUICE_ERR_SUCCESS;
	}

	void Client::FlushLink(PeerLink& link)
	{
		for (const std::vector<std::byte>& data : link.outbox) {
			juice_send(link.agent, (const char*)data.data(), data.size());
		}
		link.outbox.clear();
	}

	bool Client::LinkStalled(const PeerLink& link) const
	{
		return RouteStalled(_now_ms - link.recv_ms, _now_ms - link.send_ms, link.heard, default_recovery_policy);
	}

	// One clock per peer, started at the first sign of trouble and carried
	// across redials: a second report must not push the deadline back.
	void Client::MarkLinkDown(uint32_t id)
	{
		auto [it, fresh] = _mesh->recovery.try_emplace(id);
		if (fresh) {
			it->second.since_ms = _now_ms;
			it->second.state = LinkState::Recovering;
		}
	}

	// The escalation, and the expensive half of recovery. libjuice has no ICE
	// restart, so a route that is really gone can only be replaced by a whole
	// new agent; what we do still have is the signaling channel, which is TCP
	// and never dropped, so this is a re-signal and not a reconnect.
	void Client::RedialLink(uint32_t id)
	{
		// A link that has never been up is still opening, however many rounds it takes.
		const LinkState state = PeerLinkState(id) == LinkState::Opening ? LinkState::Opening : LinkState::Recovering;

		DestroyLink(id);

		PeerLink* link = CreateLink(id, state);
		if (!link)
			return; // out of budget; CreateLink has already recorded that

		if (!ShareLink(*link, id))
			DestroyLink(id); // the round is spent either way; the next sweep waits out its backoff
	}

	void Client::DestroyLink(uint32_t id)
	{
		auto it = _mesh->links.find(id);
		if (it == _mesh->links.end())
			return;

		juice_destroy(it->second.agent); // joins the callbacks, so the ctx is safe to free
		_mesh->links.erase(it);
	}

	void Client::DestroyAllLinks()
	{
		for (auto& [id, link] : _mesh->links) {
			juice_destroy(link.agent);
		}
		_mesh->links.clear();
		_mesh->recovery.clear();
	}

	void Client::HandleP2PSignal(const Proto::P2PSignal& sig)
	{
		PeerLink* link = FindLink(sig.id);

		switch (sig.kind) {
		case Proto::P2PSignalKind::Description:
			HandleP2PDescription(link, sig);
			break;
		case Proto::P2PSignalKind::Candidate:
			if (link)
				juice_add_remote_candidate(link->agent, sig.payload.c_str());
			break;
		case Proto::P2PSignalKind::GatheringDone:
			if (link)
				juice_set_remote_gathering_done(link->agent);
			break;
		}
	}

	void Client::HandleP2PDescription(PeerLink* link, const Proto::P2PSignal& sig)
	{
		// a second description on a link that already spent its one
		// juice_set_remote_description is the peer redialing with a fresh agent
		// (theirs died, ours survived); ours is pinned to the old credentials,
		// so tear it down and answer the new one
		if (link && link->remote_set) {
			DestroyLink(sig.id);
			link = nullptr;
		}

		bool answering = !link;
		if (answering) {
			_mesh->recovery.erase(sig.id); // an inbound dial proves the peer is alive; answering is always allowed
			link = CreateLink(sig.id, LinkState::Opening); // a peer reached out with no link of ours yet
			if (!link)
				return;
		}

		if (!link->remote_set) { // glare aside, apply the remote description once
			juice_set_remote_description(link->agent, sig.payload.c_str());
			link->remote_set = true;
		}

		if (answering)
			ShareLink(*link, sig.id); // remote set first = we take the controlled role
	}

	void Client::PollPeers()
	{
		std::vector<JuiceEvent>& events = _mesh->scratch;
		{
			std::lock_guard lock(_mesh->mutex);
			events.swap(_mesh->events);
		}

		for (JuiceEvent& ev : events) {
			HandleJuiceEvent(ev);
		}
		events.clear(); // keeps its capacity for the next poll

		RecoverLinks();
	}

	// Walks every peer whose link is not simply up and applies the policy. The
	// verdicts that do nothing are the point: most drops are repaired by juice
	// or by the peer, and a redial we did not have to make is a stall the
	// application never saw.
	void Client::RecoverLinks()
	{
		std::vector<uint32_t>& peers = _mesh->sweep;
		peers.clear();
		for (const auto& [id, link] : _mesh->links) {
			peers.push_back(id);
		}
		for (const auto& [id, recovery] : _mesh->recovery) {
			if (!_mesh->links.contains(id))
				peers.push_back(id); // the record outlives the agent we gave up on
		}

		for (uint32_t id : peers) {
			const PeerLink* link = FindLink(id);
			const bool up = link && link->connected && !LinkStalled(*link);
			if (!up)
				MarkLinkDown(id); // a stall starts the clock here; juice would not call it for another 30 s

			auto recovery = _mesh->recovery.find(id);
			if (recovery == _mesh->recovery.end())
				continue; // up, and nothing was ever wrong with it

			LinkStatus status;
			status.member = std::ranges::find(_members, id) != _members.end();
			status.up = up;
			status.attempts = recovery->second.attempts;
			status.dialer = DialsFirst(_id, id);
			status.down_ms = _now_ms - recovery->second.since_ms;

			switch (DecideLink(status, default_recovery_policy)) {
			case LinkAction::Healthy:
				_mesh->recovery.erase(recovery); // it came back on its own; nothing was spent
				break;
			case LinkAction::Redial:
				RedialLink(id);
				break;
			case LinkAction::GiveUp:
				DestroyLink(id); // the agent is dead weight now; the record keeps refusing the peer
				recovery->second.state = LinkState::GaveUp;
				break;
			case LinkAction::Drop:
				DestroyLink(id);
				_mesh->recovery.erase(recovery); // not a member any more; there is nothing to reach
				break;
			case LinkAction::Hold:
			case LinkAction::Answer:
			case LinkAction::Wait:
				break; // juice, or the peer, or the backoff, is on it
			}
		}
	}

	void Client::HandleJuiceEvent(JuiceEvent& ev)
	{
		PeerLink* link = FindLink(ev.peer);
		if (!link || link->agent != ev.agent)
			return; // the link was torn down (or rebuilt) after this event was queued

		switch (ev.kind) {
		case JuiceEvent::Kind::State:
			HandleLinkState(*link, ev);
			break;
		case JuiceEvent::Kind::Candidate:
			SendServer(Proto::P2PSignal{ ev.peer, Proto::P2PSignalKind::Candidate, std::string{ (const char*)ev.payload.data(), ev.payload.size() } });
			break;
		case JuiceEvent::Kind::GatheringDone:
			SendServer(Proto::P2PSignal{ ev.peer, Proto::P2PSignalKind::GatheringDone, {} });
			break;
		case JuiceEvent::Kind::Recv:
			link->heard = true; // the other half of the stall rule: only a route that answered can go silent
			link->recv_ms = _now_ms;
			_p2p_inbox.emplace(ev.peer, std::move(ev.payload));
			break;
		}
	}

	void Client::HandleLinkState(PeerLink& link, const JuiceEvent& ev)
	{
		switch (ev.state) {
		case JUICE_STATE_CONNECTED:
		case JUICE_STATE_COMPLETED:
			link.connected = true;
			link.recv_ms = _now_ms; // a route that has just come up has not gone silent
			_mesh->recovery.erase(ev.peer); // success clears the budget; a later drop retries fresh
			FlushLink(link);
			break;
		case JUICE_STATE_DISCONNECTED:
			link.connected = false; // may still recover; sends queue meanwhile
			MarkLinkDown(ev.peer);
			break;
		case JUICE_STATE_FAILED:
			// The agent is not destroyed here any more. juice keeps reading its
			// socket after a failure and keeps learning peer reflexive candidates
			// from the peer's checks, so a link whose route moved can pair up
			// again on its own; RecoverLinks holds it open for that first.
			link.connected = false;
			MarkLinkDown(ev.peer);
			break;
		default:
			break;
		}
	}
}
