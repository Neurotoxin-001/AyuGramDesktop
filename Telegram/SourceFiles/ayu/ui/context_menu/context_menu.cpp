// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/context_menu/context_menu.h"

#include "apiwrap.h"
#include "lang_auto.h"
#include "mainwidget.h"
#include "api/api_sending.h"
#include "ayu/ayu_settings.h"
#include "ayu/ayu_state.h"
#include "ayu/data/messages_storage.h"
#include "ayu/features/filters/filters_cache_controller.h"
#include "ayu/features/filters/filters_controller.h"
#include "ayu/features/forward/ayu_forward.h"
#include "ayu/features/forward/ayu_forward_rich.h"
#include "ayu/ui/boxes/delete_channel_posts_box.h"
#include "ayu/ui/context_menu/menu_item_subtext.h"
#include "ayu/ui/message_history/history_section.h"
#include "ayu/ui/settings/filters/edit_filter.h"
#include "ayu/ui/settings/filters/settings_filters_list.h"
#include "ayu/utils/qt_key_modifiers_extended.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/mime_type.h"
#include "data/data_channel.h"
#include "data/data_chat_filters.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_chat.h"
#include "data/data_forum_topic.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "styles/style_ayu_icons.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include <algorithm>

namespace AyuUi {

namespace {

class RepostExceptionsController final : public ChatsListBoxController {
public:
	explicit RepostExceptionsController(not_null<Main::Session*> session)
	: ChatsListBoxController(session)
	, _session(session) {
	}

	Main::Session &session() const override {
		return *_session;
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

protected:
	void prepareViewHook() override {
		auto selected = std::vector<not_null<PeerData*>>();
		for (const auto &[peerId, enabled] : AyuState::repostFilter(_session).channels) {
			if (!enabled) {
				const auto peer = _session->data().peer(peerId);
				if (peer->isBroadcast()) {
					selected.push_back(peer);
				}
			}
		}
		delegate()->peerListAddSelectedPeers(selected);
	}

	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		return history->peer->isBroadcast() ? std::make_unique<Row>(history) : nullptr;
	}

private:
	const not_null<Main::Session*> _session;

};

class RepostFilterCheckbox final : public Ui::Checkbox {
public:
	RepostFilterCheckbox(QWidget *parent, const QString &text, bool checked)
	: Ui::Checkbox(parent, text, checked, st::defaultBoxCheckbox) {
		setAllowTextLines();
	}

protected:
	QPoint prepareRippleStartPosition() const override {
		return DisabledRippleStartPosition();
	}

};

void RepostFilterSettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	const auto filter = AyuState::repostFilter(session);
	const auto addNumberInput = [&](
			rpl::producer<QString> placeholder,
			int value,
			int limit) {
		const auto wrap = box->addRow(object_ptr<Ui::FixedHeightWidget>(
			box,
			st::defaultInputField.heightMin));
		const auto field = Ui::CreateChild<Ui::NumberInput>(
			wrap,
			st::defaultInputField,
			std::move(placeholder),
			QString::number(value),
			limit);
		wrap->widthValue() | rpl::on_next([=](int width) {
			field->resize(width, field->height());
		}, wrap->lifetime());
		return field;
	};
	box->setTitle(rpl::single(u"Фильтрация постов по репостам"_q));
	box->setWidth(st::boxWideWidth * 3 / 2);
	box->setMaxHeight(st::boxMaxListHeight);
	const auto active = box->addRow(object_ptr<RepostFilterCheckbox>(
		box,
		u"Фильтрация включена"_q,
		!filter.paused));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(u"Скрывать посты с долей репостов ниже порога спустя 60 минут после публикации. Посты старше указанного срока остаются видимыми."_q),
		st::boxLabel));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(u"Порог репостов, % (1–100)"_q),
		st::boxLabel));
	const auto percent = addNumberInput(
		rpl::single(u"Процент репостов"_q),
		filter.percent,
		100);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(u"Возраст постов в днях (1–365)"_q),
		st::boxLabel));
	const auto days = addNumberInput(
		rpl::single(u"Количество дней"_q),
		filter.days,
		365);
	const auto onlyFolders = box->addRow(object_ptr<RepostFilterCheckbox>(
		box,
		u"Только выбранные папки"_q,
		filter.onlyFolders));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(u"Выберите папки для фильтрации или исключения. Исключения имеют приоритет. Новые каналы в папке учитываются автоматически."_q),
		st::boxLabel));
	struct FolderSelection {
		FilterId id = 0;
		Ui::Checkbox *included = nullptr;
		Ui::Checkbox *excluded = nullptr;
	};
	auto folders = std::vector<FolderSelection>();
	for (const auto &folder : session->data().chatsFilters().list()) {
		if (!folder.id()) {
			continue;
		}
		box->addSkip(st::defaultCheck.diameter / 2);
		const auto title = folder.titleText().text;
		const auto included = box->addRow(object_ptr<RepostFilterCheckbox>(
			box,
			u"Включить: %1"_q.arg(title),
			filter.includedFolders.contains(folder.id())));
		const auto excluded = box->addRow(object_ptr<RepostFilterCheckbox>(
			box,
			u"Исключить: %1"_q.arg(title),
			filter.excludedFolders.contains(folder.id())));
		included->checkedChanges() | rpl::on_next([=](bool checked) {
			if (checked) {
				excluded->setChecked(false);
			}
		}, box->lifetime());
		excluded->checkedChanges() | rpl::on_next([=](bool checked) {
			if (checked) {
				included->setChecked(false);
			}
		}, box->lifetime());
		folders.push_back({ folder.id(), included, excluded });
	}
	const auto error = box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
	error->setTextColorOverride(st::boxTextFgError->c);
	const auto save = [=] {
		const auto percentValue = percent->getLastText().toInt();
		const auto daysValue = days->getLastText().toInt();
		if (percentValue < 1 || percentValue > 100) {
			percent->showError();
			return;
		}
		if (daysValue < 1 || daysValue > 365) {
			days->showError();
			return;
		}
		auto updated = AyuState::repostFilter(session);
		updated.paused = !active->checked();
		updated.percent = percentValue;
		updated.days = daysValue;
		updated.onlyFolders = onlyFolders->checked();
		updated.includedFolders.clear();
		updated.excludedFolders.clear();
		for (const auto &folder : folders) {
			if (folder.included->checked()) {
				updated.includedFolders.insert(folder.id);
			}
			if (folder.excluded->checked()) {
				updated.excludedFolders.insert(folder.id);
			}
		}
		if (!updated.paused && updated.onlyFolders && updated.includedFolders.empty()) {
			error->setText(u"Выберите хотя бы одну папку для фильтрации."_q);
			return;
		}
		AyuState::setRepostFilter(session, std::move(updated));
		box->closeBox();
	};
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	QObject::connect(percent, &Ui::NumberInput::submitted, box, [=] {
		days->setFocusFast();
	});
	QObject::connect(days, &Ui::NumberInput::submitted, box, save);
}

