/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <fcitx-utils/log.h>
#include <fcitx-utils/stringutils.h>
#include "libime/core/historybigram.h"
#include "libime/core/utils_p.h"
#include "libime/core/zstdfilter.h"

namespace {

void testBasic() {
    using namespace libime;
    HistoryBigram history;
    history.setUnknownPenalty(std::log10(1.0F / 8192));
    history.add({"你", "是", "一个", "好人"});
    history.add({"我", "是", "一个", "坏人"});
    history.add({"他"});

    history.dump(std::cout);
    std::cout << history.score("你", "是") << '\n';
    std::cout << history.score("他", "是") << '\n';
    std::cout << history.score("他", "不是") << '\n';
    {
        std::stringstream ss;
        history.save(ss);
        history.clear();
        std::cout << history.score("你", "是") << '\n';
        std::cout << history.score("他", "是") << '\n';
        std::cout << history.score("他", "不是") << '\n';
        history.load(ss);
    }
    std::cout << history.score("你", "是") << '\n';
    std::cout << history.score("他", "是") << '\n';
    std::cout << history.score("他", "不是") << '\n';

    history.dump(std::cout);
    {
        std::stringstream ss;
        try {
            history.load(ss);
        } catch (const std::exception &e) {
            history.clear();
        }
    }
    std::cout << history.score("你", "是") << '\n';
    std::cout << history.score("他", "是") << '\n';
    std::cout << history.score("他", "不是") << '\n';

    std::cout << "--------------------" << '\n';
    history.clear();
    history.add({"泥浩"});
    std::cout << history.score("", "泥浩") << " " << history.score("", "你好")
              << '\n';
    history.add({"你好"});
    std::cout << history.score("", "泥浩") << " " << history.score("", "你好")
              << '\n';
    for (int i = 0; i < 40; i++) {
        history.add({"泥浩"});
        std::cout << history.score("", "泥浩") << " "
                  << history.score("", "你好") << '\n';
    }
    history.add({"你好"});
    std::cout << history.score("", "泥浩") << " " << history.score("", "你好")
              << '\n';
    history.add({"你好"});
    std::cout << history.score("", "泥浩") << " " << history.score("", "你好")
              << '\n';
    history.add({"泥浩"});
    std::cout << history.score("", "泥浩") << " " << history.score("", "你好")
              << '\n';
    history.add({"泥浩"});
    std::cout << history.score("", "泥浩") << " " << history.score("", "你好")
              << '\n';

    history.clear();
    for (int i = 0; i < 1; i++) {
        history.add({"跑", "不", "起来"});
        std::cout << "paobuqilai "
                  << history.score("", "跑") + history.score("跑", "不") +
                         history.score("不", "起来")
                  << " "
                  << history.score("", "跑步") + history.score("跑步", "起来")
                  << '\n';
    }
    for (int i = 0; i < 100; i++) {
        history.add({"跑步", "起来"});
        std::cout << "paobuqilai "
                  << history.score("", "跑") + history.score("跑", "不") +
                         history.score("不", "起来")
                  << " "
                  << history.score("", "跑步") + history.score("跑步", "起来")
                  << '\n';
    }
    FCITX_ASSERT(!history.isUnknown("跑步"));
    history.forget("跑步");
    FCITX_ASSERT(history.rawUnigramFrequency({"跑步", ""}) == 50);
    FCITX_ASSERT(history.rawBigramFrequency({"跑步", ""}, {"起来", ""}) == 100);
    std::cout << "paobuqilai "
              << history.score("", "跑") + history.score("跑", "不") +
                     history.score("不", "起来")
              << " "
              << history.score("", "跑步") + history.score("跑步", "起来")
              << '\n';
}

void testOverflow() {
    using namespace libime;
    HistoryBigram history;
    constexpr auto total = 100000;
    for (auto i : std::views::iota(0, total)) {
        history.add({std::to_string(i)});
    }
    for (auto i : std::views::iota(0, total)) {
        FCITX_ASSERT(history.rawUnigramFrequency({std::to_string(i), ""}) == 1);
    }
}

void testPredict() {
    using namespace libime;
    HistoryBigram history;
    constexpr auto total = 10000;
    for (auto i : std::views::iota(0, total)) {
        history.add({std::to_string(i), std::to_string(i + 1)});
    }

    {
        std::unordered_set<std::string> result;
        history.fillPredict(result, {std::to_string(5)}, 10);
        FCITX_ASSERT(result ==
                     std::unordered_set<std::string>{std::to_string(6)});
    }

    {
        std::unordered_set<std::string> result;
        history.add({std::to_string(5), std::to_string(7)});
        history.add({std::to_string(5), std::to_string(4)});
        history.add({std::to_string(5), std::to_string(3)});
        history.add({std::to_string(5), std::to_string(6)});
        history.fillPredict(result, {std::to_string(5)}, 3);
        FCITX_ASSERT(result.size() == 3) << result;
        result.clear();
        history.fillPredict(result, {std::to_string(5)}, 0);
        FCITX_ASSERT(result ==
                     std::unordered_set<std::string>{
                         std::to_string(6), std::to_string(7),
                         std::to_string(3), std::to_string(4)})
            << result;
    }
}

void testSaveAndLoad() {
    using namespace libime;
    HistoryBigram history;
    history.add({std::to_string(1)});
    history.add({std::to_string(2)});
    history.add({std::to_string(3)});

    std::stringstream dump1;
    std::stringstream dump2;
    std::stringstream ss;
    history.save(ss);

    HistoryBigram history2;
    history2.load(ss);

    history.dump(dump1);
    history2.dump(dump2);

    FCITX_ASSERT(dump1.str() == dump2.str());
}

void testSaveAndLoadText() {
    using namespace libime;
    HistoryBigram history;
    constexpr auto total = 100000;
    for (auto i : std::views::iota(0, total)) {
        history.add({std::to_string(i)});
    }
    std::stringstream dump1;
    std::stringstream dump2;
    std::stringstream ss;
    history.dump(ss);

    HistoryBigram history2;
    history2.loadText(ss);

    history.dump(dump1);
    history2.dump(dump2);

    FCITX_ASSERT(dump1.str() == dump2.str());
}

void testWithCode() {
    using namespace libime;
    HistoryBigram history;
    history.addWithCode({{"你", "code1"},
                         {"是", "code2"},
                         {"一个", "code3"},
                         {"好人", "code4"}});

    auto score = history.scoreWithCode({"你", "code1"}, {"是", "code2"});
    auto scoreWithoutCode = history.score("你", "是");
    auto scoreWithEmptyCode = history.scoreWithCode({"你", ""}, {"是", ""});
    auto scoreWithMismatchCode =
        history.scoreWithCode({"你", "code1"}, {"是", "code5"});
    FCITX_ASSERT(score == scoreWithoutCode) << score << " " << scoreWithoutCode;
    FCITX_ASSERT(score == scoreWithEmptyCode)
        << score << " " << scoreWithEmptyCode;
    FCITX_ASSERT(score > scoreWithMismatchCode)
        << score << " " << scoreWithMismatchCode;
    FCITX_ASSERT(history.rawUnigramFrequency({"你", ""}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code1"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code2"}) == 0);
    FCITX_ASSERT(history.rawBigramFrequency({"你", ""}, {"是", ""}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code1"}, {"是", "code2"}) ==
                 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code2"}, {"是", "code2"}) ==
                 0);
    FCITX_ASSERT(history.rawBigramFrequency({"你", ""}, {"是", "code2"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code1"}, {"是", ""}) == 1);
}

void testWithEmptyCode() {
    using namespace libime;
    HistoryBigram history;
    history.addWithCode({{"你", "code1"}, {"是", ""}});

    FCITX_ASSERT(history.rawUnigramFrequency({"你", ""}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code1"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code2"}) == 0);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", ""}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", "code1"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", "code2"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", ""}, {"是", ""}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code1"}, {"是", "code2"}) ==
                 0);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code2"}, {"是", "code2"}) ==
                 0);
    FCITX_ASSERT(history.rawBigramFrequency({"你", ""}, {"是", "code2"}) == 0);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code1"}, {"是", ""}) == 1);
}

