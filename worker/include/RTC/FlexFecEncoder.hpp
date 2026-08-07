#ifndef MS_RTC_FLEX_FEC_ENCODER_HPP
#define MS_RTC_FLEX_FEC_ENCODER_HPP

#include "modules/rtp_rtcp/source/forward_error_correction.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace RTC
{
	class FlexFecEncoder
	{
	public:
		struct GeneratedPayload
		{
			std::vector<uint8_t> data;
		};

	public:
		FlexFecEncoder(uint32_t flexFecSsrc, uint32_t mediaSsrc);

		// 최종 형태의 RTP 패킷 전체를 복사한다.
		bool AddMediaPacket(const uint8_t* data, size_t size);

		// 현재 저장된 미디어 패킷들에 대해 FlexFEC payload를 생성한다.
		bool Generate(
		  uint8_t protectionFactor,
		  std::vector<GeneratedPayload>& generatedPayloads);

		void Reset();

		size_t GetBufferedPacketCount() const
		{
			return this->mediaPackets.size();
		}

	private:
		uint32_t flexFecSsrc{ 0 };
		uint32_t mediaSsrc{ 0 };

		std::unique_ptr<webrtc::ForwardErrorCorrection> encoder;

		webrtc::ForwardErrorCorrection::PacketList mediaPackets;
	};
}

#endif