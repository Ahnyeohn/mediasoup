#include "RTC/FrameRecordCsvWriter.hpp"
#include <cmath>
#include <filesystem>
#include <iomanip>

namespace RTC
{
	FrameRecordCsvWriter::FrameRecordCsvWriter(const std::string& filePath) : filePath(filePath)
	{
		this->out.open(this->filePath, std::ios::out | std::ios::app);
		WriteHeaderIfNeeded();
	}

	FrameRecordCsvWriter::~FrameRecordCsvWriter()
	{
		if (this->out.is_open())
		{
			this->out.flush();
			this->out.close();
		}
	}

	void FrameRecordCsvWriter::WriteHeaderIfNeeded()
	{
		if (!this->out.is_open() || this->headerWritten)
		{
			return;
		}

		const bool fileExists = std::filesystem::exists(this->filePath);
		const auto fileSize   = fileExists ? std::filesystem::file_size(this->filePath) : 0;

		if (fileSize == 0)
		{
			this->out << "frameId, "
			          //   << "firstPacketSentAtMs,"
			          //   << "lastPacketSentAtMs,"
			          //   << "frameSizeBytes,"
			          //   << "packetCount,"
			          << "isKeyFrame, "
			          //  << "temporalLayer, "
			          //  << "spatialLayer,"
			          << "currentSpatialLayer, "
			          << "targetSpatialLayer, "
			          << "preferredSpatialLayer, "

			          << "rttMs, "
			          << "lossRate, "
			          << "availableBitrateBps, "
			          << "gccAvailableBitrateBps, "
			          << "camelAvailableBitrateBps, "

			          //  << "predictedSlackMs, "
			          << "receiveTimeMs, "
			          << "latestDecodeTimeMs, "
			          << "frameBufferInsertTimeMs, "
			          << "frameBufferExtractTimeMs, "
			          << "decodeQueueInsertTimeMs, "
			          << "decodeQueueExtractTimeMs, "

			          << "now, "
			          << "render_time, "
			          << "max_wait, "

			          << "decodeStartMs, "
			          << "decodeFinishMs, "
			          << "desiredReceiveTimeMs, "
			          << "desiredDecodeStartMs, "
			          << "actualSlackMs, "
			          << "actualSlackEffectiveMs, "
			          << "receiveSlackMs, "
			          << "decodeSlackNominalMs, "
			          // << "queueResidenceMs, "
			          << "frameBufferResidenceMs, "
			          << "decodeQueueResidenceMs, "
			          << "decodeSlackEffectiveMs, "
			          << "aceQueueBytes, "
			          << "pacingBacklogBytes, "
			          << "pacingBucketSizeBytes,"
			          << "camelBurstLengthBytes, "
			          << "pacing\n";
			//  << "errorMs\n";
			this->out.flush();
		}

		this->headerWritten = true;
	}

	void FrameRecordCsvWriter::WriteRecord(const FrameRecord& record)
	{
		std::lock_guard<std::mutex> lock(this->mutex);

		if (!this->out.is_open())
		{
			return;
		}

		WriteHeaderIfNeeded();

		double predicted = record.hasPredictedSlack ? record.predictedSlackMs : NAN;
		double actual    = record.hasSlack ? record.slackMs : NAN;
		double error     = (record.hasPredictedSlack && record.hasSlack)
		                     ? (record.predictedSlackMs - record.slackMs)
		                     : NAN;

		this->out
		  << record.frameId
		  << ", "
		  //   << record.firstPacketSentAtMs << ","
		  //   << record.lastPacketSentAtMs << ","
		  //   << record.frameSizeBytes << ","
		  //   << record.packetCount << ","
		  << (record.isKeyFrame ? 1 : 0) << ", "
		  //   << static_cast<uint32_t>(record.temporalLayer) << ","

		  //<< static_cast<uint32_t>(record.SpatialLayer) << ","
		  << record.currentSpatialLayer << ", " << record.targetSpatialLayer << ", "
		  << record.preferredSpatialLayer << ", "

		  << record.network.rttMs << ", " << record.network.lossRate << ", "
		  << record.network.availableBitratebps << ", " 
		  << record.network.gccAvailableBitrateBps << ", "
		  << record.network.camelAvailableBitrateBps << ", "
		  //  << predicted << ", "
		  << (record.hasReceiveTimeMs ? std::to_string(record.receiveTimeMs) : "") << ", "
		  << (record.hasLatestDecodeTimeMs ? std::to_string(record.latestDecodeTimeMs) : "") << ", "
		  << (record.hasFrameBufferInsertTimeMs ? std::to_string(record.frameBufferInsertTimeMs) : "")
		  << ", "
		  << (record.hasFrameBufferExtractTimeMs ? std::to_string(record.frameBufferExtractTimeMs) : "")
		  << ", "
		  << (record.hasDecodeQueueInsertTimeMs ? std::to_string(record.decodeQueueInsertTimeMs) : "")
		  << ", "
		  << (record.hasDecodeQueueExtractTimeMs ? std::to_string(record.decodeQueueExtractTimeMs) : "")
		  << ", "

		  << (record.hasnow ? std::to_string(record.now) : "") << ", "
		  << (record.hasrender_time ? std::to_string(record.render_time) : "") << ", "
		  << (record.hasmax_wait ? std::to_string(record.max_wait) : "") << ", "

		  << (record.hasDecodeStartMs ? std::to_string(record.decodeStartMs) : "") << ", "
		  << (record.hasDecodeFinishMs ? std::to_string(record.decodeFinishMs) : "") << ", "
		  << (record.hasDesiredReceiveTimeMs ? std::to_string(record.desiredReceiveTimeMs) : "") << ", "
		  << (record.hasDesiredDecodeStartMs ? std::to_string(record.desiredDecodeStartMs) : "") << ", "
		  << actual << ", "
		  << (record.hasActualSlackEffectiveMs ? std::to_string(record.actualSlackEffectiveMs) : "")
		  << ", " << (record.hasReceiveSlackMs ? std::to_string(record.receiveSlackMs) : "") << ", "
		  << (record.hasDecodeSlackNominalMs ? std::to_string(record.decodeSlackNominalMs) : "")
		  << ", "
		  //<< (record.hasQueueResidenceMs ? std::to_string(record.queueResidenceMs) : "") << ", "
		  << (record.hasFrameBufferResidenceMs ? std::to_string(record.frameBufferResidenceMs) : "")
		  << ", "
		  << (record.hasDecodeQueueResidenceMs ? std::to_string(record.decodeQueueResidenceMs) : "")
		  << ", "

		  << (record.hasDecodeSlackEffectiveMs ? std::to_string(record.decodeSlackEffectiveMs) : "")
		  << ", " << record.network.aceQueueBytes << ", " << record.network.pacingBacklogBytes << ", "
		  << record.network.pacingBucketSizeBytes << ", " << record.network.camelBurstLengthBytes
		  << ", " << record.pacingEnabled << "\n";

		//  << error << "\n";

		this->out.flush();
	}
} // namespace RTC