void testWithEmptyAndNonEmptyCode() {
    using namespace libime;
    HistoryBigram history;
    history.addWithCode({{"你", "code1"}, {"是", ""}});
    history.addWithCode({{"你", "code1"}, {"是", "code2"}});

    FCITX_ASSERT(history.rawUnigramFrequency({"你", ""}) == 2);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code1"}) == 2);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code2"}) == 0);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", ""}) == 2);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", "code1"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", "code2"}) == 2);
    FCITX_ASSERT(history.rawBigramFrequency({"你", ""}, {"是", ""}) == 2);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code1"}, {"是", "code2"}) ==
                 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code2"}, {"是", "code2"}) ==
                 0);
    FCITX_ASSERT(history.rawBigramFrequency({"你", ""}, {"是", "code2"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"你", "code1"}, {"是", ""}) == 2);
}

void testWithCodePredict() {
    using namespace libime;
    HistoryBigram history;
    history.addWithCode({{"你", "code1"},
                         {"是", "code2"},
                         {"一个", "code3"},
                         {"好人", "code4"}});

    {
        std::unordered_set<std::string> result;
        history.fillPredict(result, {"你"}, 10);
        FCITX_ASSERT(result == std::unordered_set<std::string>{"是"});
    }

    {
        std::unordered_set<std::string> result;
        history.addWithCode({{"你", "code1"}, {"是", "code5"}});
        history.addWithCode({{"你", "code1"}, {"是", "code6"}});
        history.addWithCode({{"你", "code1"}, {"是", "code7"}});
        history.addWithCode({{"你", "code1"}, {"是", "code8"}});
        history.fillPredict(result, {"你"}, 0);
        FCITX_ASSERT(result == std::unordered_set<std::string>{"是"}) << result;
    }
}

