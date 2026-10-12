// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_state.h"

#include "ayu/libs/json.hpp"
#include "ayu/ayu_settings.h"
#include "ayu/features/filters/filters_cache_controller.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"

#include <QtCore/QFile>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace AyuState {
namespace {

using json = nlohmann::json;
using AccountId = uint64;

constexpr auto kFormatVersion = 2;
constexpr auto kRepostFilterGraceSeconds = 60 * 60;

struct AccountState {
	HiddenMessages hidden;
	std::unordered_set<PeerId> showingPeers;
	bool showingAll = false;
};

std::unordered_map<AccountId, AccountState> AccountStates;
std::unordered_map<AccountId, RepostFilter> RepostFilters;
std::optional<HiddenMessages> PendingLegacy;
std::unordered_set<AccountId> LegacySeededAccounts;
Main::Session *DisableGhostModeOnStoryCloseSession = nullptr;

[[nodiscard]] QString StatePath() {
	return cWorkingDir() + u"tdata/ayu_hidden_messages.json"_q;
}

[[nodiscard]] std::optional<uint64> ParseKey(const std::string &key) {
	auto result = uint64(0);
	const auto [end, error] = std::from_chars(
		key.data(),
		key.data() + key.size(),
		result);
	return (error == std::errc()
		&& end == key.data() + key.size()
		&& result)
		? std::make_optional(result)
		: std::nullopt;
}

[[nodiscard]] HiddenMessages ParseHiddenMessages(const json &value) {
	auto result = HiddenMessages();
	if (!value.is_object()) {
		return result;
	}
	for (auto i = value.begin(); i != value.end(); ++i) {
		const auto parsedPeerId = ParseKey(i.key());
		if (!parsedPeerId || !i.value().is_array()) {
			continue;
		}
		const auto peerId = PeerId(PeerIdHelper(*parsedPeerId));
		auto messages = HiddenMessageIds();
		for (const auto &message : i.value()) {
			if (!message.is_number_integer()) {
				continue;
			}
			auto bare = int64(0);
			if (message.is_number_unsigned()) {
				const auto value = message.get<uint64>();
				if (value > uint64(std::numeric_limits<int64>::max())) {
					continue;
				}
				bare = int64(value);
			} else {
				bare = message.get<int64>();
			}
			if (const auto messageId = MsgId(bare)) {
				messages.insert(messageId);
			}
		}
		if (!messages.empty()) {
			result.emplace(peerId, std::move(messages));
		}
	}
	return result;
}

[[nodiscard]] std::unordered_set<FilterId> ParseFolderIds(const json &value) {
	auto result = std::unordered_set<FilterId>();
	if (!value.is_array()) {
		return result;
	}
	for (const auto &id : value) {
		if (!id.is_number_integer()) {
			continue;
		}
		const auto bare = id.get<int64>();
		if (bare > 0 && bare <= std::numeric_limits<FilterId>::max()) {
			result.insert(FilterId(bare));
		}
	}
	return result;
}

[[nodiscard]] json SerializeFolderIds(const std::unordered_set<FilterId> &ids) {
	auto sorted = std::vector<FilterId>(ids.begin(), ids.end());
	std::sort(sorted.begin(), sorted.end());
	return sorted;
}

[[nodiscard]] json SerializeState() {
	auto accounts = json::object();
	for (const auto &[accountId, state] : AccountStates) {
		auto peers = json::object();
		for (const auto &[peerId, messages] : state.hidden) {
			if (messages.empty()) {
				continue;
			}
			auto sorted = std::vector<int64>();
			sorted.reserve(messages.size());
			for (const auto messageId : messages) {
				sorted.push_back(messageId.bare);
			}
			std::sort(sorted.begin(), sorted.end());
			peers[std::to_string(peerId.value)] = std::move(sorted);
		}
		if (!peers.empty()) {
			accounts[std::to_string(accountId)] = std::move(peers);
		}
	}
	auto filters = json::object();
	for (const auto &[accountId, filter] : RepostFilters) {
		auto channels = json::object();
		for (const auto &[peerId, enabled] : filter.channels) {
			channels[std::to_string(peerId.value)] = enabled;
		}
		filters[std::to_string(accountId)] = json::object({
			{ "paused", filter.paused },
			{ "all", filter.enabledAll },
			{ "only_folders", filter.onlyFolders },
			{ "included_folders", SerializeFolderIds(filter.includedFolders) },
			{ "excluded_folders", SerializeFolderIds(filter.excludedFolders) },
			{ "channels", std::move(channels) },
			{ "percent", filter.percent },
			{ "days", filter.days },
		});
	}
	return json::object({
		{ "repost_filters", std::move(filters) },
		{ "version", kFormatVersion },
		{ "accounts", std::move(accounts) },
	});
}

void Save() {
	if (PendingLegacy) {
		return;
	}
	try {
		const auto serialized = QByteArray::fromStdString(
			SerializeState().dump(4));
		auto file = QSaveFile(StatePath());
		file.setDirectWriteFallback(false);
		if (!file.open(QIODevice::WriteOnly)) {
			LOG(("AyuState: could not open hidden messages state for writing."));
			return;
		}
		if (file.write(serialized) != serialized.size()) {
			file.cancelWriting();
			LOG(("AyuState: could not write hidden messages state."));
			return;
		}
		if (!file.commit()) {
			LOG(("AyuState: could not commit hidden messages state."));
		}
	} catch (...) {
		LOG(("AyuState: could not serialize hidden messages state."));
	}
}

void RemoveEmptyAccount(AccountId accountId) {
	const auto i = AccountStates.find(accountId);
	if (i == AccountStates.end() || !i->second.hidden.empty()) {
		return;
	}
	i->second.showingPeers.clear();
	i->second.showingAll = false;
	AccountStates.erase(i);
}

void SeedLegacy(not_null<Main::Session*> session) {
	if (!PendingLegacy) {
		return;
	}
	const auto accountId = session->uniqueId();
	if (!LegacySeededAccounts.emplace(accountId).second) {
		return;
	}
	auto &hidden = AccountStates[accountId].hidden;
	for (const auto &[peerId, messages] : *PendingLegacy) {
		hidden[peerId].insert(messages.begin(), messages.end());
	}
	RemoveEmptyAccount(accountId);
}

void SeedAllLegacyAccounts() {
	Expects(PendingLegacy.has_value());
	Expects(Core::IsAppLaunched());
	Expects(Core::App().domain().started());

	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) {
			SeedLegacy(session);
		}
	}
}

