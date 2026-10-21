/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "historybigram.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/stringutils.h>
#include "constants.h"
#include "lattice.h"
#include "utils_p.h"
#include "zstdfilter.h"

namespace libime {
namespace {
using Count = HistoryBigram::Count;
using WordWithCode = HistoryBigram::WordWithCode;
using WordWithCodeView = HistoryBigram::WordWithCodeView;
template <typename T>
using StringMap = std::map<std::string, T, std::less<>>;
constexpr uint32_t historyBinaryFormatMagic = 0x000fc315;
constexpr uint32_t historyBinaryFormatVersion = 5;
constexpr std::string_view textHeader = "# libime-history-counts-v1";
constexpr char wordCodeSeparator = '\x02';

void increase(Count &value, Count delta) {
    if (delta > std::numeric_limits<Count>::max() - value) {
        throw std::overflow_error("History count overflow.");
    }
    value += delta;
}

template <typename Map, typename Key>
Count lookup(const Map &map, const Key &key) {
    const auto iter = map.find(key);
    return iter == map.end() ? 0 : iter->second;
}

// A row stores exact code counts and their sum. The sum is never a learned
// percentage: it is an incrementally maintained denominator/query aggregate.
struct Counts {
    StringMap<Count> codes;
    Count total = 0;

    Count exact(std::string_view code) const { return lookup(codes, code); }
    Count frequency(std::string_view code) const {
        return code.empty() ? total : exact("") + exact(code);
    }
    void add(std::string_view code, Count n) {
        increase(total, n);
        increase(codes[std::string(code)], n);
    }
    void halve(std::string_view code) {
        for (auto iter = codes.begin(); iter != codes.end();) {
            if (!code.empty() && iter->first != code) {
                ++iter;
                continue;
            }
            const auto half = iter->second / 2;
            total -= iter->second - half;
            if (!half) {
                iter = codes.erase(iter);
            } else {
                iter->second = half;
                ++iter;
            }
        }
    }
};

struct BigramCounts {
    // Previous code -> current code -> count. Column totals make partially
    // specified code queries efficient too; scoring never scans code variants.
    StringMap<Counts> rows;
    StringMap<Count> columns;
    Count total = 0;

    Count exact(std::string_view prev, std::string_view cur) const {
        const auto row = rows.find(prev);
        return row == rows.end() ? 0 : row->second.exact(cur);
    }
    Count frequency(std::string_view prev, std::string_view cur) const {
        if (prev.empty() && cur.empty()) {
            return total;
        }
        const auto uncoded = exact("", "");
        if (prev.empty()) {
            return uncoded + lookup(columns, cur);
        }
        if (cur.empty()) {
            const auto row = rows.find(prev);
            return uncoded + (row == rows.end() ? 0 : row->second.total);
        }
        return uncoded + exact(prev, cur);
    }
    void add(std::string_view prev, std::string_view cur, Count n) {
        increase(total, n);
        rows[std::string(prev)].add(cur, n);
        increase(columns[std::string(cur)], n);
    }
    void subtract(std::string_view prev, std::string_view cur, Count n) {
        auto row = rows.find(prev);
        if (row == rows.end()) {
            return;
        }
        auto item = row->second.codes.find(cur);
        if (item == row->second.codes.end()) {
            return;
        }
        n = std::min(n, item->second);
        item->second -= n;
        row->second.total -= n;
        total -= n;
        auto column = columns.find(cur);
        column->second -= n;
        if (!column->second) {
            columns.erase(column);
        }
        if (!item->second) {
            row->second.codes.erase(item);
        }
        if (!row->second.total) {
            rows.erase(row);
        }
    }
    void halve(std::string_view curCode) {
        // Collect changes first: subtract() can invalidate rows and columns.
        std::vector<std::pair<WordWithCode, Count>> changes;
        for (const auto &[prev, row] : rows) {
            for (const auto &[cur, n] : row.codes) {
                if (curCode.empty() || cur == curCode) {
                    changes.push_back({{prev, cur}, n - n / 2});
                }
            }
        }
        for (const auto &[codes, n] : changes) {
            subtract(codes.first, codes.second, n);
        }
    }
};

void writeCount(std::ostream &out, Count count) {
    throw_if_io_fail(marshall(out, static_cast<uint32_t>(count >> 32)));
    throw_if_io_fail(marshall(out, static_cast<uint32_t>(count)));
}
Count readCount(std::istream &in) {
    uint32_t high = 0, low = 0;
    throw_if_io_fail(unmarshall(in, high));
    throw_if_io_fail(unmarshall(in, low));
    return (static_cast<Count>(high) << 32) | low;
}
WordWithCode readWord(std::istream &in) {
    WordWithCode word;
    throw_if_io_fail(unmarshallString(in, word.first));
    throw_if_io_fail(unmarshallString(in, word.second));
    return word;
}
void writeWord(std::ostream &out, WordWithCodeView word) {
    throw_if_io_fail(marshallString(out, word.first));
    throw_if_io_fail(marshallString(out, word.second));
}
std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> result;
    while (!line.empty()) {
        std::string token;
        const auto consumed = fcitx::stringutils::consumeMaybeEscapedValue(
            line, FCITX_WHITESPACE, &token);
        if (!consumed.empty()) {
            result.push_back(std::move(token));
        }
    }
    return result;
}
std::string escaped(std::string_view value) {
    return value.empty() ? "\"\"" : fcitx::stringutils::escapeForValue(value);
}
Count parseCount(std::string_view value) {
    Count count = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), count);
    if (error != std::errc{} || end != value.data() + value.size() || !count) {
        throw std::invalid_argument("Invalid history count.");
    }
    return count;
}
} // namespace