void testAppend() {
    using namespace libime;
    HistoryBigram history;
    history.addWithCode({{"你", "code1"}, {"是", "code2"}, {"一个", "code3"}});

    history.addWithContext({{"是", "code2"}, {"一个", "code3"}},
                           {{"好人", "code4"}});

    history.addWithContext({{"不是", "code5"}}, {{"你的", "code6"}});
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code1"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"是", "code2"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"一个", "code3"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"好人", "code4"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"你的", "code6"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"不是", "code5"}) == 0);
    FCITX_ASSERT(history.rawUnigramFrequency({"<s>", ""}) == 2);
    FCITX_ASSERT(history.rawBigramFrequency({"一个", "code3"}, {"</s>", ""}) ==
                 0);
    FCITX_ASSERT(
        history.rawBigramFrequency({"一个", "code3"}, {"好人", "code4"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"好人", "code4"}, {"</s>", ""}) ==
                 1);

    // Counts and the latest append context survive both persistence formats.
    for (bool binary : {false, true}) {
        std::stringstream data;
        if (binary)
            history.save(data);
        else
            history.dump(data);
        HistoryBigram loaded;
        if (binary)
            loaded.load(data);
        else
            loaded.loadText(data);
        loaded.addWithContext({{"你的", "code6"}}, {{"朋友", "code7"}});
        FCITX_ASSERT(loaded.rawUnigramFrequency({"你的", "code6"}) == 1);
        FCITX_ASSERT(loaded.rawUnigramFrequency({"朋友", "code7"}) == 1);
        FCITX_ASSERT(loaded.rawUnigramFrequency({"<s>", ""}) == 2);
        FCITX_ASSERT(loaded.rawBigramFrequency({"你的", "code6"},
                                               {"朋友", "code7"}) == 1);
        FCITX_ASSERT(
            loaded.rawBigramFrequency({"你的", "code6"}, {"</s>", ""}) == 0);
    }
}

void testReversibleLearning() {
    using namespace libime;
    HistoryBigram history;
    auto event =
        history.addWithContextReversible({}, {{"甲", "a"}, {"乙", "b"}});
    FCITX_ASSERT(event);
    FCITX_ASSERT(history.rawUnigramFrequency({"甲", "a"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"乙", "b"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"甲", "a"}, {"乙", "b"}) == 1);
    FCITX_ASSERT(history.undo(event));
    FCITX_ASSERT(history.rawUnigramFrequency({"甲", "a"}) == 0);
    FCITX_ASSERT(history.rawUnigramFrequency({"乙", "b"}) == 0);
    FCITX_ASSERT(!history.undo(event));

    history.addWithCode({{"前词", "p"}});
    auto appended =
        history.addWithContextReversible({{"前词", "p"}}, {{"后词", "q"}});
    FCITX_ASSERT(history.rawBigramFrequency({"前词", "p"}, {"</s>", ""}) == 0);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", "p"}, {"后词", "q"}) == 1);
    FCITX_ASSERT(history.undo(appended));
    FCITX_ASSERT(history.rawUnigramFrequency({"前词", "p"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"后词", "q"}) == 0);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", "p"}, {"</s>", ""}) == 1);

    auto invalidated = history.addWithContextReversible({}, {{"失效", "x"}});
    history.forget("失效", "x");
    FCITX_ASSERT(!history.undo(invalidated));

    history.clear();
    auto first = history.addWithContextReversible({}, {{"甲", "a"}});
    auto second =
        history.addWithContextReversible({{"甲", "a"}}, {{"乙", "b"}});
    FCITX_ASSERT(history.undo(second));
    FCITX_ASSERT(history.undo(first));
    FCITX_ASSERT(history.rawUnigramFrequency({"甲", "a"}) == 0);
    FCITX_ASSERT(history.rawUnigramFrequency({"乙", "b"}) == 0);
}