Fn<void()> ClearDeletedMessagesHandler(not_null<Window::SessionController*> controller, not_null<PeerData*> peer, ID topicId) {
	return [=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::ayu_ClearDeletedMessagesText(tr::now),
			.confirmed = [=](Fn<void()> &&close) {
				auto items = std::vector<not_null<HistoryItem*>>();
				for (const auto &block : peer->owner().history(peer)->blocks) {
					for (const auto &view : block->messages) {
						const auto item = view->data();
						if (item->isDeleted() && (!topicId || (item->topicRootId().bare == topicId))) {
							items.push_back(item);
						}
					}
				}
				AyuMessages::clearDeletedMessages(peer, topicId);
				for (const auto item : items) {
					item->destroy();
				}
				close();
			},
			.confirmText = tr::ayu_ClearDeletedMessagesActionText(tr::now),
			.cancelText = tr::lng_cancel(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	};
}

void DeleteMyMessagesAfterConfirm(not_null<PeerData*> peer) {
	const auto session = &peer->session();

	if (const auto channel = peer->asChannel()) {
		if (channel->isMegagroup() && channel->canDeleteMessages()) {
			session->api().deleteAllFromParticipant(channel, session->user());
			return;
		}
	}

	auto collected = std::make_shared<std::vector<MsgId>>();

	const auto removeNext = std::make_shared<Fn<void(int)>>();
	const auto requestNext = std::make_shared<Fn<void(MsgId)>>();

	*removeNext = [=](int index)
	{
		if (index >= int(collected->size())) {
			DEBUG_LOG(("Deleted all %1 my messages in this chat").arg(collected->size()));
			return;
		}

		QVector<MTPint> ids;
		ids.reserve(std::min<int>(100, collected->size() - index));
		for (auto i = 0; i < 100 && (index + i) < int(collected->size()); ++i) {
			ids.push_back(MTP_int((*collected)[index + i].bare));
		}

		const auto batch = index / 100 + 1;
		const auto done = [=](const MTPmessages_AffectedMessages &result)
		{
			session->api().applyAffectedMessages(peer, result);
			if (peer->isChannel()) {
				session->data().processMessagesDeleted(peer->id, ids);
			} else {
				session->data().processNonChannelMessagesDeleted(ids);
			}
			const auto deleted = index + ids.size();
			DEBUG_LOG(("Deleted batch %1, total deleted %2/%3").arg(batch).arg(deleted).arg(collected->size()));
			const auto delay = crl::time(500 + base::RandomValue<int>() % 500);
			base::call_delayed(delay, [=] { (*removeNext)(deleted); });
		};
		const auto fail = [=](const MTP::Error &error)
		{
			const auto type = error.type();
			DEBUG_LOG(("Delete batch %1 failed: %2").arg(batch).arg(type));

			if (type.startsWith(u"FLOOD_WAIT_"_q)
				|| type.startsWith(u"FLOOD_PREMIUM_WAIT_"_q)) {
				const auto underscore = type.lastIndexOf('_');
				const auto seconds = (underscore >= 0)
					? type.mid(underscore + 1).toInt()
					: 0;
				const auto delay = crl::time(std::max(seconds, 1) * 1000);
				base::call_delayed(delay, [=] { (*removeNext)(index); });
				return;
			}

			if (type == u"MESSAGE_DELETE_FORBIDDEN"_q
				|| type == u"MSG_ID_INVALID"_q
				|| type == u"MESSAGE_ID_INVALID"_q) {
				DEBUG_LOG(("Skipping batch %1 (%2 ids)").arg(batch).arg(ids.size()));
				const auto delay = crl::time(500 + base::RandomValue<int>() % 500);
				base::call_delayed(delay, [=] { (*removeNext)(index + ids.size()); });
				return;
			}

			DEBUG_LOG(("Stopping deletion, unrecoverable error: %1").arg(type));
		};

		if (const auto channel = peer->asChannel()) {
			session->api()
				.request(MTPchannels_DeleteMessages(channel->inputChannel(), MTP_vector<MTPint>(ids)))
				.done(done)
				.fail(fail)
				.handleFloodErrors()
				.send();
		} else {
			using Flag = MTPmessages_DeleteMessages::Flag;
			session->api()
				.request(MTPmessages_DeleteMessages(MTP_flags(Flag::f_revoke), MTP_vector<MTPint>(ids)))
				.done(done)
				.fail(fail)
				.handleFloodErrors()
				.send();
		}
	};

	*requestNext = [=](MsgId from)
	{
		using Flag = MTPmessages_Search::Flag;
		auto request = MTPmessages_Search(
			MTP_flags(Flag::f_from_id),
			peer->input(),
			MTP_string(),
			MTP_inputPeerSelf(),
			MTPInputPeer(),
			MTPVector<MTPReaction>(),
			MTP_int(0),
			// top_msg_id
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0),
			// min_date
			MTP_int(0),
			// max_date
			MTP_int(from.bare),
			MTP_int(0),
			// add_offset
			MTP_int(100),
			MTP_int(0),
			// max_id
			MTP_int(0),
			// min_id
			MTP_long(0)); // hash

		session->api()
			.request(std::move(request))
			.done([=](const Api::HistoryRequestResult &result)
			{
				auto parsed = Api::ParseHistoryResult(peer, from, Data::LoadDirection::Before, result);
				MsgId minId;
				int batchCount = 0;
				for (const auto &id : parsed.messageIds) {
					if (!minId || id < minId) minId = id;
					collected->push_back(id);
					++batchCount;
				}
				DEBUG_LOG(("Batch found %1 my messages, total %2").arg(batchCount).arg(collected->size()));
				if (parsed.messageIds.size() == 100 && minId) {
					(*requestNext)(minId - MsgId(1));
				} else {
					DEBUG_LOG(("Found %1 my messages in this chat (SEARCH)").arg(collected->size()));
					(*removeNext)(0);
				}
			})
			.fail([=](const MTP::Error &error) { DEBUG_LOG(("History fetch failed: %1").arg(error.type())); })
			.send();
	};

	(*requestNext)(MsgId(0));
}

