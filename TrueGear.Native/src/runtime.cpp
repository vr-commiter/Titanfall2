#include "runtime.hpp"
#include "event_queue.hpp"
#include "event_router.hpp"
#include "vr_rumble_filter.hpp"
#include "truegear_player.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <thread>
#include <ctime>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>

namespace tg {
namespace {
std::int64_t Milliseconds() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

class Runtime {
	EventQueue queue_;
	std::filesystem::path directory_;
	LogCallback callback_{};
	std::mutex log_mutex_;
	std::ofstream log_;
	Json configuration_;
	Json body_slot_effects_ = Json::object();
	bool configured_{};
	std::unique_ptr<EventRouter> router_;
	std::unique_ptr<TrueGearPlayer> player_;
	std::set<std::string> played_events_;
	std::set<std::string> power_lookups_;
	struct ContinuousPlayback {
		EventItem lease;
		std::string uuid;
		Clock::time_point next_play;
		std::chrono::milliseconds interval;
	};
	std::map<std::string, ContinuousPlayback> continuous_;
	std::set<std::string> pending_continuous_stops_;
	bool player_dead_{};
	std::optional<EventItem> death_owner_;
	Clock::time_point next_connect_{};
	std::size_t max_age_ms_ = 250;
	int reconnect_ms_ = 2000;
	bool dry_run_{}, enabled_{}, log_events_{};

	void LogPlayback(const std::string& event, const std::string& uuid, const char* mode) {
		if (!log_events_ || dry_run_) return;
		const auto text = "PLAY event=" + event + " uuid=" + uuid + " mode=" + mode;
		std::lock_guard lock(log_mutex_);
		if (log_) {
			const auto now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			char stamp[32]{};
			std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
			log_ << stamp << " [0] " << text << '\n';
			log_.flush();
		}
		if (callback_) callback_(0, text.c_str());
	}
	void Configure() {
		try {
			std::filesystem::create_directories(directory_);
			{
				std::lock_guard lock(log_mutex_);
				// Configure runs once for this process. Keep its VM/reset history,
				// while replacing the preceding game process's accumulated log.
				log_.open(directory_ / L"native.log", std::ios::out | std::ios::trunc);
			}
			const auto path = directory_ / L"titanfall2-truegear.json";
			configuration_ = DefaultConfig();
			if (std::filesystem::exists(path)) {
				std::ifstream input(path, std::ios::binary);
				if (!input || std::filesystem::file_size(path) > 1024 * 1024)
					throw std::runtime_error("configuration unreadable or over 1 MiB");
				input >> configuration_;
			} else {
				std::ofstream output(path, std::ios::binary);
				output << configuration_.dump(2) << '\n';
			}
			// Older user mappings gain only the new independent death route.
			// An explicit disabled or remapped route remains authoritative.
			if (configuration_.is_object() && configuration_.contains("routes") &&
				configuration_["routes"].is_object() && !configuration_["routes"].contains("player_death"))
				configuration_["routes"]["player_death"] = DefaultConfig()["routes"]["player_death"];
			router_ = std::make_unique<EventRouter>(configuration_);
			ConfigureMappedRumbleFilter(configuration_);
			// Validate and normalize all editable body presets before output is
			// enabled. Ordinary Player UUIDs keep their existing named playback.
			if (configuration_.contains("body_slot_effects")) {
				for (const auto& [uuid, source] : configuration_["body_slot_effects"].items()) {
					Json normalized;
					std::string error;
					if (!PrepareEffectContent(source, normalized, error))
						throw std::runtime_error("body_slot_effects." + uuid + ": " + error);
					body_slot_effects_[uuid] = std::move(normalized);
				}
			}
			player_ = std::make_unique<TrueGearPlayer>(configuration_["app_id"].get<std::string>(), configuration_["api_key"].get<std::string>());
			for (const auto& uuid : configuration_["preload_events"])
				player_->PreSeekEffect(uuid.get<std::string>());
			// Direction and amplitude transforms need a base effect. Prefetch the
			// configured names, including custom controller UUIDs, at registration.
			for (const auto& [name, route] : configuration_["routes"].items()) {
				const bool controller = name.starts_with("vr_controller_haptic_");
				if ((!name.starts_with("combat_damage_") && !controller) || !route["enabled"].get<bool>()) continue;
				if (route.contains("uuid")) {
					const auto uuid = route["uuid"].get<std::string>();
					if (!controller || !body_slot_effects_.contains(uuid)) player_->PreSeekEffect(uuid);
					if (controller) power_lookups_.insert(uuid);
				}
				else for (const auto& uuid : route["hands"])
					player_->PreSeekEffect(uuid.get<std::string>());
			}
			enabled_ = configuration_.value("enabled", true);
			dry_run_ = configuration_.value("dry_run", false);
			log_events_ = configuration_.value("log_events", true);
			max_age_ms_ = configuration_["max_event_age_ms"].get<std::size_t>();
			reconnect_ms_ = configuration_["reconnect_ms"].get<int>();
			queue_.SetCapacity(configuration_["max_queue"].get<std::size_t>());
			configured_ = true;
		} catch (const std::exception&) {
			enabled_ = false;
		}
	}