void testAbsoluteCountsAndFormula() {
    using namespace libime;
    HistoryBigram history;
    for (int i = 0; i < 100; ++i)
        history.add({"不对"});
    for (int i = 0; i < 10; ++i)
        history.add({"部队"});
    history.add({"部队"});
    FCITX_ASSERT(history.rawUnigramFrequency({"不对", ""}) == 100);
    FCITX_ASSERT(history.rawUnigramFrequency({"部队", ""}) == 11);
    FCITX_ASSERT(history.unigramFrequency({"不对", ""}) == 100);
    FCITX_ASSERT(history.bigramFrequency({"<s>", ""}, {"不对", ""}) == 100);
    // Keep original boundary observations and 0.5-count denominator smoothing.
    const double expected = 0.2 * 100 / (333 + 0.5) + 0.8 * 100 / (111 + 0.5);
    FCITX_ASSERT(
        std::abs(std::pow(10.0, history.score("", "不对")) - expected) < 1e-6);
    history.setUseOnlyUnigram(true);
    FCITX_ASSERT(std::abs(std::pow(10.0, history.score("", "不对")) -
                          100 / 333.5) < 1e-6);

    // Exceed all three old pools combined; old evidence is neither moved,
    // aged, nor discarded, and bigrams accumulate indefinitely too.
    for (int i = 0; i < 100000; ++i)
        history.add({"永久", "累计"});
    FCITX_ASSERT(history.rawUnigramFrequency({"不对", ""}) == 100);
    FCITX_ASSERT(history.rawUnigramFrequency({"永久", ""}) == 100000);
    FCITX_ASSERT(history.rawBigramFrequency({"永久", ""}, {"累计", ""}) ==
                 100000);
    FCITX_ASSERT(history.rawBigramFrequency({"<s>", ""}, {"不对", ""}) == 100);
    for (bool binary : {false, true}) {
        std::stringstream data;
        if (binary)
            history.save(data);
        else
            history.dump(data);
        HistoryBigram loaded;
        if (binary)
            loaded.load(data);
        else
            loaded.loadText(data);
        FCITX_ASSERT(loaded.rawUnigramFrequency({"不对", ""}) == 100);
        FCITX_ASSERT(loaded.rawBigramFrequency({"永久", ""}, {"累计", ""}) ==
                     100000);
    }
}