[[nodiscard]] AccountState *FindAccount(
		not_null<Main::Session*> session) {
	SeedLegacy(session);
	const auto i = AccountStates.find(session->uniqueId());
	return (i != AccountStates.end()) ? &i->second : nullptr;
}

} // namespace

void load() {
	RepostFilters.clear();
	auto file = QFile(StatePath());
	if (!file.open(QIODevice::ReadOnly)) {
		AccountStates.clear();
		PendingLegacy.reset();
		LegacySeededAccounts.clear();
		return;
	}
	try {
		const auto serialized = file.readAll();
		const auto root = json::parse(
			serialized.constData(),
			serialized.constData() + serialized.size());
		if (!root.is_object()) {
			throw std::runtime_error("invalid root");
		}
		auto loaded = std::unordered_map<AccountId, AccountState>();
		auto legacy = std::optional<HiddenMessages>();
		if (root.contains("version")) {
			const auto &version = root.at("version");
			const auto validVersion = version.is_number_unsigned()
				? (version.get<uint64>() == kFormatVersion)
				: (version.is_number_integer()
					&& version.get<int64>() == kFormatVersion);
			if (!validVersion
				|| !root.contains("accounts")
				|| !root.at("accounts").is_object()) {
				throw std::runtime_error("unsupported format");
			}
			for (auto i = root.at("accounts").begin();
				i != root.at("accounts").end();
				++i) {
				const auto accountId = ParseKey(i.key());
				if (!accountId) {
					continue;
				}
				auto hidden = ParseHiddenMessages(i.value());
				if (!hidden.empty()) {
					loaded.emplace(
						*accountId,
						AccountState{ .hidden = std::move(hidden) });
				}
			}
		} else {
			auto hidden = ParseHiddenMessages(root);
			if (!root.empty() && hidden.empty()) {
				throw std::runtime_error("invalid legacy format");
			}
			legacy = std::move(hidden);
		}
		auto filters = std::unordered_map<AccountId, RepostFilter>();
		if (root.contains("repost_filters")
			&& root.at("repost_filters").is_object()) {
			for (const auto &[key, value] : root.at("repost_filters").items()) {
				const auto accountId = ParseKey(key);
				if (!accountId || !value.is_object()) {
					continue;
				}
				try {
					auto filter = RepostFilter();
					filter.paused = value.value("paused", false);
					filter.enabledAll = value.value("all", false);
					filter.onlyFolders = value.value("only_folders", false);
					filter.includedFolders = ParseFolderIds(
						value.value("included_folders", json::array()));
					filter.excludedFolders = ParseFolderIds(
						value.value("excluded_folders", json::array()));
					filter.percent = std::clamp(value.value("percent", 2), 1, 100);
					filter.days = std::clamp(value.value("days", 7), 1, 365);
					if (value.contains("channels") && value.at("channels").is_object()) {
						for (const auto &[peerKey, enabled] : value.at("channels").items()) {
							const auto peerId = ParseKey(peerKey);
							if (peerId && enabled.is_boolean()) {
								filter.channels.emplace(
									PeerId(PeerIdHelper(*peerId)),
									enabled.get<bool>());
							}
						}
					}
					filters.emplace(*accountId, std::move(filter));
				} catch (...) {
					LOG(("AyuState: invalid repost filter settings."));
				}
			}
		}
		RepostFilters = std::move(filters);
		AccountStates = std::move(loaded);
		PendingLegacy = std::move(legacy);
		LegacySeededAccounts.clear();
	} catch (...) {
		AccountStates.clear();
		PendingLegacy.reset();
		LegacySeededAccounts.clear();
		LOG(("AyuState: failed to read hidden messages state."));
	}
}

