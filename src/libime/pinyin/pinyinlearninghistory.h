/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _LIBIME_PINYIN_PINYINLEARNINGHISTORY_H_
#define _LIBIME_PINYIN_PINYINLEARNINGHISTORY_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <libime/core/historybigram.h>
#include <libime/pinyin/libimepinyin_export.h>

namespace libime {

class PinyinIME;

/// Unicode character offsets within the committed text.
struct PinyinPendingLearning {
    HistoryBigram::LearningEvent event;
    size_t from = 0;
    size_t to = 0;
    std::optional<std::pair<std::string, std::string>> userWord;
    std::vector<std::pair<std::string, std::string>> restoreBlockedWords;
};

class PinyinLearningHistoryPrivate;

/** Tracks learning receipts through confirmed surrounding-text edits. */
class LIBIMEPINYIN_EXPORT PinyinLearningHistory {
public:
    explicit PinyinLearningHistory(PinyinIME *ime);
    ~PinyinLearningHistory();
    PinyinLearningHistory(const PinyinLearningHistory &) = delete;
    PinyinLearningHistory &operator=(const PinyinLearningHistory &) = delete;

    void update(std::string_view text, size_t cursor, size_t anchor);
    void committed(std::string_view text,
                   std::vector<PinyinPendingLearning> events = {});
    void expectDeletion();
    void invalidate();

private:
    std::unique_ptr<PinyinLearningHistoryPrivate> d_ptr;
};

} // namespace libime

#endif