void testHalveAndClear() {
    using namespace libime;
    HistoryBigram history;
    for (int i = 0; i < 37; ++i)
        history.add({"前词", "目标", "后词"});
    for (int i = 0; i < 3; ++i)
        history.add({"另一前词", "目标"});
    history.add({"单次前词", "目标"});
    history.forget("目标");
    FCITX_ASSERT(history.rawUnigramFrequency({"目标", ""}) == 20);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", ""}, {"目标", ""}) == 18);
    FCITX_ASSERT(history.rawBigramFrequency({"另一前词", ""}, {"目标", ""}) ==
                 1);
    FCITX_ASSERT(history.rawBigramFrequency({"单次前词", ""}, {"目标", ""}) ==
                 0);
    FCITX_ASSERT(history.rawBigramFrequency({"目标", ""}, {"后词", ""}) == 37);
    FCITX_ASSERT(history.rawBigramFrequency({"目标", ""}, {"</s>", ""}) == 4);
    FCITX_ASSERT(history.rawUnigramFrequency({"前词", ""}) == 37);
    FCITX_ASSERT(history.rawUnigramFrequency({"后词", ""}) == 37);
    // Total was 201; halving the target removes 21 unigram observations.
    history.setUseOnlyUnigram(true);
    FCITX_ASSERT(std::abs(std::pow(10.0, history.score("", "目标")) -
                          20 / 180.5) < 1e-6);
    for (bool binary : {false, true}) {
        std::stringstream data;
        if (binary)
            history.save(data);
        else
            history.dump(data);
        HistoryBigram loaded;
        if (binary)
            loaded.load(data);
        else
            loaded.loadText(data);
        FCITX_ASSERT(loaded.rawUnigramFrequency({"目标", ""}) == 20);
        FCITX_ASSERT(loaded.rawBigramFrequency({"前词", ""}, {"目标", ""}) ==
                     18);
        FCITX_ASSERT(loaded.rawBigramFrequency({"目标", ""}, {"后词", ""}) ==
                     37);
        loaded.setUseOnlyUnigram(true);
        FCITX_ASSERT(std::abs(std::pow(10.0, loaded.score("", "目标")) -
                              20 / 180.5) < 1e-6);
    }
    history.clear();
    FCITX_ASSERT(history.isUnknown("目标") && history.isUnknown("前词"));
    FCITX_ASSERT(!history.containsBigram("目标", "后词"));
    FCITX_ASSERT(history.score("", "目标") == history.unknownPenalty());
    std::unordered_set<std::string> predictions;
    history.fillPredict(predictions, {"目标"}, 0);
    FCITX_ASSERT(predictions.empty());
    history.addWithContext({{"单次前词", ""}, {"目标", ""}}, {{"新词", ""}});
    FCITX_ASSERT(history.rawBigramFrequency({"目标", ""}, {"新词", ""}) == 0);
    FCITX_ASSERT(history.rawBigramFrequency({"<s>", ""}, {"新词", ""}) == 1);
}

void testHalveCodes() {
    using namespace libime;
    HistoryBigram history;
    for (int i = 0; i < 3; ++i) {
        history.addWithCode({{"前词", "p"}, {"目标", "a"}, {"后词", "n"}});
    }
    history.addWithCode({{"前词", "p"}, {"目标", "b"}, {"后词", "n"}});
    history.forget("目标", "a");
    FCITX_ASSERT(history.rawUnigramFrequency({"目标", "a"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"目标", "b"}) == 1);
    FCITX_ASSERT(history.rawUnigramFrequency({"目标", ""}) == 2);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", "p"}, {"目标", "a"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", "p"}, {"目标", "b"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", ""}, {"目标", "a"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"前词", "p"}, {"目标", ""}) == 2);
    FCITX_ASSERT(history.rawBigramFrequency({"目标", "a"}, {"后词", "n"}) == 3);
    history.forget("目标");
    FCITX_ASSERT(history.isUnknown("目标"));
    FCITX_ASSERT(!history.containsBigram("前词", "目标"));
    FCITX_ASSERT(history.rawBigramFrequency({"目标", ""}, {"后词", ""}) == 4);
}

void testRemoveIdentity() {
    using namespace libime;
    HistoryBigram history;
    history.addWithCode({{"前", "p"}, {"甲", "a"}, {"后", "h"}});
    history.addWithCode({{"前", "p"}, {"乙", "e"}, {"后", "h"}});
    history.remove("甲", "a");
    FCITX_ASSERT(history.isUnknown("甲"));
    FCITX_ASSERT(!history.containsBigram("前", "甲"));
    FCITX_ASSERT(!history.containsBigram("甲", "后"));
    FCITX_ASSERT(history.rawUnigramFrequency({"乙", "e"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"前", "p"}, {"乙", "e"}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"乙", "e"}, {"后", "h"}) == 1);
}

void testSelfBigram() {
    using namespace libime;
    HistoryBigram history;
    for (int i = 0; i < 3; ++i)
        history.add({"重复", "重复"});
    history.forget("重复");
    FCITX_ASSERT(history.rawUnigramFrequency({"重复", ""}) == 3);
    // A self-edge is incoming to the target, and is halved exactly once.
    FCITX_ASSERT(history.rawBigramFrequency({"重复", ""}, {"重复", ""}) == 1);
    FCITX_ASSERT(history.rawBigramFrequency({"重复", ""}, {"</s>", ""}) == 3);
}

