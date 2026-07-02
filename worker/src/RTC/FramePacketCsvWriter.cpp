#include "RTC/FramePacketCsvWriter.hpp"

#include <filesystem>

namespace RTC
{
	FramePacketCsvWriter::FramePacketCsvWriter(const std::string& filePath) : filePath(filePath)
	{
		this->out.open(this->filePath, std::ios::out | std::ios::app);
		WriteHeaderIfNeeded();
	}

	FramePacketCsvWriter::~FramePacketCsvWriter()
	{
		if (this->out.is_open())
		{
			this->out.flush();
			this->out.close();
		}
	}

	void FramePacketCsvWriter::WriteHeaderIfNeeded()
	{
		if (!this->out.is_open() || this->headerWritten)
		{
			return;
		}

		const bool fileExists = std::filesystem::exists(this->filePath);
		const auto fileSize   = fileExists ? std::filesystem::file_size(this->filePath) : 0;

		if (fileSize == 0)
		{
			this->out << "frameId,sequenceNumber,receiveTimeMs\n";
			this->out.flush();
		}

		this->headerWritten = true;
	}

	void FramePacketCsvWriter::WritePacketReceiveTimes(
	  uint32_t frameId, const std::vector<PacketReceiveInfo>& packetReceiveTimes)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (!this->out.is_open())
		{
			return;
		}

		if (packetReceiveTimes.empty())
		{
			return;
		}

		WriteHeaderIfNeeded();

		for (const auto& packet : packetReceiveTimes)
		{
			this->out
			  << frameId << ","
			  << packet.sequenceNumber << ","
			  << packet.receiveTimeMs << "\n";
		}

		this->out.flush();
	}
} // namespace RTC