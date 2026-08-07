#define MS_CLASS "RTC::FlexFecEncoder"

#include "RTC/FlexFecEncoder.hpp"
#include "Logger.hpp"
#include "MediaSoupErrors.hpp"

#include <cstring>
#include <limits>
#include <list>
#include <utility>

#include <inttypes.h>

namespace RTC
{
	FlexFecEncoder::FlexFecEncoder(uint32_t flexFecSsrc, uint32_t mediaSsrc)
	  : flexFecSsrc(flexFecSsrc), mediaSsrc(mediaSsrc)
	{
		MS_TRACE();

		this->encoder =
		  webrtc::ForwardErrorCorrection::CreateFlexfec(flexFecSsrc, mediaSsrc);

		if (!this->encoder)
		{
			MS_THROW_ERROR(
			  "failed to create FlexFEC encoder [mediaSsrc:%" PRIu32
			  ", flexFecSsrc:%" PRIu32 "]",
			  mediaSsrc,
			  flexFecSsrc);
		}
	}

	bool FlexFecEncoder::AddMediaPacket(const uint8_t* data, size_t size)
	{
		MS_TRACE();

		if (!data)
		{
			MS_WARN_TAG(rtp, "null media RTP data");

			return false;
		}

		// 최소 RTP 고정 헤더 크기.
		if (size < 12u)
		{
			MS_WARN_TAG(rtp, "media RTP packet is too small [size:%zu]", size);

			return false;
		}

		// 현재 가져온 M77 FEC Packet은 IP_PACKET_SIZE 크기의 고정 배열을 사용한다.
		if (size > IP_PACKET_SIZE)
		{
			MS_WARN_TAG(
			  rtp,
			  "media RTP packet exceeds FEC buffer [size:%zu, max:%u]",
			  size,
			  static_cast<unsigned int>(IP_PACKET_SIZE));

			return false;
		}

		if (size > std::numeric_limits<uint16_t>::max())
		{
			return false;
		}

		// WebRTC packet mask가 한 번에 보호할 수 있는 최대 미디어 패킷 수.
		if (this->mediaPackets.size() >= 48u)
		{
			MS_WARN_TAG(
			  rtp,
			  "too many media packets in one FEC block [count:%zu]",
			  this->mediaPackets.size());

			return false;
		}

		auto mediaPacket =
		  std::make_unique<webrtc::ForwardErrorCorrection::Packet>();

		mediaPacket->length = static_cast<uint16_t>(size);

		std::memcpy(mediaPacket->data, data, size);

		this->mediaPackets.emplace_back(std::move(mediaPacket));

		return true;
	}

	bool FlexFecEncoder::Generate(
	  uint8_t protectionFactor,
	  std::vector<GeneratedPayload>& generatedPayloads)
	{
		MS_TRACE();

		generatedPayloads.clear();

		if (this->mediaPackets.empty())
		{
			return true;
		}

		std::list<webrtc::ForwardErrorCorrection::Packet*> fecPackets;

		const int result = this->encoder->EncodeFec(
		  this->mediaPackets,
		  protectionFactor,
		  0,                       // numImportantPackets
		  false,                   // useUnequalProtection
		  webrtc::kFecMaskRandom,  // 랜덤 손실용 packet mask
		  &fecPackets);

		if (result != 0)
		{
			MS_WARN_TAG(
			  rtp,
			  "EncodeFec failed [mediaPackets:%zu, protectionFactor:%u]",
			  this->mediaPackets.size(),
			  static_cast<unsigned int>(protectionFactor));

			this->Reset();

			return false;
		}

		generatedPayloads.reserve(fecPackets.size());

		// fecPackets 내부 메모리는 다음 EncodeFec() 호출 때 덮어써질 수 있으므로
		// 반드시 여기서 mediasoup 소유 메모리로 복사한다.
		for (const auto* fecPacket : fecPackets)
		{
			if (!fecPacket || fecPacket->length == 0u)
			{
				continue;
			}

			GeneratedPayload generatedPayload;

			generatedPayload.data.assign(
			  fecPacket->data,
			  fecPacket->data + fecPacket->length);

			generatedPayloads.emplace_back(std::move(generatedPayload));
		}

		MS_DEBUG_TAG(
		  rtp,
		  "generated FlexFEC payloads "
		  "[mediaPackets:%zu, fecPackets:%zu, protectionFactor:%u]",
		  this->mediaPackets.size(),
		  generatedPayloads.size(),
		  static_cast<unsigned int>(protectionFactor));

		this->Reset();

		return true;
	}

	void FlexFecEncoder::Reset()
	{
		MS_TRACE();

		this->mediaPackets.clear();
	}
}