void testLargeCountsAndEscaping() {
    using namespace libime;
    HistoryBigram history;
    std::stringstream text;
    text << "# libime-history-counts-v1\n"
         << "U 4294967297 长期 \"\"\n"
         << "B 4294967297 前词 \"\" 长期 \"\"\n";
    history.loadText(text);
    FCITX_ASSERT(history.rawUnigramFrequency({"长期", ""}) ==
                 UINT64_C(4294967297));
    history.forget("长期");
    history.add({"长期"});
    FCITX_ASSERT(history.rawUnigramFrequency({"长期", ""}) ==
                 UINT64_C(2147483649));
    FCITX_ASSERT(history.rawBigramFrequency({"前词", ""}, {"长期", ""}) ==
                 UINT64_C(2147483648));
    history.addWithCode(
        {{"space tab\t newline\n quote\" slash\\", "code\t\n\"\\"}});
    for (bool binary : {false, true}) {
        std::stringstream data, before, after;
        history.dump(before);
        if (binary)
            history.save(data);
        else
            history.dump(data);
        HistoryBigram loaded;
        if (binary)
            loaded.load(data);
        else
            loaded.loadText(data);
        loaded.dump(after);
        FCITX_ASSERT(before.str() == after.str())
            << before.str() << after.str();
        FCITX_ASSERT(loaded.rawUnigramFrequency({"长期", ""}) ==
                     UINT64_C(2147483649));
    }
    const auto before = history.rawUnigramFrequency({"长期", ""});
    std::stringstream invalid("# libime-history-counts-v1\nU -1 bad \"\"\n");
    bool threw = false;
    try {
        history.loadText(invalid);
    } catch (const std::exception &) {
        threw = true;
    }
    FCITX_ASSERT(threw && history.rawUnigramFrequency({"长期", ""}) == before);
}

void testLegacyFormats() {
    using namespace libime;
    for (uint32_t version : {1U, 2U, 3U, 4U}) {
        std::stringstream binary;
        marshall(binary, uint32_t{0x000fc315});
        marshall(binary, version);
        auto write = [version](std::ostream &out) {
            const auto pools = version == 1 ? 2 : 3;
            for (int pool = 0; pool < pools; ++pool) {
                marshall(out, uint32_t{1}); // one sentence in every pool
                marshall(out, uint32_t{1}); // one word
                std::string word = pool == 0 ? "部队" : "不对";
                if (version == 4)
                    word += std::string(1, '\x02') + "bu'dui";
                marshallString(out, word);
            }
        };
        if (version >= 3)
            writeZSTDCompressed(binary, write);
        else
            write(binary);
        HistoryBigram history;
        history.load(binary);
        FCITX_ASSERT(history.rawUnigramFrequency({"部队", ""}) == 1);
        FCITX_ASSERT(history.rawUnigramFrequency({"不对", ""}) ==
                     (version == 1 ? 1 : 2));
        FCITX_ASSERT(history.rawBigramFrequency({"<s>", ""}, {"不对", ""}) ==
                     (version == 1 ? 1 : 2));
        history.addWithContext({{"部队", version == 4 ? "bu'dui" : ""}},
                               {{"后词", ""}});
        FCITX_ASSERT(history.rawBigramFrequency({"部队", ""}, {"后词", ""}) ==
                     1);
        FCITX_ASSERT(history.rawBigramFrequency({"部队", ""}, {"</s>", ""}) ==
                     0);
    }
    std::stringstream text(
        "你的\tcode6\n你\tcode1 是\tcode2 一个\tcode3 好人\tcode4\n");
    HistoryBigram history;
    history.loadText(text);
    FCITX_ASSERT(history.rawUnigramFrequency({"你", "code1"}) == 1);
    FCITX_ASSERT(
        history.rawBigramFrequency({"一个", "code3"}, {"好人", "code4"}) == 1);
    history.addWithContext({{"你的", "code6"}}, {{"后词", ""}});
    FCITX_ASSERT(history.rawBigramFrequency({"你的", "code6"}, {"后词", ""}) ==
                 1);
}

} // namespace

int main() {
    testBasic();
    testOverflow();
    testPredict();
    testSaveAndLoad();
    testSaveAndLoadText();
    testWithCode();
    testWithEmptyCode();
    testWithEmptyAndNonEmptyCode();
    testWithCodePredict();
    testAppend();
    testReversibleLearning();
    testAbsoluteCountsAndFormula();
    testHalveAndClear();
    testHalveCodes();
    testRemoveIdentity();
    testSelfBigram();
    testLargeCountsAndEscaping();
    testLegacyFormats();
    return 0;
}
