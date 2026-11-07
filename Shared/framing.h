#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <thirdparty/zpp_bits/zpp_bits.h>

#include "protocol.h"

// Wire framing shared by both ends of the server connection: a message is
// encoded as one or more [uint32 header][fragment] frames. The header's low
// bits are the fragment byte count (<= max_message_size); the high bit means
// more fragments of the same message follow. A message that fits in one frame
// keeps the top bit clear, so single-fragment traffic is byte-for-byte the
// plain length prefix. Transport (asio coroutines on the server, polled
// sockets on the client) differs, but this byte layout does not.
namespace Weyvelength::Proto {
	constexpr size_t frame_header_size = sizeof(uint32_t);
	constexpr uint32_t frame_more_flag = 0x80000000u; // header high bit: another fragment follows
	constexpr size_t max_reassembled_size = 64 * max_message_size; // cap on one reassembled message; sized for the room list

	// Encodes a message as one or more length-prefixed fragments, each within
	// max_message_size, ready to write to a socket back to back.
	inline std::vector<std::byte> FrameMessage(const ServerMessage& msg)
	{
		std::vector<std::byte> body;
		zpp::bits::out{ body }(msg).or_throw();

		std::vector<std::byte> frames;
		size_t offset = 0;
		do {
			uint32_t chunk = (uint32_t)std::min<size_t>(max_message_size, body.size() - offset);
			bool more = offset + chunk < body.size();

			std::vector<std::byte> header;
			zpp::bits::out{ header }(chunk | (more ? frame_more_flag : 0u)).or_throw();
			frames.insert(frames.end(), header.begin(), header.end());
			frames.insert(frames.end(), body.begin() + offset, body.begin() + offset + chunk);
			offset += chunk;
		} while (offset < body.size());

		return frames;
	}

	// Decodes one reassembled message. zpp_bits reads a string/vector's wire
	// length and resizes the container to it BEFORE checking that those bytes
	// exist, so without a limit a ~20-byte frame claiming a 4 GB string makes
	// the receiver allocate (and zero) 4 GB per field. No container in a
	// message can legitimately exceed the reassembled message, so that is the
	// allocation cap. Every decode of untrusted bytes goes through here.
	template <typename Message>
	inline bool DecodeMessage(std::span<const std::byte> body, Message& msg)
	{
		return !failure(zpp::bits::in{ body, zpp::bits::alloc_limit<max_reassembled_size>() }(msg));
	}

	// Splits a frame header into its fragment length and more-fragments flag.
	// False if the bytes are unreadable or the length exceeds max_message_size.
	inline bool DecodeFrameHeader(std::span<const std::byte> header, uint32_t& len, bool& more)
	{
		uint32_t raw;
		if (failure(zpp::bits::in{ header }(raw)))
			return false;
		more = (raw & frame_more_flag) != 0;
		len = raw & ~frame_more_flag;
		return len <= max_message_size;
	}

	// Appends a fragment payload to a message being reassembled, guarding the
	// total against max_reassembled_size. False = the message is oversized.
	inline bool AppendFragment(std::vector<std::byte>& body, std::span<const std::byte> payload)
	{
		if (body.size() + payload.size() > max_reassembled_size)
			return false;
		body.insert(body.end(), payload.begin(), payload.end());
		return true;
	}
}