void migrateLegacy() {
	if (!PendingLegacy
		|| !Core::IsAppLaunched()
		|| !Core::App().domain().started()) {
		return;
	}
	SeedAllLegacyAccounts();
	PendingLegacy.reset();
	LegacySeededAccounts.clear();
	Save();
}

void setHidden(
		not_null<Main::Session*> session,
		const MessageIdsList &ids,
		bool hidden) {
	SeedLegacy(session);
	const auto accountId = session->uniqueId();
	auto i = AccountStates.find(accountId);
	if (i == AccountStates.end()) {
		if (!hidden) {
			return;
		}
		i = AccountStates.emplace(accountId, AccountState()).first;
	}
	auto &state = i->second;
	auto changed = false;
	for (const auto &id : ids) {
		if (!id.peer || !id.msg) {
			continue;
		}
		if (hidden) {
			changed |= state.hidden[id.peer].insert(id.msg).second;
		} else {
			const auto peer = state.hidden.find(id.peer);
			if (peer != state.hidden.end()) {
				changed |= (peer->second.erase(id.msg) != 0);
				if (peer->second.empty()) {
					state.showingPeers.erase(id.peer);
					state.hidden.erase(peer);
				}
			}
		}
	}
	RemoveEmptyAccount(accountId);
	if (changed) {
		Save();
	}
}

void clearHidden(
		not_null<Main::Session*> session,
		PeerId peerId) {
	SeedLegacy(session);
	const auto accountId = session->uniqueId();
	const auto i = AccountStates.find(accountId);
	if (i == AccountStates.end()) {
		return;
	}
	const auto changed = (i->second.hidden.erase(peerId) != 0);
	i->second.showingPeers.erase(peerId);
	RemoveEmptyAccount(accountId);
	if (changed) {
		Save();
	}
}

void clearAllHidden(not_null<Main::Session*> session) {
	SeedLegacy(session);
	const auto i = AccountStates.find(session->uniqueId());
	if (i == AccountStates.end()) {
		return;
	}
	const auto changed = !i->second.hidden.empty();
	AccountStates.erase(i);
	if (changed) {
		Save();
	}
}

const HiddenMessageIds *getHiddenMessages(
		not_null<Main::Session*> session,
		PeerId peerId) {
	const auto state = FindAccount(session);
	if (!state) {
		return nullptr;
	}
	const auto i = state->hidden.find(peerId);
	return (i != state->hidden.end() && !i->second.empty())
		? &i->second
		: nullptr;
}

const HiddenMessages &getAllHiddenMessages(
		not_null<Main::Session*> session) {
	static const auto empty = HiddenMessages();
	const auto state = FindAccount(session);
	return state ? state->hidden : empty;
}

bool hasAnyHiddenMessagesAll(not_null<Main::Session*> session) {
	const auto state = FindAccount(session);
	return state && !state->hidden.empty();
}

bool hasHiddenFlag(not_null<HistoryItem*> item) {
	const auto history = item->history();
	const auto messages = getHiddenMessages(
		&history->session(),
		history->peer->id);
	return messages && messages->contains(item->id);
}

RepostFilter repostFilter(not_null<Main::Session*> session) {
	const auto i = RepostFilters.find(session->uniqueId());
	return (i != RepostFilters.end()) ? i->second : RepostFilter();
}

