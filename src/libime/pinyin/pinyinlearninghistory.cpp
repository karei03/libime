/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "pinyinlearninghistory.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <fcitx-utils/utf8.h>
#include <libime/core/userlanguagemodel.h>
#include "pinyindictionary.h"
#include "pinyinime.h"

namespace libime {
namespace {
std::vector<size_t> offsets(std::string_view text) {
    const auto length = fcitx::utf8::lengthValidated(text);
    if (length == fcitx::utf8::INVALID_LENGTH) {
        return {};
    }
    std::vector<size_t> result;
    result.reserve(length + 1);
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) {
            result.push_back(i);
        }
    }
    result.push_back(text.size());
    return result;
}
struct Range {
    size_t from;
    size_t to;
};
struct Tracked {
    PinyinPendingLearning event;
    std::vector<Range> remaining;
};
struct Acknowledgement {
    std::string text;
    size_t cursor;
    size_t anchor;
};
} // namespace

class PinyinLearningHistoryPrivate {
public:
    explicit PinyinLearningHistoryPrivate(PinyinIME *value) : ime(value) {}

    PinyinIME *ime;
    bool valid = false;
    bool deletionExpected = false;
    std::string text;
    size_t cursor = 0;
    size_t anchor = 0;
    std::vector<Tracked> events;
    std::vector<Acknowledgement> acknowledgements;
    std::vector<PinyinPendingLearning> cancelled;

    // Map every tracked range through one surrounding-text replacement. Text
    // inserted by the edit is deliberately not attributed to older learning.
    void replace(size_t from, size_t removed, size_t inserted) {
        if (!removed && !inserted) {
            return;
        }
        const auto removedEnd = from + removed;
        for (auto &tracked : events) {
            std::vector<Range> mapped;
            mapped.reserve(tracked.remaining.size() * 2);
            for (const auto &range : tracked.remaining) {
                const auto leftEnd = std::min(range.to, from);
                if (range.from < leftEnd) {
                    mapped.push_back({range.from, leftEnd});
                }
                const auto rightBegin = std::max(range.from, removedEnd);
                if (range.to > rightBegin) {
                    mapped.push_back({from + inserted + rightBegin - removedEnd,
                                      from + inserted + range.to - removedEnd});
                }
            }
            tracked.remaining = std::move(mapped);
            if (tracked.remaining.empty()) {
                cancelled.push_back(std::move(tracked.event));
            }
        }
        std::erase_if(events, [](const auto &tracked) {
            return tracked.remaining.empty();
        });
    }

    bool tracks(const std::pair<std::string, std::string> &word) const {
        return std::any_of(events.begin(), events.end(),
                           [&](const auto &tracked) {
                               return tracked.event.userWord == word;
                           });
    }

    bool restores(std::string_view code, std::string_view word) const {
        return std::any_of(
            events.begin(), events.end(), [&](const auto &tracked) {
                return std::ranges::any_of(tracked.event.restoreBlockedWords,
                                           [&](const auto &blocked) {
                                               return blocked.first == code &&
                                                      blocked.second == word;
                                           });
            });
    }

    void confirm() {
        auto *history = &ime->model()->history();
        auto *dict = ime->dict();
        for (auto iter = cancelled.rbegin(); iter != cancelled.rend(); ++iter) {
            history->undo(std::move(iter->event));
        }
        for (const auto &pending : cancelled) {
            if (pending.userWord && !tracks(*pending.userWord)) {
                dict->removeWord(PinyinDictionary::UserDict,
                                 pending.userWord->first,
                                 pending.userWord->second);
            }
            for (const auto &[code, word] : pending.restoreBlockedWords) {
                if (!restores(code, word)) {
                    dict->setWordBlocked(code, word, true);
                }
            }
        }
        cancelled.clear();
        acknowledgements.clear();
    }
};

PinyinLearningHistory::PinyinLearningHistory(PinyinIME *ime)
    : d_ptr(std::make_unique<PinyinLearningHistoryPrivate>(ime)) {}
PinyinLearningHistory::~PinyinLearningHistory() = default;

