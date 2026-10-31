# libime User-First

**libime User-First** is a fork of libime for Pinyin input, focused on personal language memory that accumulates over the long term. Used with the modified fcitx5-chinese-addons, it provides the following features:

- Accumulates word frequencies and word associations from user input without decay over time or through unrelated input.
- Gives personal usage habits greater weight in candidate scoring, using a probability mixture of 99% user history and 1% static language model by default.
- Supports blocking word entries for specific pronunciations, removing the corresponding user dictionary entries and learning records so they no longer appear among ordinary Pinyin candidates.
- Supports pinning candidates to the top of the list.
- Supports reducing candidate frequencies: press Ctrl+Alt+7 by default to open the selection interface, then choose a candidate to halve the cumulative frequencies of its constituent words and the counts for their associated preceding-word combinations.
- Supports undoing learning after deletion: within the same input focus, when the application provides reliable surrounding text, fully deleting newly committed text reverses the learning changes from that commit while preserving previously accumulated memory.
- Retains the static language model for baseline language scoring and cold-start support.

**libime User-First** 是 libime 的一个 fork，面向拼音输入，强调长期累积的个人语言记忆。配合修改后的 fcitx5-chinese-addons，具备以下特点：

- 长期累积用户输入的词频和词语搭配，不随时间或无关输入衰减。
- 提高个人使用习惯在候选评分中的权重，默认采用 99% 用户历史与 1% 静态模型的概率混合。
- 支持屏蔽指定读音的词条，同时清除对应用户词条和学习记录，使其不再出现在普通拼音候选中。
- 支持候选置顶。
- 支持候选降频：默认按 Ctrl+Alt+7 进入选择界面，将选中候选组成词的累计词频及相关前文搭配计数减半。
- 支持删除后撤销学习：在同一输入焦点内，应用提供可靠的周围文本时，完整删除刚提交的内容可撤回本次学习影响，保留此前累计记忆。
- 保留静态语言模型，提供基础语言评分和冷启动支持。

Documentation:

- [Pinyin dictionary text format](src/libime/pinyin/pinyindictionary-text-format.md)
- [拼音词典文本格式](src/libime/pinyin/pinyindictionary-text-format_zh.md)
- [Shuangpin profile text format](src/libime/pinyin/shuangpin-profile-format.md)
- [双拼方案文本格式](src/libime/pinyin/shuangpin-profile-format_zh.md)
- [Table text format](src/libime/table/table-text-format.md)
- [码表文本格式](src/libime/table/table-text-format_zh.md)
