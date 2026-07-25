#pragma once

#include <cstdint>
#include <mutex>

namespace RTC
{
	struct NetworkSnapshot
	{
		double rttMs{ 0.0 };
		double lossRate{ 0.0 }; // 0.0 ~ 1.0
		double twccDelayTrend{ 0.0 };
		double availableBitratebps{ 0.0 };

		// 추가
		uint32_t gccAvailableBitrateBps{ 0u };
		uint32_t camelAvailableBitrateBps{ 0u };

		double aceQueueBytes{ 0.0 };
		double pacingBacklogBytes{ 0.0 };
		double pacingBucketSizeBytes{ 0.0 }; // yeon: ACE/token bucket size

		uint64_t updatedAtMs{ 0 };

		// yeon: Camel burst length controller output.
		uint32_t camelBurstLengthBytes{ 0u };
	};

	class NetworkState
	{
	public:
		NetworkState()  = default;
		~NetworkState() = default;

	public:
		void UpdateRttMs(double rttMs, uint64_t nowMs);
		void UpdateLossRate(double lossRate, uint64_t nowMs);
		void UpdateTwccDelayTrend(double trend, uint64_t nowMs);
		void UpdateAvailableBitrateBps(double bitrateBps, uint64_t nowMs);
		void UpdateAceQueueBytes(double queueBytes, uint64_t nowMs);
		void UpdatePacingBacklogBytes(double backlogBytes, uint64_t nowMs);

		NetworkSnapshot GetSnapshot() const;

	private:
		mutable std::mutex mutex;
		NetworkSnapshot snapshot;
	};
} // namespace RTC