Fn<void()> DeleteMyMessagesHandler(not_null<Window::SessionController*> controller, not_null<PeerData*> peer) {
	return [=]
	{
		if (!controller->showFrozenError()) {
			controller->show(Ui::MakeConfirmBox({
				.text = tr::ayu_DeleteOwnMessagesConfirmation(tr::now),
				.confirmed =
				[=](Fn<void()> &&close)
				{
					DeleteMyMessagesAfterConfirm(peer);
					close();
				},
				.confirmText = tr::lng_box_delete(),
				.cancelText = tr::lng_cancel(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}
	};
}

}

bool needToShowItem(ContextMenuVisibility state) {
	return state == ContextMenuVisibility::Visible
		|| (state == ContextMenuVisibility::VisibleWithModifier && base::IsExtendedContextMenuModifierPressed());
}

void AddAyuGramActions(PeerData *peerData,
							   Data::Thread *thread,
							   not_null<Window::SessionController*> sessionController,
							   const Window::PeerMenuCallback &addCallback) {
	if (!peerData) {
		return;
	}

	const auto &settings = AyuSettings::getInstance();
	const auto user = peerData->asUser();
	const auto showFilters = settings.filtersEnabled()
		&& (!user || user->isBot());
	const auto saveDeletedMessages = settings.saveDeletedMessages();
	const auto session = &peerData->session();
	const auto hiddenToggleShown = AyuState::hiddenMessagesShown(
		session,
		peerData->id);
	const auto allHiddenToggleShown = AyuState::allHiddenMessagesShown(session);
	const auto hasAnyHidden = AyuState::hasAnyHiddenMessagesAll(session);
	if (!peerData->isBroadcast()
		&& !showFilters && !saveDeletedMessages
		&& !hiddenToggleShown && !allHiddenToggleShown && !hasAnyHidden) {
		return;
	}

	const auto topic = peerData->isForum() && thread ? thread->asTopic() : nullptr;
	const auto topicId = topic ? topic->rootId().bare : 0;

	addCallback(Window::PeerMenuCallback::Args{
		.text = u"AyuGram"_q,
		.handler = nullptr,
		.icon = &st::menuIconGroupReactions,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
			if (peerData->isBroadcast()) {
				const auto enabled = AyuState::repostFilterEnabled(session, peerData->id);
				const auto filter = AyuState::repostFilter(session);
				const auto enabledAnywhere = filter.enabledAll
					|| !filter.includedFolders.empty()
					|| std::any_of(filter.channels.begin(), filter.channels.end(), [](const auto &entry) {
						return entry.second;
					});
				const auto history = session->data().historyLoaded(peerData->id);
				const auto excludedByFolder = history && ranges::any_of(
					session->data().chatsFilters().list(),
					[&](const Data::ChatFilter &folder) {
						return filter.excludedFolders.contains(folder.id()) && folder.contains(history);
					});
				if (!filter.paused && (enabled || (!filter.onlyFolders && !excludedByFolder))) {
					addAction(
						enabled
							? u"Выключить фильтрацию (Этот канал)"_q
							: u"Включить фильтрацию (Этот канал)"_q,
						[=] {
							auto updated = AyuState::repostFilter(session);
							updated.channels[peerData->id] = !enabled;
							AyuState::setRepostFilter(session, std::move(updated));
						},
						&st::menuIconCaptionHide);
				}
				if (enabledAnywhere || filter.paused) {
					addAction(
						filter.paused
							? u"Включить настроенную фильтрацию"_q
							: u"Выключить настроенную фильтрацию"_q,
						[=] {
							auto updated = AyuState::repostFilter(session);
							updated.paused = !filter.paused;
							AyuState::setRepostFilter(session, std::move(updated));
						},
						filter.paused ? &st::menuIconCaptionHide : &st::menuIconCaptionShow);
				}
				addAction(
					filter.enabledAll
						? u"Отключить режим всех каналов"_q
						: u"Фильтровать все каналы"_q,
					[=] {
						auto updated = AyuState::repostFilter(session);
						updated.enabledAll = !filter.enabledAll;
						if (updated.enabledAll) {
							updated.onlyFolders = false;
							updated.paused = false;
						}
						AyuState::setRepostFilter(session, std::move(updated));
					},
					&st::menuIconCaptionHide);
				const auto exception = filter.channels.find(peerData->id);
				const auto excluded = exception != filter.channels.end() && !exception->second;
				addAction(
					excluded ? u"Убрать канал из исключений"_q : u"Добавить канал в исключения"_q,
					[=] {
						auto updated = AyuState::repostFilter(session);
						if (excluded) {
							updated.channels.erase(peerData->id);
						} else {
							updated.channels[peerData->id] = false;
						}
						AyuState::setRepostFilter(session, std::move(updated));
					},
					&st::menuIconCaptionShow);
				addAction(
					u"Исключения: каналы"_q,
					[=] {
						sessionController->show(Box<PeerListBox>(
							std::make_unique<RepostExceptionsController>(session),
							[=](not_null<PeerListBox*> box) {
								box->setTitle(rpl::single(u"Исключённые каналы"_q));
								box->addButton(tr::lng_settings_save(), [=] {
									auto updated = AyuState::repostFilter(session);
									std::erase_if(updated.channels, [](const auto &entry) {
										return !entry.second;
									});
									for (const auto peer : box->collectSelectedRows()) {
										updated.channels[peer->id] = false;
									}
									AyuState::setRepostFilter(session, std::move(updated));
									box->closeBox();
								});
								box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
							}));
					},
					&st::menuIconCaptionShow);
				addAction(
					u"Настройки фильтрации (%1%, %2 дн.)"_q.arg(filter.percent).arg(filter.days),
					[=] {
						sessionController->show(Box(RepostFilterSettingsBox, session));
					},
					&st::menuIconSettings);
			}
			if (showFilters) {
				addAction(
					tr::ayu_ViewFiltersMenuText(tr::now),
					[=]
					{
						sessionController->dialogId = getDialogIdFromPeer(peerData);
						sessionController->showExclude = true;
						sessionController->shadowBan = false;
						sessionController->showSettings(Settings::AyuFiltersList::Id());
					},
					&st::menuIconAddToFolder);
			}
			const auto filteredToggleShown = FiltersController::filteredMessagesShown(peerData);
			if (filteredToggleShown) {
				addAction(
					*filteredToggleShown
						? tr::ayu_HideFilteredMessagesMenuText(tr::now)
						: tr::ayu_ShowFilteredMessagesMenuText(tr::now),
					[=]
					{
						FiltersController::toggleFilteredMessagesShown(peerData);
					},
					*filteredToggleShown
						? &st::menuIconCaptionHide
						: &st::menuIconCaptionShow);
			}
			if (hiddenToggleShown) {
				addAction(
					*hiddenToggleShown
						? u"Hide Hidden (This Chat)"_q
						: u"Show Hidden (This Chat)"_q,
					[=]
					{
						AyuState::showHiddenMessages(
							session,
							peerData->id,
							!*hiddenToggleShown);
						const auto owner = &peerData->session().data();
						if (const auto msgs = AyuState::getHiddenMessages(
							session,
							peerData->id)) {
							for (const auto &msgId : *msgs) {
								if (const auto item = owner->message(peerData->id, msgId)) {
									owner->requestItemResize(item);
								}
							}
						}
						FiltersCacheController::fireUpdate();
					},
					*hiddenToggleShown
						? &st::menuIconCaptionHide
						: &st::menuIconCaptionShow);
			}
			if (allHiddenToggleShown) {
				addAction(
					*allHiddenToggleShown
						? u"Hide Hidden (All)"_q
						: u"Show Hidden (All)"_q,
					[=]
					{
						AyuState::showAllHiddenMessages(
							session,
							!*allHiddenToggleShown);
						const auto owner = &peerData->session().data();
						for (const auto &[pId, msgs] : AyuState::getAllHiddenMessages(
							session)) {
							for (const auto &msgId : msgs) {
								if (const auto item = owner->message(pId, msgId)) {
									owner->requestItemResize(item);
								}
							}
						}
						FiltersCacheController::fireUpdate();
					},
					*allHiddenToggleShown
						? &st::menuIconCaptionHide
						: &st::menuIconCaptionShow);
			}
			if (hiddenToggleShown) {
				addAction(
					u"Clear Hidden (This Chat)"_q,
					[=]
					{
						const auto owner = &peerData->session().data();
						if (const auto msgs = AyuState::getHiddenMessages(
							session,
							peerData->id)) {
							for (const auto &msgId : *msgs) {
								if (const auto item = owner->message(peerData->id, msgId)) {
									owner->requestItemResize(item);
								}
							}
						}
						AyuState::clearHidden(session, peerData->id);
						FiltersCacheController::fireUpdate();
					},
					&st::menuIconDelete);
			}

			if (hasAnyHidden) {
				addAction(
					u"Clear Hidden (All)"_q,
					[=]
					{
						const auto owner = &peerData->session().data();
						for (const auto &[pId, msgs] : AyuState::getAllHiddenMessages(
							session)) {
							for (const auto &msgId : msgs) {
								if (const auto item = owner->message(pId, msgId)) {
									owner->requestItemResize(item);
								}
							}
						}
						AyuState::clearAllHidden(session);
						FiltersCacheController::fireUpdate();
					},
					&st::menuIconDelete);
			}
			if (saveDeletedMessages) {
				addAction(
					tr::ayu_ViewDeletedMenuText(tr::now),
					[=]
					{
						if (const auto window = sessionController->session().tryResolveWindow()) {
							window->showSection(std::make_shared<MessageHistory::SectionMemento>(
								peerData,
								nullptr,
								topicId));
						}
					},
					&st::menuIconArchive);
				if (showFilters || filteredToggleShown.value_or(false)) addAction({ .isSeparator = true });
				addAction({
					.text = tr::ayu_ClearDeletedMenuText(tr::now),
					.handler = ClearDeletedMessagesHandler(sessionController, peerData, topicId),
					.icon = &st::menuIconClearAttention,
					.isAttention = true,
				});
			}
		},
	});
}

void AddJumpToBeginningAction(PeerData *peerData,
							  Data::Thread *thread,
							  not_null<Window::SessionController*> sessionController,
							  const Window::PeerMenuCallback &addCallback) {
	const auto user = peerData->asUser();
	const auto group = peerData->isChat() ? peerData->asChat() : nullptr;
	const auto chat = peerData->isMegagroup()
						  ? peerData->asMegagroup()
						  : peerData->isChannel()
								? peerData->asChannel()
								: nullptr;
	const auto topic = peerData->isForum() ? thread->asTopic() : nullptr;
	if (!user && !group && !chat && !topic) {
		return;
	}
	if (topic && topic->creating()) {
		return;
	}

	const auto controller = sessionController;
	const auto jumpToDate = [=](auto history, auto callback)
	{
		const auto weak = base::make_weak(controller);
		controller->session().api().resolveJumpToDate(
			history,
			QDate(2013, 8, 1),
			[=](not_null<PeerData*> peer, MsgId id)
			{
				if (weak.get()) {
					// API returns 0 if message "Channel created" (ID: 1) was deleted, which scrolls to the bottom
					if (id.bare == 0) {
						id = MsgId(2);
					}
					callback(peer, id);
				}
			});
	};

	const auto showPeerHistory = [=](auto peer, MsgId id)
	{
		controller->showPeerHistory(
			peer,
			Window::SectionShow::Way::Forward,
			id);
	};

	const auto showTopic = [=](auto topic, MsgId id)
	{
		controller->showTopic(
			topic,
			id,
			Window::SectionShow::Way::Forward);
	};

	addCallback(
		tr::ayu_JumpToBeginning(tr::now),
		[=]
		{
			if (user) {
				jumpToDate(controller->session().data().history(user), showPeerHistory);
			} else if (group && !chat) {
				jumpToDate(controller->session().data().history(group), showPeerHistory);
			} else if (chat && !topic) {
				if (!chat->migrateFrom() && chat->availableMinId() == 1) {
					showPeerHistory(chat, 1);
				} else {
					jumpToDate(controller->session().data().history(chat), showPeerHistory);
				}
			} else if (topic) {
				if (topic->isGeneral()) {
					showTopic(topic, 1);
				} else {
					jumpToDate(
						topic,
						[=](not_null<PeerData*>, MsgId id)
						{
							showTopic(topic, id);
						});
				}
			}
		},
		&st::ayuToBeginningMenuIcon);
}

void AddOpenChannelAction(PeerData *peerData,
						  not_null<Window::SessionController*> sessionController,
						  const Window::PeerMenuCallback &addCallback) {
	if (!peerData || !peerData->isMegagroup()) {
		return;
	}

	const auto chat = peerData->asMegagroup()->discussionLink();
	if (!chat) {
		return;
	}

	addCallback(
		tr::lng_context_open_channel(tr::now),
		[=]
		{
			sessionController->showPeerHistory(chat, Window::SectionShow::Way::Forward);
		},
		&st::menuIconChannel);
}

void AddShadowBanAction(PeerData *peerData,
						const Window::PeerMenuCallback &addCallback) {
	const auto &settings = AyuSettings::getInstance();
	if (!peerData || !(peerData->isUser() || peerData->isBroadcast()) || !settings.filtersEnabled()) {
		return;
	}

	if (const auto user = peerData->asUser()) {
		if (user->isSelf()) {
			return;
		}
	}

	const auto realId = getDialogIdFromPeer(peerData);
	const auto shadowBanned = AyuSettings::getInstance().isShadowBanned(realId);
	const auto toggleShadowBan = [=]
	{
		if (shadowBanned) {
			AyuSettings::getInstance().removeShadowBan(realId);
		} else {
			AyuSettings::getInstance().addShadowBan(realId);
		}
	};

	addCallback({
		.text = (shadowBanned
					 ? tr::ayu_FiltersQuickUnshadowBan(tr::now)
					 : tr::ayu_FiltersQuickShadowBan(tr::now)),
		.handler = toggleShadowBan,
		.icon = shadowBanned ? &st::menuIconShowInChat : &st::menuIconStealth,
	});
}

void AddDeleteOwnMessagesAction(PeerData *peerData,
								Data::ForumTopic *topic,
								not_null<Window::SessionController*> sessionController,
								const Window::PeerMenuCallback &addCallback) {
	if (topic) {
		return;
	}
	if (const auto chat = peerData->asChat()) {
		if (!chat->amIn()) {
			return;
		}
	} else if (const auto channel = peerData->asChannel()) {
		if (!channel->isMegagroup() || !channel->amIn()) {
			return;
		}
	} else {
		return;
	}
	addCallback(
		tr::ayu_DeleteOwnMessages(tr::now),
		DeleteMyMessagesHandler(sessionController, peerData),
		&st::menuIconTTL);
}

void AddDeleteChannelPostsAction(
		PeerData *peerData,
		Data::ForumTopic *topic,
		not_null<Window::SessionController*> sessionController,
		const Window::PeerMenuCallback &addCallback) {
	const auto channel = peerData ? peerData->asBroadcast() : nullptr;
	if (topic || !channel) {
		return;
	}
	const auto hasPanel = HasDeleteChannelPostsPanel(channel);
	if (!hasPanel
		&& (!channel->amIn() || !channel->canDeleteMessages())) {
		return;
	}
	addCallback(Window::PeerMenuCallback::Args{
		.text = tr::ayu_DeleteChannelPosts(tr::now),
		.handler = [=] {
			if (HasDeleteChannelPostsPanel(channel)
				|| (channel->amIn()
					&& channel->canDeleteMessages()
					&& !sessionController->showFrozenError())) {
				ShowDeleteChannelPostsPanel(channel);
			}
		},
		.icon = &st::menuIconDeleteAttention,
		.isAttention = true,
	});
}

void AddHistoryAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	if (item->hideEditedBadge()) {
		return;
	}

	const auto edited = item->Get<HistoryMessageEdited>();
	if (!edited) {
		return;
	}

	const auto has = AyuMessages::hasRevisions(item);
	if (!has) {
		return;
	}

	menu->addAction(
		tr::ayu_EditsHistoryMenuText(tr::now),
		[=]
		{
			if (const auto window = item->history()->session().tryResolveWindow()) {
				window->showSection(
					std::make_shared<MessageHistory::SectionMemento>(item->history()->peer, item, 0));
			}
		},
		&st::ayuEditsHistoryIcon);
}

void AddHideMessageAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	const auto &settings = AyuSettings::getInstance();
	if (!needToShowItem(settings.showHideMessageInContextMenu())) {
		return;
	}

	if (item->history()->peer->isSelf()) {
		return;
	}

	const auto history = item->history();
	const auto session = &history->session();
	const auto owner = &history->owner();
	const auto itemId = item->fullId();
	menu->addAction(
		tr::ayu_ContextHideMessage(tr::now),
		[=]()
		{
			if (const auto item = owner->message(itemId)) {
				const auto ids = owner->itemOrItsGroup(item);
				AyuState::setHidden(session, ids, true);
				for (const auto &fullId : ids) {
					if (const auto current = owner->message(fullId)) {
						current->destroy();
					}
				}
			}
			history->requestChatListMessage();
		},
		&st::menuIconClear);
}

void AddUserMessagesAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	const auto &settings = AyuSettings::getInstance();
	if (!needToShowItem(settings.showUserMessagesInContextMenu())) {
		return;
	}

	if (!item->isHistoryEntry()) {
		return;
	}

	if (item->history()->peer->isChat() || item->history()->peer->isMegagroup()) {
		menu->addAction(
			tr::ayu_UserMessagesMenuText(tr::now),
			[=]
			{
				if (const auto controller = item->history()->session().tryResolveWindow()) {
					const auto peer = item->history()->peer;
					const auto key = (peer && !peer->isUser())
										 ? item->topic()
											   ? Dialogs::Key{item->topic()}
											   : Dialogs::Key{item->history()}
										 : Dialogs::Key{item->history()};
					controller->searchInChat(key, item->from());
				}
			},
			&st::menuIconTTL);
	}
}

void AddMessageDetailsAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	const auto &settings = AyuSettings::getInstance();
	if (!needToShowItem(settings.showMessageDetailsInContextMenu())) {
		return;
	}

	if (item->isLocal()) {
		return;
	}

	const auto view = item->mainView();
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	const auto views = item->Get<HistoryMessageViews>();
	const auto media = item->media();

	const auto isSticker = media && media->document() && media->document()->sticker();

	const auto emojiPacks = HistoryView::CollectEmojiPacks(item, HistoryView::EmojiPacksSource::Message);
	auto containsSingleCustomEmojiPack = emojiPacks.size() == 1;
	if (!containsSingleCustomEmojiPack && emojiPacks.size() > 1) {
		const auto author = emojiPacks.front().id >> 32;
		auto sameAuthor = true;
		for (const auto &pack : emojiPacks) {
			if (pack.id >> 32 != author) {
				sameAuthor = false;
				break;
			}
		}

		containsSingleCustomEmojiPack = sameAuthor;
	}

	const auto isForwarded = forwarded && !forwarded->story && forwarded->psaType.isEmpty();

	const auto messageId = QString::number(item->id.bare);
	const auto messageDate = base::unixtime::parse(item->date());
	const auto messageEditDate = base::unixtime::parse(view ? view->displayedEditDate() : TimeId(0));

	const auto messageForwardedDate =
		isForwarded && forwarded
			? base::unixtime::parse(forwarded->originalDate)
			: QDateTime();

	const auto
		messageViews = item->hasViews() && item->viewsCount() > 0 ? QString::number(item->viewsCount()) : QString();
	const auto messageForwards = views && views->forwardsCount > 0 ? QString::number(views->forwardsCount) : QString();

	const auto mediaSize = media ? getMediaSize(item) : QString();
	const auto mediaMime = media ? getMediaMime(item) : QString();
	// todo: bitrate (?)
	const auto mediaName = media ? getMediaName(item) : QString();
	const auto mediaResolution = media ? getMediaResolution(item) : QString();
	const auto mediaDC = media ? getMediaDC(item) : QString();

	const auto hasAnyPostField =
		!messageViews.isEmpty() ||
		!messageForwards.isEmpty();

	const auto hasAnyMediaField =
		!mediaSize.isEmpty() ||
		!mediaMime.isEmpty() ||
		!mediaName.isEmpty() ||
		!mediaResolution.isEmpty() ||
		!mediaDC.isEmpty();

	const auto callback = Ui::Menu::CreateAddActionCallback(menu);

	callback(Window::PeerMenuCallback::Args{
		.text = tr::ayu_MessageDetailsPC(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconInfo,
		.fillSubmenu = [&](not_null<Ui::PopupMenu*> menu2)
		{
			if (hasAnyPostField) {
				if (!messageViews.isEmpty()) {
					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::menuIconShowInChat,
						tr::ayu_MessageDetailsViewsPC(tr::now),
						messageViews
					));
				}

				if (!messageForwards.isEmpty()) {
					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::menuIconViewReplies,
						tr::ayu_MessageDetailsSharesPC(tr::now),
						messageForwards
					));
				}

				menu2->addSeparator();
			}

			menu2->addAction(Ui::ContextActionWithSubText(
				menu2->menu(),
				st::menuIconInfo,
				QString("ID"),
				messageId
			));

			menu2->addAction(Ui::ContextActionWithSubText(
				menu2->menu(),
				st::menuIconSchedule,
				tr::ayu_MessageDetailsDatePC(tr::now),
				formatDateTime(messageDate)
			));

			if (view && view->displayedEditDate()) {
				menu2->addAction(Ui::ContextActionWithSubText(
					menu2->menu(),
					st::menuIconEdit,
					tr::ayu_MessageDetailsEditedDatePC(tr::now),
					formatDateTime(messageEditDate)
				));
			}

			if (isForwarded) {
				menu2->addAction(Ui::ContextActionWithSubText(
					menu2->menu(),
					st::menuIconTTL,
					tr::ayu_MessageDetailsForwardedDatePC(tr::now),
					formatDateTime(messageForwardedDate)
				));
			}

			if (media && hasAnyMediaField) {
				menu2->addSeparator();

				if (!mediaSize.isEmpty()) {
					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::menuIconDownload,
						tr::ayu_MessageDetailsFileSizePC(tr::now),
						mediaSize
					));
				}

				if (!mediaMime.isEmpty()) {
					const auto mime = Core::MimeTypeForName(mediaMime);

					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::menuIconShowAll,
						tr::ayu_MessageDetailsMimeTypePC(tr::now),
						mime.name()
					));
				}

				if (!mediaName.isEmpty()) {
					auto const shortified = mediaName.length() > 20 ? "…" + mediaName.right(20) : mediaName;

					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::ayuEditsHistoryIcon,
						tr::ayu_MessageDetailsFileNamePC(tr::now),
						shortified,
						[=]
						{
							QGuiApplication::clipboard()->setText(mediaName);
						}
					));
				}

				if (!mediaResolution.isEmpty()) {
					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::menuIconStats,
						tr::ayu_MessageDetailsResolutionPC(tr::now),
						mediaResolution
					));
				}

				if (!mediaDC.isEmpty()) {
					menu2->addAction(Ui::ContextActionWithSubText(
						menu2->menu(),
						st::menuIconBoosts,
						tr::ayu_MessageDetailsDatacenterPC(tr::now),
						mediaDC
					));
				}

				if (isSticker) {
					const auto authorId = getUserIdFromPackId(media->document()->sticker()->set.id);

					if (authorId != 0) {
						menu2->addAction(Ui::ContextActionStickerAuthor(
							menu2->menu(),
							&item->history()->session(),
							authorId
						));
					}
				}
			}

			if (containsSingleCustomEmojiPack) {
				const auto authorId = getUserIdFromPackId(emojiPacks.front().id);

				if (authorId != 0) {
					menu2->addAction(Ui::ContextActionStickerAuthor(
						menu2->menu(),
						&item->history()->session(),
						authorId
					));
				}
			}
		},
	});
}

void AddRepeatMessageAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item, HistoryView::Context context) {
	const auto &settings = AyuSettings::getInstance();
	if (!needToShowItem(settings.showRepeatMessageInContextMenu())) {
		return;
	}

	if (!item || !item->isHistoryEntry() || item->isService() || item->isLocal() || !item->allowsForward() || item->id <= 0) {
		return;
	}

	const auto history = item->history();
	const auto peer = history->peer;
	if (!peer->isUser() && !peer->isChat() && !peer->isMegagroup() && !peer->isGigagroup()) {
		return;
	}

	const auto itemId = item->fullId();
	const auto session = &history->session();

	menu->addAction(
		tr::ayu_RepeatMessage(tr::now),
		[=]
		{
			const auto sendAs = (peer->isUser() || peer->isChat() || history->peer->isMonoforum())
				? nullptr
				: session->sendAsPeers().resolveChosen(peer).get();

			const auto inRepliesView = (context == HistoryView::Context::Replies);
			const auto replyTo = item->replyTo();
			const auto hasReply = replyTo.messageId.msg != 0;
			const auto shiftPressed = base::IsShiftPressed();

			const auto useNoQuote = shiftPressed || (inRepliesView && !history->peer->isForum());
			const auto preserveReply = inRepliesView ? hasReply : (hasReply && shiftPressed);

			const auto currentItem = history->owner().message(itemId);
			if (!currentItem) {
				return;
			}

			auto action = Api::SendAction(
				history,
				Api::SendOptions{ .sendAs = sendAs });
			if (history->peer->amMonoforumAdmin()) {
				action.replyTo.monoforumPeerId = currentItem->sublistPeerId();
			}
			action.clearDraft = false;

			applyGhostScheduling(session, action.options);

			if (currentItem->topic()) {
				action.replyTo.topicRootId = currentItem->topicRootId();
			}

			if (preserveReply) {
				action.replyTo.messageId = replyTo.messageId;
			}

			if (useNoQuote) {
				if (currentItem->richPage() && session->premium()) {
					if (preserveReply) {
						crl::async([=] {
							AyuForward::forwardRichMessage(session, itemId, action);
						});
					} else {
						const auto forwardDraft = Data::ForwardDraft{
							.ids = MessageIdsList{ itemId },
							.options = Data::ForwardOptions::NoSenderNames,
						};
						auto resolvedDraft = history->resolveForwardDraft(forwardDraft);
						session->api().forwardMessages(
							std::move(resolvedDraft),
							action,
							[] {});
					}
				} else {
					auto message = ApiWrap::MessageToSend(action);
					const auto media = currentItem->media();
					if (!currentItem->originalText().text.isEmpty()) {
						message.textWithTags = {
							currentItem->originalText().text,
							TextUtilities::ConvertEntitiesToTextTags(
								currentItem->originalText().entities),
						};
					}
					if (media) {
						if (const auto photo = media->photo()) {
							Api::SendExistingPhoto(std::move(message), photo);
						} else if (const auto document = media->document()) {
							Api::SendExistingDocument(std::move(message), document);
						}
					} else {
						session->api().sendMessage(std::move(message));
					}
				}
			} else {
				const auto forwardDraft = Data::ForwardDraft{
					.ids = MessageIdsList{ itemId },
					.options = Data::ForwardOptions::PreserveInfo,
				};
				auto resolvedDraft = history->resolveForwardDraft(forwardDraft);

				if (AyuForward::isFullAyuForwardNeeded(currentItem)) {
					crl::async([=]
					{
						AyuForward::forwardMessages(session, action, false, resolvedDraft);
					});
				} else if (AyuForward::isAyuForwardNeeded(currentItem)) {
					crl::async([=]
					{
						AyuForward::intelligentForward(session, action, resolvedDraft);
					});
				} else {
					session->api().forwardMessages(std::move(resolvedDraft), action, [] {});
				}
			}
		},
		&st::ayuRepeatMenuIcon);
}

void AddReadUntilAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	const auto group = item->history()->owner().groups().find(item);
	const auto readItem = group ? group->items.back().get() : item;
	if (!readItem->isHistoryEntry()
		|| readItem->isLocal()
		|| readItem->out()
		|| readItem->isDeleted()
		|| readItem->history()->peer->isSelf()) {
		return;
	}

	const auto &ghost = AyuSettings::ghost(&readItem->history()->session());
	if (ghost.sendReadMessages()) {
		return;
	}

	menu->addAction(
		tr::ayu_ReadUntilMenuText(tr::now),
		[=]
		{
			readHistory(readItem);
			const auto media = readItem->media();
			if (media
				&& media->ttlSeconds() <= 0
				&& !readItem->out()) {
				const auto ids = MTP_vector<MTPint>(1, MTP_int(readItem->id));
				if (const auto channel = readItem->history()->peer->asChannel()) {
					readItem->history()->session().api().request(
						MTPchannels_ReadMessageContents(
						channel->inputChannel(),
						ids)).send();
				} else {
					readItem->history()->session().api().request(
						MTPmessages_ReadMessageContents(ids)
					).done([=](const MTPmessages_AffectedMessages &result)
					{
						readItem->history()->session().api()
							.applyAffectedMessages(
							readItem->history()->peer,
							result);
					}).send();
				}
				readItem->markContentsRead();
			}
		},
		&st::menuIconShowInChat);
}