	void StopContinuous(std::string name) {
		const auto at = continuous_.find(name);
		if (at == continuous_.end()) return;
		const auto uuid = at->second.uuid;
		continuous_.erase(at);
		// A user may map two independent actions to the same Player effect.
		// Keep it alive until the last action using that identity has ended.
		for (const auto& [other, playback] : continuous_)
			if (playback.uuid == uuid) return;
		// Only a successful loop start can create this entry. Never stop another
		// configured name, or ask Player to stop all applications' effects.
		if (!played_events_.erase(uuid)) return;
		if (dry_run_) return;
		if (player_ && player_->GetStatus() && player_->Stop(uuid)) return;
		pending_continuous_stops_.insert(uuid);
		if (player_ && player_->GetStatus()) {
			player_->Disconnect();
			next_connect_ = Clock::now() + std::chrono::milliseconds(reconnect_ms_);
		}
	}
	bool FlushContinuousStops() {
		if (dry_run_) { pending_continuous_stops_.clear(); return true; }
		while (!pending_continuous_stops_.empty()) {
			if (!player_ || !player_->GetStatus()) return false;
			const auto at = pending_continuous_stops_.begin();
			if (!player_->Stop(*at)) {
				player_->Disconnect();
				next_connect_ = Clock::now() + std::chrono::milliseconds(reconnect_ms_);
				return false;
			}
			pending_continuous_stops_.erase(at);
		}
		return true;
	}
	// Player owns event contents and duration; retain sent names until reset.
	void StopOwned() {
		if (router_) router_->Reset();
		while (!continuous_.empty()) StopContinuous(continuous_.begin()->first);
		for (const auto& uuid : played_events_) {
			if (!dry_run_ && player_ && player_->GetStatus())
				player_->Stop(uuid);
		}
		played_events_.clear();
	}
	bool Play(const EventItem& item, const std::string& event_name, const std::string& uuid,
	          const Json* content = nullptr, const Json* direction = nullptr, const Json* power = nullptr, bool named_only = false) {
		if (!Fresh(item)) return false;
		if (!named_only && !content && !direction) {
			const auto preset = body_slot_effects_.find(uuid);
			if (preset != body_slot_effects_.end()) content = &*preset;
		}
		std::optional<Json> source;
		Json scaled;
		bool power_fallback = false;
		auto fallback = [&] {
			content = nullptr;
			power_fallback = true;
		};
		if (power) {
			try {
				if (!content && !dry_run_) {
					source = player_->FindEffectByUuid(uuid);
					if (source) content = &*source;
					else {
						fallback();
						if (!power_lookups_.contains(uuid)) {
							if (player_->PreSeekEffect(uuid) && player_->SendSeekEffectByContent(uuid)) power_lookups_.insert(uuid);
						}
					}
				}
				if (content) {
					std::string error;
					if (PreparePowerEffectContent(*content, (*power)["amplitude"].get<float>(), scaled, error))
						content = &scaled;
					else fallback();
				}
			} catch (const std::exception&) {
				fallback();
			}
		}
		if (!played_events_.contains(uuid) && played_events_.size() >= 4096) {
			return false;
		}
		if (!dry_run_) {
			if (!Fresh(item)) return false;
			bool sent = false;
			if (power) {
				if (content) {
					try {
						sent = player_->SendPlayEffectByContent(*content);
						if (!sent) fallback();
					} catch (const std::exception&) {
						fallback();
					}
				}
				// Match the supplied C# catch path without resolving a built-in
				// preset again. Filtered amplitudes never reach this method.
				if (power_fallback && Fresh(item)) sent = player_->SendPlay(uuid);
			} else {
				sent = content ? player_->SendPlayEffectByContent(*content) :
					direction ? player_->PlayAngle(uuid, (*direction)["angle_degrees"].get<float>(), (*direction)["vertical"].get<float>()) :
					player_->SendPlay(uuid);
			}
			if (!sent) {
				player_->Disconnect();
				next_connect_ = Clock::now() + std::chrono::milliseconds(reconnect_ms_);
				return false;
			}
		}
		played_events_.insert(uuid);
		LogPlayback(event_name, uuid, power && !power_fallback ? "power" :
			direction ? "angle" : content ? "content" : "named");
		return true;
	}
	bool Fresh(const EventItem& item) {
		return queue_.Current(item) && Clock::now() - item.created <= std::chrono::milliseconds(max_age_ms_);
	}
	bool ControllerFallback(const EventItem& item, const std::string& name, const std::string& uuid) {
		if (!Fresh(item)) return false;
		if (!played_events_.contains(uuid) && played_events_.size() >= 4096) return false;
		if (!dry_run_ && !player_->SendPlay(uuid)) {
			player_->Disconnect();
			next_connect_ = Clock::now() + std::chrono::milliseconds(reconnect_ms_);
			return false;
		}
		played_events_.insert(uuid);
		LogPlayback(name, uuid, "named");
		return true;
	}
	void PlayControllerFrame(const EventItem& item, const Json& frame) {
		if (!Fresh(item) || !enabled_ || !configured_ ||
			(!dry_run_ && !player_->GetStatus()) || !FlushContinuousStops()) return;
		struct Component {
			std::string name, uuid;
			Json content;
		};
		std::vector<Component> components;
		const auto now = Milliseconds();
		for (const auto* side : {"left", "right"}) {
			const auto field = std::string(side) + "_amplitude";
			const auto& data = frame["data"];
			if (!data.contains(field) || !data[field].is_number()) continue;
			const double raw = data[field].get<double>();
			if (!std::isfinite(raw) || raw < 0.1 || raw > std::numeric_limits<float>::max()) continue;
			const float amplitude = static_cast<float>(raw);
			const std::string name = std::string("vr_controller_haptic_") + side;
			auto event = frame;
			event["event"] = name;
			event["playback_name_resolved"] = true;
			event["data"] = {{"playback", "power"}, {"amplitude", amplitude}};
			for (const auto& uuid : router_->Map(event, now)) {
				try {
					std::optional<Json> source;
					const auto preset = body_slot_effects_.find(uuid);
					if (preset != body_slot_effects_.end()) source = *preset;
					else if (!dry_run_) source = player_->FindEffectByUuid(uuid);
					if (!source) {
						if (!dry_run_ && !power_lookups_.contains(uuid) &&
							player_->PreSeekEffect(uuid) && player_->SendSeekEffectByContent(uuid)) power_lookups_.insert(uuid);
						ControllerFallback(item, name, uuid);
						continue;
					}
					Json scaled;
					std::string error;
					if (!PreparePowerEffectContent(*source, amplitude, scaled, error)) {
						ControllerFallback(item, name, uuid);
						continue;
					}
					if (!scaled["tracks"].empty()) components.push_back({name, uuid, std::move(scaled)});
				} catch (const std::exception&) {
					ControllerFallback(item, name, uuid);
				}
			}
		}
		if (components.empty() || !Fresh(item)) return;
		Json content = components.front().content;
		std::string wire_name = components.front().uuid;
		if (components.size() == 2) {
			// Keep all per-track fields. Parent settings must agree so combining
			// does not silently change keep or a caller's effect extensions.
			auto left_parent = content;
			auto right_parent = components.back().content;
			for (const auto* field : {"name", "uuid", "tracks"}) {
				left_parent.erase(field);
				right_parent.erase(field);
			}
			if (left_parent != right_parent) {
				for (const auto& part : components)
					ControllerFallback(item, part.name, part.uuid);
				return;
			}
			wire_name = "vr_controller_haptic_frame";
			content["name"] = wire_name;
			content["uuid"] = wire_name;
			for (const auto& track : components.back().content["tracks"]) content["tracks"].push_back(track);
		}
		if (!played_events_.contains(wire_name) && played_events_.size() >= 4096) return;
		bool sent = dry_run_;
		try {
			if (!Fresh(item)) return;
			if (!dry_run_) sent = player_->SendPlayEffectByContent(content);
		} catch (const std::exception&) {}
		if (!sent) {
			for (const auto& part : components)
				ControllerFallback(item, part.name, part.uuid);
			return;
		}
		// Stop the identity actually sent, rather than the two cached sources.
		played_events_.insert(wire_name);
		LogPlayback(components.size() == 2 ? std::string("vr_controller_haptic_frame") : components.front().name,
			wire_name, "frame");
	}
	void RefreshContinuous(const EventItem& item, const Json& event) {
		const auto name = event["event"].get<std::string>();
		const auto& data = event["data"];
		if (!data.contains("active") || !data["active"].is_boolean()) {
			return;
		}
		if (!data["active"].get<bool>()) {
			StopContinuous(name);
			return;
		}
		auto at = continuous_.find(name);
		if (at != continuous_.end() && (!queue_.Current(at->second.lease) ||
			Clock::now() - at->second.lease.created >= std::chrono::milliseconds(350)))
			StopContinuous(name);
		if (!enabled_ || !configured_ || (!dry_run_ && !player_->GetStatus()) || !FlushContinuousStops()) return;
		const auto route = configuration_["routes"].find(name);
		if (route == configuration_["routes"].end() || !(*route)["enabled"].get<bool>()) return;
		at = continuous_.find(name);
		if (at != continuous_.end()) {
			// A refresh renews the current generation/epoch lease, independently of
			// the route's playback cooldown. It does not send another immediate hit.
			at->second.lease = item;
			return;
		}
		for (const auto& uuid : router_->Map(event, Milliseconds())) {
			if (!Play(item, name, uuid)) return;
			const auto interval = std::chrono::milliseconds(std::max<std::int64_t>(150, (*route)["cooldown_ms"].get<std::int64_t>()));
			continuous_.emplace(name, ContinuousPlayback{item, uuid, Clock::now() + interval, interval});
			return;
		}
	}
	void TickContinuous() {
		const auto now = Clock::now();
		for (auto at = continuous_.begin(); at != continuous_.end();) {
			const auto name = at->first;
			auto& playback = (at++)->second;
			if (!queue_.Current(playback.lease)) { StopContinuous(name); continue; }
			if (now - playback.lease.created >= std::chrono::milliseconds(350)) {
				StopContinuous(name);
				continue;
			}
			if (!enabled_ || !configured_) { StopContinuous(name); continue; }
			if (!dry_run_ && !player_->GetStatus()) { StopContinuous(name); continue; }
			if (now < playback.next_play) continue;
			// Only the currently leased action may create a new playback request.
			// Do not reuse a stale queued timestamp or replay missed intervals.
			auto repeat = playback.lease;
			repeat.created = now;
			const auto uuid = playback.uuid;
			playback.next_play = now + playback.interval;
			if (!Play(repeat, name, uuid))
				StopContinuous(name);
		}
	}
	void Process(const EventItem& item) {
		if (!Fresh(item)) return;
		try {
			if (item.kind != EventKind::Collected && (player_dead_ || !enabled_ || !configured_ || (!dry_run_ && !player_->GetStatus()))) return;
			if (item.kind == EventKind::NamedPlayback) {
				if (IsTrueGearEventName(item.payload)) Play(item, "script", item.payload);
				return;
			}
			auto depth_guard = [](int depth, Json::parse_event_t, Json&) {
				if (depth > 24) throw std::runtime_error("event JSON is too deeply nested");
				return true;
			};
			const auto event = Json::parse(item.payload, depth_guard);
			if (item.kind == EventKind::ContentPlayback) {
				Json normalized;
				std::string error;
				if (!PrepareEffectContent(event, normalized, error)) return;
				Play(item, "script", normalized["uuid"].get<std::string>(), &normalized);
				return;
			}
			if (!event.is_object() || event.value("vm", "") != (item.context == 0 ? "server" : "client")) return;
			if (event.value("schema", "") != "truegear.events/1" || !event.contains("data") || !event["data"].is_object()) return;
			const auto name = event.value("event", "");
			if (!IsTrueGearEventName(name)) return;
			if (name == "player_spawn") {
				if (death_owner_ && queue_.SameGeneration(*death_owner_) &&
					(item.context != death_owner_->context || item.generation != death_owner_->generation)) return;
				if (player_dead_) StopOwned();
				if (death_owner_) death_owner_ = item;
				player_dead_ = false;
				return;
			}
			if (name == "player_death") {
				if (player_dead_) return;
				if (death_owner_ && queue_.SameGeneration(*death_owner_) &&
					(item.context != death_owner_->context || item.generation != death_owner_->generation)) return;
				player_dead_ = true;
				death_owner_ = item;
				StopOwned();
				EventItem death;
				if (!queue_.ResetForPlayback(item, death)) return;
				if (!enabled_ || !configured_ || (!dry_run_ && !player_->GetStatus()) || !FlushContinuousStops()) return;
				auto named = event;
				named["data"] = Json::object();
				named["playback_name_resolved"] = true;
				for (const auto& uuid : router_->Map(named, Milliseconds()))
					Play(death, name, uuid, nullptr, nullptr, nullptr, true);
				return;
			}
			if (name == "session_ready" || name == "player_despawn") {
				// Late peer-VM readiness and dead-player disappearance must not
				// cut the one-shot or rearm it. Real VM/reset boundaries still stop it.
				if (player_dead_) return;
				StopOwned();
				queue_.Reset(-1, name.c_str());
				return;
			}
			const auto& data = event["data"];
			if (name == "vr_haptic_sources") {
				if (item.context != 1 || !data.contains("sources")) return;
				std::string error;
				SetMappedRumbleSources(data["sources"], error);
				return; // Metadata only; never route or send this packet to Player.
			}
			if (player_dead_) return;
			if (data.value("playback", "") == "controller_haptic_frame") {
				if (name == "vr_controller_haptic_frame") PlayControllerFrame(item, event);
				return;
			}
			const Json* power = nullptr;
			if (data.contains("playback") && data["playback"] == "power") {
				if (!data.contains("amplitude") || !data["amplitude"].is_number()) return;
				const double amplitude = data["amplitude"].get<double>();
				if (!std::isfinite(amplitude) || std::abs(amplitude) > std::numeric_limits<float>::max()) return;
				if (amplitude < 0.1) return;
				power = &data;
			}
			if (data.value("playback", "") == "continuous") {
				if (name == "movement_slide_start") RefreshContinuous(item, event);
				return;
			}
			if (!enabled_ || !configured_ || (!dry_run_ && !player_->GetStatus()) || !FlushContinuousStops()) return;
			const Json* direction = nullptr;
			if (data.contains("playback") && data["playback"] == "angle") {
				bool valid = true;
				for (const char* key : {"angle_degrees", "vertical"}) {
					if (!data.contains(key) || !data[key].is_number()) { valid = false; break; }
					const double value = data[key].get<double>();
					if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) { valid = false; break; }
				}
				if (valid) direction = &data;
			}
			for (const auto& uuid : router_->Map(event, Milliseconds()))
				if (!Play(item, name, uuid, nullptr, direction, power)) return;
		} catch (const std::exception&) {}
	}
	void Run() {
		Configure();
		for (;;) {
			try {
				auto batch = queue_.Next(std::chrono::milliseconds(50));
				if (batch.reset) StopOwned();
				const bool connected = player_ && player_->GetStatus();
				if (player_) player_->Poll();
				if (connected && !player_->GetStatus()) {
					while (!continuous_.empty()) StopContinuous(continuous_.begin()->first);
				}
				if (!queue_.Active()) {
					// EndVm may run after Next took its reset snapshot. Stop before
					// disconnecting even when that snapshot did not contain a reset.
					StopOwned();
					if (player_ && player_->GetStatus()) player_->Disconnect();
					continue;
				}
				if (configured_ && enabled_ && !dry_run_ && !player_->GetStatus() && Clock::now() >= next_connect_) {
					player_->Start();
					next_connect_ = Clock::now() + std::chrono::milliseconds(reconnect_ms_);
				}
				FlushContinuousStops();
				for (const auto& item : batch.events) Process(item);
				TickContinuous();
			} catch (const std::exception&) {
				if (player_) player_->Disconnect();
				next_connect_ = Clock::now() + std::chrono::milliseconds(reconnect_ms_);
			} catch (...) {
			}
		}
	}
public:
	Runtime(HMODULE module, LogCallback callback) : callback_(callback) {
		std::wstring filename(32768, L'\0');
		const DWORD size = GetModuleFileNameW(module, filename.data(), static_cast<DWORD>(filename.size()));
		if (!size || size >= filename.size()) throw std::runtime_error("plugin path unavailable");
		filename.resize(size);
		directory_ = std::filesystem::path(filename).parent_path() / L"TrueGear";
	}
	void Start() { std::thread([this] { Run(); }).detach(); }
	EventQueue& Queue() { return queue_; }
};

// Northstar's plugin is pinned. A process-lifetime object avoids destructing
// network callbacks or joining threads while Windows holds its loader lock.
std::atomic<Runtime*> runtime{};
std::once_flag initialized;
}

