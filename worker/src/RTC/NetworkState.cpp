#include "RTC/NetworkState.hpp"

namespace RTC
{
	void NetworkState::UpdateRttMs(double rttMs, uint64_t nowMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->snapshot.rttMs      = rttMs;
		this->snapshot.updatedAtMs = nowMs;
	}

	void NetworkState::UpdateLossRate(double lossRate, uint64_t nowMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->snapshot.lossRate   = lossRate;
		this->snapshot.updatedAtMs = nowMs;
	}

	void NetworkState::UpdateTwccDelayTrend(double trend, uint64_t nowMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->snapshot.twccDelayTrend = trend;
		this->snapshot.updatedAtMs    = nowMs;
	}

	void NetworkState::UpdateAvailableBitrateBps(double bitrateBps, uint64_t nowMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->snapshot.availableBitratebps = bitrateBps;
		this->snapshot.updatedAtMs         = nowMs;
	}

	void NetworkState::UpdateAceQueueBytes(double queueBytes, uint64_t nowMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->snapshot.aceQueueBytes = queueBytes;
		this->snapshot.updatedAtMs   = nowMs;
	}

	void NetworkState::UpdatePacingBacklogBytes(double backlogBytes, uint64_t nowMs)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->snapshot.pacingBacklogBytes = backlogBytes;
		this->snapshot.updatedAtMs        = nowMs;
	}

	NetworkSnapshot NetworkState::GetSnapshot() const
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->snapshot;
	}
} // namespace RTC