class HistoryBigramPrivate {
public:
    StringMap<Counts> unigram_;
    StringMap<StringMap<BigramCounts>> bigram_;
    Count total_ = 0;
    // Only this editing context is needed to retain the author's append/EOS
    // semantics. There is no sentence history pool, capacity, or age weight.
    std::vector<WordWithCode> latest_;
    float unknown_ =
        std::log10(DEFAULT_LANGUAGE_MODEL_UNKNOWN_PROBABILITY_PENALTY);
    bool useOnlyUnigram_ = false;

    Count unigramFreq(WordWithCodeView word) const {
        const auto iter = unigram_.find(word.first);
        return iter == unigram_.end() ? 0 : iter->second.frequency(word.second);
    }
    Count bigramFreq(WordWithCodeView prev, WordWithCodeView cur) const {
        const auto before = bigram_.find(prev.first);
        if (before == bigram_.end()) {
            return 0;
        }
        const auto after = before->second.find(cur.first);
        return after == before->second.end()
                   ? 0
                   : after->second.frequency(prev.second, cur.second);
    }
    void addUnigram(WordWithCodeView word, Count n) {
        increase(total_, n);
        unigram_[std::string(word.first)].add(word.second, n);
    }
    void addBigram(WordWithCodeView prev, WordWithCodeView cur, Count n) {
        bigram_[std::string(prev.first)][std::string(cur.first)].add(
            prev.second, cur.second, n);
    }
    void subtractBigram(WordWithCodeView prev, WordWithCodeView cur, Count n) {
        auto before = bigram_.find(prev.first);
        if (before == bigram_.end()) {
            return;
        }
        auto after = before->second.find(cur.first);
        if (after == before->second.end()) {
            return;
        }
        after->second.subtract(prev.second, cur.second, n);
        if (!after->second.total) {
            before->second.erase(after);
        }
        if (before->second.empty()) {
            bigram_.erase(before);
        }
    }
    static bool valid(const std::vector<WordWithCode> &sentence) {
        return !sentence.empty() &&
               std::ranges::none_of(sentence, [](const auto &word) {
                   return word.first.find('\0') != std::string::npos;
               });
    }
    void add(const std::vector<WordWithCode> &sentence, bool append = false) {
        if (!valid(sentence)) {
            return;
        }
        if (append) {
            subtractBigram(latest_.back(), {"</s>", ""}, 1);
        } else {
            latest_.clear();
            addUnigram({"<s>", ""}, 1);
            addUnigram({"</s>", ""}, 1);
        }
        for (const auto &word : sentence) {
            addUnigram(word, 1);
            addBigram(latest_.empty() ? WordWithCodeView{"<s>", ""}
                                      : WordWithCodeView{latest_.back()},
                      word, 1);
            latest_.push_back(word);
        }
        addBigram(latest_.back(), {"</s>", ""}, 1);
    }
    bool canAppend(const std::vector<WordWithCode> &context) const {
        return !context.empty() && latest_.size() >= context.size() &&
               std::ranges::equal(
                   context,
                   latest_ | std::views::drop(latest_.size() - context.size()));
    }
    void forget(std::string_view word, std::string_view code) {
        auto unigram = unigram_.find(word);
        if (unigram != unigram_.end()) {
            const auto before = unigram->second.total;
            unigram->second.halve(code);
            total_ -= before - unigram->second.total;
            if (!unigram->second.total) {
                unigram_.erase(unigram);
            }
        }
        // Only incoming edges (p, word). Never remove a sentence or touch
        // (word, next), dictionary entries, or explicit pinning.
        for (auto prev = bigram_.begin(); prev != bigram_.end();) {
            auto cur = prev->second.find(word);
            if (cur != prev->second.end()) {
                cur->second.halve(code);
                if (!cur->second.total) {
                    prev->second.erase(cur);
                }
            }
            if (prev->second.empty()) {
                prev = bigram_.erase(prev);
            } else {
                ++prev;
            }
        }
    }
    template <typename F>
    void forUnigrams(F func) const {
        for (const auto &[word, row] : unigram_) {
            for (const auto &[code, n] : row.codes) {
                func(WordWithCodeView{word, code}, n);
            }
        }
    }
    template <typename F>
    void forBigrams(F func) const {
        for (const auto &[prev, following] : bigram_) {
            for (const auto &[cur, pair] : following) {
                for (const auto &[prevCode, row] : pair.rows) {
                    for (const auto &[curCode, n] : row.codes) {
                        func(WordWithCodeView{prev, prevCode},
                             WordWithCodeView{cur, curCode}, n);
                    }
                }
            }
        }
    }
    void loadLegacyPool(std::istream &in) {
        uint32_t count = 0;
        throw_if_io_fail(unmarshall(in, count));
        while (count--) {
            uint32_t size = 0;
            throw_if_io_fail(unmarshall(in, size));
            std::vector<WordWithCode> sentence;
            while (size--) {
                std::string word;
                throw_if_io_fail(unmarshallString(in, word));
                const auto separator = word.find(wordCodeSeparator);
                sentence.emplace_back(word.substr(0, separator),
                                      separator == std::string::npos
                                          ? ""
                                          : word.substr(separator + 1));
            }
            add(sentence);
        }
    }
    void loadLegacy(std::istream &in, uint32_t poolCount) {
        loadLegacyPool(in);
        // The first legacy pool has the latest sentence. Older pools must
        // contribute counts without replacing that editing context.
        auto latest = std::move(latest_);
        for (uint32_t i = 1; i < poolCount; ++i) {
            loadLegacyPool(in);
        }
        latest_ = std::move(latest);
    }
    void loadCounts(std::istream &in) {
        auto count = readCount(in);
        while (count--) {
            const auto word = readWord(in);
            const auto n = readCount(in);
            if (!n) {
                throw std::invalid_argument("Zero history count.");
            }
            addUnigram(word, n);
        }
        count = readCount(in);
        while (count--) {
            const auto prev = readWord(in);
            const auto cur = readWord(in);
            const auto n = readCount(in);
            if (!n) {
                throw std::invalid_argument("Zero history count.");
            }
            addBigram(prev, cur, n);
        }
        count = readCount(in);
        while (count--) {
            latest_.push_back(readWord(in));
        }
    }
};