void Initialize(HMODULE self, LogCallback callback) {
	std::call_once(initialized, [&] {
		try {
			auto* instance = new Runtime(self, callback);
			runtime.store(instance, std::memory_order_release);
			instance->Start();
		} catch (const std::exception&) {}
	});
}
// Plugin telemetry never enters either log, even if its text resembles PLAY.
void Diagnostic(int, const char*) noexcept {}
void BeginVm(int context, void* token) {
	if (auto* instance = runtime.load(std::memory_order_acquire)) instance->Queue().Begin(context, token);
}
void EndVm(int context, void* token) {
	if (auto* instance = runtime.load(std::memory_order_acquire)) instance->Queue().End(context, token);
}
bool Submit(int context, const char* text) {
	if (!text) return false;
	const auto length = strnlen_s(text, 32769);
	if (!length || length > 32768) return false;
	if (auto* instance = runtime.load(std::memory_order_acquire))
		return instance->Queue().Push(context, std::string(text, length));
	return false;
}
bool SendPlay(int context, const char* uuid) {
	if (!uuid || context < 0 || context > 1) return false;
	const auto length = strnlen_s(uuid, 257);
	if (!length || length > 256) return false;
	const std::string name(uuid, length);
	if (!IsTrueGearEventName(name)) return false;
	if (auto* instance = runtime.load(std::memory_order_acquire))
		return instance->Queue().Push(context, name, Clock::now(), EventKind::NamedPlayback);
	return false;
}
bool SendPlayEffectByContent(int context, const char* json) {
	if (!json || context < 0 || context > 1) return false;
	const auto length = strnlen_s(json, 32769);
	if (!length || length > 32768) return false;
	// Copy only; parsing and SDK normalization happen on the output worker.
	if (auto* instance = runtime.load(std::memory_order_acquire))
		return instance->Queue().Push(context, std::string(json, length), Clock::now(), EventKind::ContentPlayback);
	return false;
}
void Reset(int context, const char* reason) {
	if (auto* instance = runtime.load(std::memory_order_acquire)) instance->Queue().Reset(context, reason);
}
}
