#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tg {
using Clock = std::chrono::steady_clock;
enum class EventKind { Collected, NamedPlayback, ContentPlayback };
struct EventItem {
	int context{};
	std::uint64_t generation{}, epoch{};
	Clock::time_point created;
	std::string payload;
	EventKind kind = EventKind::Collected;
};
struct EventBatch {
	bool reset{};
	std::string reason;
	std::vector<EventItem> events;
};

// VM pointers are identity tokens only. Never dereferenced or given to the worker.
class EventQueue {
	struct Vm { void* token{}; std::uint64_t generation{}; };
	std::array<Vm, 2> vms_{};
	std::deque<EventItem> items_;
	mutable std::mutex mutex_;
	std::condition_variable ready_;
	std::uint64_t epoch_{};
	std::size_t capacity_;
	bool reset_{};
	std::string reason_;
	void Invalidate(const char* reason) {
		++epoch_;
		items_.clear();
		reset_ = true;
		reason_ = std::string(reason ? reason : "reset").substr(0, 128);
		ready_.notify_one();
	}
public:
	explicit EventQueue(std::size_t capacity = 256) : capacity_(capacity) {}
	void SetCapacity(std::size_t capacity) {
		std::lock_guard lock(mutex_);
		capacity_ = capacity;
		while (items_.size() > capacity_) items_.pop_front();
	}
	void Begin(int context, void* token) {
		if (context < 0 || context > 1 || !token) return;
		std::lock_guard lock(mutex_);
		auto& vm = vms_[context];
		if (vm.token == token) return;
		vm.token = token;
		++vm.generation;
		Invalidate("VM created");
	}
	void End(int context, void* token) {
		if (context < 0 || context > 1 || !token) return;
		std::lock_guard lock(mutex_);
		auto& vm = vms_[context];
		if (vm.token != token) return;
		vm.token = nullptr;
		++vm.generation;
		Invalidate("VM destroyed");
	}
	void Reset(int context, const char* reason) {
		std::lock_guard lock(mutex_);
		if (context != -1 && (context < 0 || context > 1 || !vms_[context].token)) return;
		Invalidate(reason);
	}
	// The output worker has already stopped owned effects. Reset the boundary
	// atomically, renewing only this current witness without scheduling a second
	// cleanup that would stop its new one-shot. Never extend its original age.
	bool ResetForPlayback(const EventItem& source, EventItem& renewed) {
		std::lock_guard lock(mutex_);
		if (source.context < 0 || source.context > 1 || !vms_[source.context].token ||
			vms_[source.context].generation != source.generation || source.epoch != epoch_) return false;
		Invalidate("playback lifecycle boundary");
		reset_ = false;
		reason_.clear();
		renewed = source;
		renewed.epoch = epoch_;
		return true;
	}
	bool Push(int context, std::string payload, Clock::time_point created = Clock::now(), EventKind kind = EventKind::Collected) {
		if (context < 0 || context > 1 || payload.empty() || payload.size() > 32768) return false;
		std::lock_guard lock(mutex_);
		const auto& vm = vms_[context];
		if (!vm.token || !capacity_) return false;
		if (items_.size() >= capacity_) items_.pop_front();
		items_.push_back({context, vm.generation, epoch_, created, std::move(payload), kind});
		ready_.notify_one();
		return true;
	}
	EventBatch Next(std::chrono::milliseconds wait) {
		std::unique_lock lock(mutex_);
		ready_.wait_for(lock, wait, [&] { return reset_ || !items_.empty(); });
		EventBatch result;
		result.reset = reset_;
		result.reason = std::move(reason_);
		reset_ = false;
		while (!items_.empty() && result.events.size() < 32) {
			result.events.push_back(std::move(items_.front()));
			items_.pop_front();
		}
		return result;
	}
	bool Current(const EventItem& item) const {
		std::lock_guard lock(mutex_);
		return item.context >= 0 && item.context <= 1 && vms_[item.context].token &&
			vms_[item.context].generation == item.generation && item.epoch == epoch_;
	}
	bool SameGeneration(const EventItem& item) const {
		std::lock_guard lock(mutex_);
		return item.context >= 0 && item.context <= 1 && vms_[item.context].token &&
			vms_[item.context].generation == item.generation;
	}
	bool Active() const {
		std::lock_guard lock(mutex_);
		return vms_[0].token || vms_[1].token;
	}
};
}
