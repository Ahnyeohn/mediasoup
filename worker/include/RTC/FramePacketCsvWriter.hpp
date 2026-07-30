#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "RTC/FrameRecord.hpp"

namespace RTC
{
	class FramePacketCsvWriter
	{
	public:
		explicit FramePacketCsvWriter(const std::string& filePath);
		~FramePacketCsvWriter();

	public:
		void WritePacketReceiveTimes(
		  const std::string& transportId,
		  const std::string& consumerId,
		  const std::string& producerId,
		  uint32_t frameId,
		  const std::vector<PacketReceiveInfo>& packetReceiveTimes);

	private:
		void WriteHeaderIfNeeded();

	private:
		std::string filePath;
		std::ofstream out;
		std::mutex mutex;
		bool headerWritten{ false };
	};
} // namespace RTC