void AddBurnAction(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	if (!item->media() || item->media()->ttlSeconds() <= 0 || item->out() ||
		!item->hasUnreadMediaFlag()) {
		return;
	}

	menu->addAction(
		tr::ayu_ExpireMediaContextMenuText(tr::now),
		[=]
		{
			const auto ids = MTP_vector<MTPint>(1, MTP_int(item->id));

			item->history()->session().api().request(MTPmessages_ReadMessageContents(
					ids
				)).done([=](const MTPmessages_AffectedMessages &result)
				{
					item->history()->session().api().applyAffectedMessages(
						item->history()->peer,
						result);
					item->markContentsRead();
				}).send();
		},
		&st::menuIconTTLAny);
}

void AddCreateFilterAction(not_null<Ui::PopupMenu*> menu,
						   not_null<Window::SessionController*> controller,
						   HistoryItem *item,
						   const QString &selectedText) {
	const auto &settings = AyuSettings::getInstance();
	if (!needToShowItem(settings.showAddFilterInContextMenu()) || !settings.filtersEnabled()) {
		return;
	}

	if (!item || selectedText.isEmpty()) {
		return;
	}

	menu->addAction(
		tr::ayu_RegexFilterQuickAdd(tr::now),
		[=]
		{
			RegexFilter filter;
			filter.text = selectedText.toStdString();
			filter.enabled = true;
			filter.caseInsensitive = true;
			filter.reversed = false;

			controller->show(Settings::RegexEditBox(&filter, {}, getDialogIdFromPeer(item->history()->peer), true));
		},
		&st::menuIconAddToFolder);
}

} // namespace AyuUi