void PinyinLearningHistory::invalidate() {
    auto &d = *d_ptr;
    d.valid = false;
    d.events.clear();
    d.cancelled.clear();
    d.acknowledgements.clear();
    d.deletionExpected = false;
    d.text.clear();
}

void PinyinLearningHistory::update(std::string_view text, size_t cursor,
                                   size_t anchor) {
    auto &d = *d_ptr;
    const auto next = offsets(text);
    if (next.empty() || cursor >= next.size() || anchor >= next.size()) {
        invalidate();
        return;
    }
    const bool textChanged = !d.valid || text != d.text;
    if (d.valid && text != d.text) {
        const auto ack =
            std::find_if(d.acknowledgements.begin(), d.acknowledgements.end(),
                         [&](const auto &value) {
                             return value.text == text &&
                                    value.cursor == cursor &&
                                    value.anchor == anchor;
                         });
        if (!d.deletionExpected && ack != d.acknowledgements.end()) {
            return;
        }

        const auto previous = offsets(d.text);
        const auto beforeLength = previous.size() - 1;
        const auto afterLength = next.size() - 1;
        const auto from = std::min(d.cursor, d.anchor);
        const auto to = std::max(d.cursor, d.anchor);
        auto matches = [&](size_t start, size_t end, size_t inserted) {
            return cursor == anchor && cursor == start + inserted &&
                   text.substr(0, next[start]) ==
                       std::string_view(d.text).substr(0, previous[start]) &&
                   text.substr(next[start + inserted]) ==
                       std::string_view(d.text).substr(previous[end]);
        };

        size_t editFrom = 0;
        size_t removed = 0;
        size_t inserted = 0;
        bool mapped = false;
        if (from != to && afterLength == beforeLength - (to - from) &&
            matches(from, to, 0)) {
            editFrom = from;
            removed = to - from;
            mapped = true;
        } else if (from == to && d.deletionExpected &&
                   afterLength < beforeLength && cursor == anchor) {
            const auto count = beforeLength - afterLength;
            if (count <= from && cursor == from - count &&
                matches(cursor, from, 0)) {
                editFrom = cursor;
                removed = count;
                mapped = true;
            } else if (count <= beforeLength - from && cursor == from &&
                       matches(from, from + count, 0)) {
                editFrom = from;
                removed = count;
                mapped = true;
            }
        } else if (from == to && afterLength > beforeLength) {
            inserted = afterLength - beforeLength;
            mapped = matches(from, from, inserted);
            editFrom = from;
        }
        if (!mapped) {
            if (ack != d.acknowledgements.end()) {
                return;
            }
            invalidate();
        } else {
            d.replace(editFrom, removed, inserted);
        }
    }
    d.text = text;
    d.cursor = cursor;
    d.anchor = anchor;
    d.valid = true;
    d.confirm();
    if (textChanged) {
        d.deletionExpected = false;
    }
}

void PinyinLearningHistory::committed(
    std::string_view text, std::vector<PinyinPendingLearning> events) {
    auto &d = *d_ptr;
    const auto inserted = offsets(text);
    if (!d.valid || inserted.empty()) {
        invalidate();
        return;
    }
    const auto length = inserted.size() - 1;
    if (std::any_of(events.begin(), events.end(), [&](const auto &event) {
            return event.from >= event.to || event.to > length;
        })) {
        invalidate();
        return;
    }

    d.deletionExpected = false;
    d.acknowledgements.push_back({d.text, d.cursor, d.anchor});
    const auto previous = offsets(d.text);
    const auto from = std::min(d.cursor, d.anchor);
    const auto to = std::max(d.cursor, d.anchor);
    d.replace(from, to - from, length);
    d.text.replace(previous[from], previous[to] - previous[from], text);
    d.cursor = d.anchor = from + length;
    for (auto &event : events) {
        d.events.push_back(
            {std::move(event), {{from + event.from, from + event.to}}});
    }
}

void PinyinLearningHistory::expectDeletion() { d_ptr->deletionExpected = true; }
} // namespace libime