HistoryBigram::HistoryBigram()
    : d_ptr(std::make_unique<HistoryBigramPrivate>()) {}
FCITX_DEFINE_DEFAULT_DTOR_AND_MOVE(HistoryBigram)

void HistoryBigram::setUnknownPenalty(float unknown) {
    FCITX_D();
    d->unknown_ = unknown;
}
float HistoryBigram::unknownPenalty() const {
    FCITX_D();
    return d->unknown_;
}
void HistoryBigram::setUseOnlyUnigram(bool value) {
    FCITX_D();
    d->useOnlyUnigram_ = value;
}
bool HistoryBigram::useOnlyUnigram() const {
    FCITX_D();
    return d->useOnlyUnigram_;
}
void HistoryBigram::add(const SentenceResult &sentence) {
    addWithCode(sentence, nullptr);
}
void HistoryBigram::addWithCode(const SentenceResult &sentence,
                                const ValidationCodeExtractor &extractor) {
    std::vector<WordWithCode> words;
    for (const auto *word : sentence.sentence()) {
        words.emplace_back(word->word(), extractor ? extractor(word) : "");
    }
    addWithCode(words);
}
void HistoryBigram::add(const std::vector<std::string> &sentence) {
    std::vector<WordWithCode> words;
    for (const auto &word : sentence) {
        words.emplace_back(word, "");
    }
    addWithCode(words);
}
void HistoryBigram::addWithCode(const std::vector<WordWithCode> &sentence) {
    FCITX_D();
    d->add(sentence);
}
void HistoryBigram::addWithContext(const std::vector<WordWithCode> &context,
                                   std::vector<WordWithCode> sentence) {
    FCITX_D();
    d->add(sentence, d->canAppend(context));
}
bool HistoryBigram::isUnknown(std::string_view word) const {
    return rawUnigramFrequency({word, ""}) == 0;
}
float HistoryBigram::score(std::string_view prev, std::string_view cur) const {
    return scoreWithCode({prev, ""}, {cur, ""});
}
float HistoryBigram::scoreWithCode(WordWithCodeView prev,
                                   WordWithCodeView cur) const {
    FCITX_D();
    if (prev.first.empty()) {
        prev.first = "<s>";
    }
    if (cur.first.empty()) {
        cur.first = "<unk>";
    }
    const float bigramWeight = d->useOnlyUnigram_ ? 0.0F : 0.8F;
    const float unigramWeight = 1.0F - bigramWeight;
    // The author's 0.5-count denominator smoothing and 20/80 interpolation,
    // now in absolute-count units. total_ includes the original BOS/EOS
    // unigram observations. No pool capacity or age weight remains.
    constexpr double half = 0.5;
    double pr = unigramWeight * static_cast<double>(d->unigramFreq(cur)) /
                (static_cast<double>(d->total_) + half);
    if (!d->useOnlyUnigram_) {
        pr += bigramWeight * static_cast<double>(d->bigramFreq(prev, cur)) /
              (static_cast<double>(d->unigramFreq(prev)) + half);
    }
    pr = std::min(pr, 1.0);
    return pr == 0 ? d->unknown_ : static_cast<float>(std::log10(pr));
}
float HistoryBigram::score(const WordNode *prev, const WordNode *cur) const {
    return scoreWithCode(prev, cur, nullptr);
}
float HistoryBigram::scoreWithCode(
    const WordNode *prev, const WordNode *cur,
    const ValidationCodeExtractor &extractor) const {
    return scoreWithCode(
        {prev ? prev->word() : "", extractor && prev ? extractor(prev) : ""},
        {cur ? cur->word() : "", extractor && cur ? extractor(cur) : ""});
}
void HistoryBigram::load(std::istream &in) {
    auto loaded = std::make_unique<HistoryBigramPrivate>();
    loaded->unknown_ = d_ptr->unknown_;
    loaded->useOnlyUnigram_ = d_ptr->useOnlyUnigram_;
    uint32_t magic = 0, version = 0;
    throw_if_io_fail(unmarshall(in, magic));
    throw_if_io_fail(unmarshall(in, version));
    if (magic != historyBinaryFormatMagic) {
        throw std::invalid_argument("Invalid history magic.");
    }
    switch (version) {
    case 1:
    case 2:
        loaded->loadLegacy(in, version == 1 ? 2 : 3);
        break;
    case 3:
    case 4:
    case historyBinaryFormatVersion:
        readZSTDCompressed(in, [&](std::istream &compressed) {
            if (version == historyBinaryFormatVersion) {
                loaded->loadCounts(compressed);
            } else {
                loaded->loadLegacy(compressed, 3);
            }
        });
        break;
    default:
        throw std::invalid_argument("Invalid history version.");
    }
    d_ptr = std::move(loaded);
}
void HistoryBigram::save(std::ostream &out) {
    FCITX_D();
    throw_if_io_fail(marshall(out, historyBinaryFormatMagic));
    throw_if_io_fail(marshall(out, historyBinaryFormatVersion));
    writeZSTDCompressed(out, [d](std::ostream &compressed) {
        Count unigrams = 0, bigrams = 0;
        d->forUnigrams([&](auto, Count) { ++unigrams; });
        d->forBigrams([&](auto, auto, Count) { ++bigrams; });
        writeCount(compressed, unigrams);
        d->forUnigrams([&](auto word, Count n) {
            writeWord(compressed, word);
            writeCount(compressed, n);
        });
        writeCount(compressed, bigrams);
        d->forBigrams([&](auto prev, auto cur, Count n) {
            writeWord(compressed, prev);
            writeWord(compressed, cur);
            writeCount(compressed, n);
        });
        writeCount(compressed, d->latest_.size());
        for (const auto &word : d->latest_) {
            writeWord(compressed, word);
        }
    });
}
void HistoryBigram::dump(std::ostream &out) {
    FCITX_D();
    out << textHeader << '\n';
    d->forUnigrams([&](auto word, Count n) {
        out << "U " << n << ' ' << escaped(word.first) << ' '
            << escaped(word.second) << '\n';
    });
    d->forBigrams([&](auto prev, auto cur, Count n) {
        out << "B " << n << ' ' << escaped(prev.first) << ' '
            << escaped(prev.second) << ' ' << escaped(cur.first) << ' '
            << escaped(cur.second) << '\n';
    });
    for (const auto &word : d->latest_) {
        out << "L " << escaped(word.first) << ' ' << escaped(word.second)
            << '\n';
    }
}
void HistoryBigram::loadText(std::istream &in) {
    auto loaded = std::make_unique<HistoryBigramPrivate>();
    loaded->unknown_ = d_ptr->unknown_;
    loaded->useOnlyUnigram_ = d_ptr->useOnlyUnigram_;
    std::string line;
    const bool cumulative = std::getline(in, line) && line == textHeader;
    if (cumulative) {
        while (std::getline(in, line)) {
            const auto fields = tokens(line);
            if (fields.empty()) {
                continue;
            }
            if (fields[0] == "U" && fields.size() == 4) {
                loaded->addUnigram({fields[2], fields[3]},
                                   parseCount(fields[1]));
            } else if (fields[0] == "B" && fields.size() == 6) {
                loaded->addBigram({fields[2], fields[3]},
                                  {fields[4], fields[5]},
                                  parseCount(fields[1]));
            } else if (fields[0] == "L" && fields.size() == 3) {
                loaded->latest_.emplace_back(fields[1], fields[2]);
            } else {
                throw std::invalid_argument("Invalid history count record.");
            }
        }
    } else {
        // Legacy text lists newest sentences first, with optional tab-separated
        // codes. Import every retained observation once, without pool limits.
        std::vector<std::vector<WordWithCode>> sentences;
        do {
            const auto fields = tokens(line);
            std::vector<WordWithCode> sentence;
            const auto firstToken = fcitx::stringutils::trimView(line);
            std::string first;
            auto rest = firstToken;
            fcitx::stringutils::consumeMaybeEscapedValue(rest, FCITX_WHITESPACE,
                                                         &first);
            const bool coded = !rest.empty() && rest.front() == '\t';
            if (coded) {
                if (fields.size() % 2 != 0) {
                    continue;
                }
                for (size_t i = 0; i < fields.size(); i += 2) {
                    sentence.emplace_back(fields[i], fields[i + 1]);
                }
            } else {
                for (const auto &word : fields) {
                    const auto parts = fcitx::stringutils::split(
                        word, "\t",
                        fcitx::stringutils::SplitBehavior::KeepEmpty);
                    if (parts.size() == 2) {
                        sentence.emplace_back(parts[0], parts[1]);
                    } else {
                        sentence.emplace_back(word, "");
                    }
                }
            }
            if (!sentence.empty()) {
                sentences.push_back(std::move(sentence));
            }
        } while (std::getline(in, line));
        for (const auto &sentence : sentences | std::views::reverse) {
            loaded->add(sentence);
        }
    }
    d_ptr = std::move(loaded);
}
void HistoryBigram::clear() {
    auto cleared = std::make_unique<HistoryBigramPrivate>();
    cleared->unknown_ = d_ptr->unknown_;
    cleared->useOnlyUnigram_ = d_ptr->useOnlyUnigram_;
    d_ptr = std::move(cleared);
}
void HistoryBigram::forget(std::string_view word) { forget(word, ""); }
void HistoryBigram::forget(std::string_view word, std::string_view code) {
    FCITX_D();
    d->forget(word, code);
}
void HistoryBigram::remove(std::string_view word, std::string_view code) {
    FCITX_D();
    auto unigram = d->unigram_.find(word);
    if (unigram != d->unigram_.end()) {
        for (auto iter = unigram->second.codes.begin();
             iter != unigram->second.codes.end();) {
            if (!code.empty() && iter->first != code) {
                ++iter;
                continue;
            }
            d->total_ -= iter->second;
            unigram->second.total -= iter->second;
            iter = unigram->second.codes.erase(iter);
        }
        if (!unigram->second.total) {
            d->unigram_.erase(unigram);
        }
    }
    // Remove only edges involving this identity; other words' counts survive.
    for (auto before = d->bigram_.begin(); before != d->bigram_.end();) {
        for (auto after = before->second.begin(); after != before->second.end();) {
            auto &pair = after->second;
            std::vector<std::pair<WordWithCode, Count>> changes;
            for (const auto &[prevCode, row] : pair.rows) {
                for (const auto &[curCode, n] : row.codes) {
                    if ((before->first == word &&
                         (code.empty() || prevCode == code)) ||
                        (after->first == word &&
                         (code.empty() || curCode == code))) {
                        changes.push_back({{prevCode, curCode}, n});
                    }
                }
            }
            for (const auto &[codes, n] : changes) {
                pair.subtract(codes.first, codes.second, n);
            }
            if (!pair.total) {
                after = before->second.erase(after);
            } else {
                ++after;
            }
        }
        if (before->second.empty()) {
            before = d->bigram_.erase(before);
        } else {
            ++before;
        }
    }
    if (std::ranges::any_of(d->latest_, [word, code](const auto &entry) {
            return entry.first == word && (code.empty() || entry.second == code);
        })) {
        d->latest_.clear();
    }
}
void HistoryBigram::fillPredict(std::unordered_set<std::string> &words,
                                const std::vector<std::string> &sentence,
                                size_t maxSize) const {
    FCITX_D();
    if (maxSize && words.size() >= maxSize) {
        return;
    }
    const auto before =
        d->bigram_.find(sentence.empty() ? "<s>" : sentence.back());
    if (before == d->bigram_.end()) {
        return;
    }
    for (const auto &[word, pair] : before->second) {
        if (word == "<s>" || word == "</s>" || !pair.total) {
            continue;
        }
        words.insert(word);
        if (maxSize && words.size() >= maxSize) {
            break;
        }
    }
}
bool HistoryBigram::containsBigram(std::string_view prev,
                                   std::string_view cur) const {
    return rawBigramFrequency({prev, ""}, {cur, ""}) > 0;
}
float HistoryBigram::unigramFrequency(WordWithCodeView word) const {
    return static_cast<float>(rawUnigramFrequency(word));
}
float HistoryBigram::bigramFrequency(WordWithCodeView prev,
                                     WordWithCodeView cur) const {
    return static_cast<float>(rawBigramFrequency(prev, cur));
}
HistoryBigram::Count
HistoryBigram::rawUnigramFrequency(WordWithCodeView word) const {
    FCITX_D();
    return d->unigramFreq(word);
}
HistoryBigram::Count
HistoryBigram::rawBigramFrequency(WordWithCodeView prev,
                                  WordWithCodeView cur) const {
    FCITX_D();
    return d->bigramFreq(prev, cur);
}
} // namespace libime