bool repostFilterEnabled(not_null<Main::Session*> session, PeerId peerId) {
	const auto i = RepostFilters.find(session->uniqueId());
	if (i == RepostFilters.end()) {
		return false;
	}
	const auto &filter = i->second;
	if (filter.paused) {
		return false;
	}
	const auto channel = filter.channels.find(peerId);
	if (channel != filter.channels.end() && !channel->second) {
		return false;
	}
	auto included = false;
	if (const auto history = session->data().historyLoaded(peerId)) {
		for (const auto &folder : session->data().chatsFilters().list()) {
			const auto id = folder.id();
			if (!id
				|| (!filter.includedFolders.contains(id)
					&& !filter.excludedFolders.contains(id))
				|| !folder.contains(history)) {
				continue;
			}
			if (filter.excludedFolders.contains(id)) {
				return false;
			}
			included = true;
		}
	}
	return filter.onlyFolders
		? included
		: (included || filter.enabledAll || channel != filter.channels.end());
}

void setRepostFilter(not_null<Main::Session*> session, RepostFilter filter) {
	filter.percent = std::clamp(filter.percent, 1, 100);
	filter.days = std::clamp(filter.days, 1, 365);
	RepostFilters[session->uniqueId()] = std::move(filter);
	Save();
	FiltersCacheController::fireUpdate();
}

bool isRepostFiltered(not_null<HistoryItem*> item) {
	const auto history = item->history();
	if (!history->peer->isBroadcast()
		|| !item->isHistoryEntry()
		|| item->isService()
		|| item->isLocal()
		|| !repostFilterEnabled(&history->session(), history->peer->id)) {
		return false;
	}
	const auto &filter = RepostFilters.at(history->session().uniqueId());
	const auto now = int64(base::unixtime::now());
	const auto cutoff = now - int64(filter.days) * 86400;
	const auto newestAllowed = now - kRepostFilterGraceSeconds;
	const auto matches = [&](not_null<HistoryItem*> post) {
		const auto views = post->Get<HistoryMessageViews>();
		return int64(post->date()) > cutoff
			&& int64(post->date()) <= newestAllowed
			&& views
			&& views->views.count > 0
			&& views->forwardsCount >= 0
			&& int64(views->forwardsCount) * 100
				< int64(views->views.count) * filter.percent;
	};
	if (const auto group = history->owner().groups().find(item)) {
		return std::any_of(group->items.begin(), group->items.end(), matches);
	}
	return matches(item);
}

bool isHidden(not_null<HistoryItem*> item) {
	if (isRepostFiltered(item)) {
		return true;
	}
	const auto history = item->history();
	const auto peerId = history->peer->id;
	const auto state = FindAccount(&history->session());
	if (!state
		|| state->showingAll
		|| state->showingPeers.contains(peerId)) {
		return false;
	}
	const auto messages = state->hidden.find(peerId);
	return (messages != state->hidden.end())
		&& messages->second.contains(item->id);
}

void showHiddenMessages(
		not_null<Main::Session*> session,
		PeerId peerId,
		bool show) {
	const auto state = FindAccount(session);
	if (!state) {
		return;
	}
	if (show && state->hidden.contains(peerId)) {
		state->showingPeers.insert(peerId);
	} else {
		state->showingPeers.erase(peerId);
	}
}

std::optional<bool> hiddenMessagesShown(
		not_null<Main::Session*> session,
		PeerId peerId) {
	const auto state = FindAccount(session);
	if (!state || !state->hidden.contains(peerId)) {
		return std::nullopt;
	}
	return state->showingPeers.contains(peerId);
}

void showAllHiddenMessages(
		not_null<Main::Session*> session,
		bool show) {
	const auto state = FindAccount(session);
	if (state) {
		state->showingAll = show && !state->hidden.empty();
	}
}

std::optional<bool> allHiddenMessagesShown(
		not_null<Main::Session*> session) {
	const auto state = FindAccount(session);
	return (state && !state->hidden.empty())
		? std::make_optional(state->showingAll)
		: std::nullopt;
}

void setDisableGhostModeOnStoryClose(Main::Session *session) {
	DisableGhostModeOnStoryCloseSession = session;
}

void disableGhostModeOnStoryClose(Main::Session *session) {
	if (DisableGhostModeOnStoryCloseSession != session) {
		return;
	}
	DisableGhostModeOnStoryCloseSession = nullptr;
	if (session) {
		AyuSettings::ghost(session).setGhostModeEnabled(false);
	}
}

} // namespace AyuState
