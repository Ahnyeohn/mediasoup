#pragma once

#include "RTC/FrameRecord.hpp"
#include <fstream>
#include <mutex>
#include <string>

namespace RTC
{
	class FrameRecordCsvWriter
	{
	public:
		explicit FrameRecordCsvWriter(const std::string& filePath);
		~FrameRecordCsvWriter();

	public:
		void WriteRecord(const FrameRecord& record);

	private:
		void WriteHeaderIfNeeded();

	private:
		std::string filePath;
		std::ofstream out;
		std::mutex mutex;
		bool headerWritten{ false };
	};
} // namespace RTC