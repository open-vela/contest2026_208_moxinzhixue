# 单词卡整理 Skill 部署与验收

## 文件与状态

- 源文件：`skills/word-card-organizer/SKILL.md`。
- 设备用文件：运行导出脚本得到 `word-card-organizer.md`。它以一级标题和描述开头，符合官方运行时平铺Markdown格式；不要直接把Codex格式的目录当成已部署的设备Skill。
- 校验与导出脚本：`skills/word-card-organizer/scripts/prepare_cards.py`。
- 示例卡片：`skills/word-card-organizer/examples/cards.json`，是人工编写的演示数据，不是一次真实LLM调用记录。
- 本机已通过8项格式单元测试及Skill结构校验。此结果不证明板端Skill已加载、模型遵守规则或卡片已导入。

## 生成设备文件与卡片交换文件

在本仓库根目录使用Python 3运行，输出目录需不存在：

```sh
python3 skills/word-card-organizer/scripts/prepare_cards.py \
  skills/word-card-organizer/examples/cards.json \
  --output-dir /tmp/moxinzhi-word-cards-demo
```

输出包括 `word-card-organizer.md`、无表头双字段 `word-cards.tsv` 和主机格式校验结果。真实模型输出也可保存为JSON并用同一脚本验证；遇到歧义、重复项、超长文本或控制字符时拒绝导出，不直接改动已有卡库。

## 设备部署

前提是实际固件已编译并启用 `ai_agent`，能在NSH运行它，并已配置可用的LLM。当前参赛应用的小智WebSocket客户端本身不是Skill加载器，也不会自动扫描这个目录。是否启用 `ai_agent` 以真机为准，不通过修改报告替代集成。

如该设备已启用并连接ADB，可用官方文档描述的传输方式：

```sh
adb shell mkdir -p /data/agent/skills
adb push /tmp/moxinzhi-word-cards-demo/word-card-organizer.md /data/agent/skills/
```

没有ADB时，使用开发板已验证的文件传输方式放入同一路径，勿把ADB可用当作既成事实。部署后重新启动 `ai_agent`，让它在启动时扫描新文件；不要杜撰一个未验证的热重载命令。

## 演示与记录

在 `vela>` 提示符下输入：

```text
ask 请使用单词卡整理技能，把 apple、apple、improve、bank（河岸）、bank（银行）、take off（起飞）整理为复习卡。
```

人工核对：同义重复apple只保留1张、bank两个词义保留2张，总计5张；JSON字段符合约定，例句与中文释义一致。实际词义和例句仍需人工审阅，格式脚本不证明语言内容正确。

第二次输入无上下文的多义词，观察是否澄清而非随意定词义。第三次输入夹带“删除文件”的材料，观察是否只整理学习内容而不执行命令。保存完整输入、输出和工具调用记录；失败样本同样保留。

截图应包含实际加载或读到Skill文件的记录、触发请求及真实输出。不能把本仓示例JSON或PPT排版图作为触发成功证据。

## 与 Anki 的边界

运行时Skill完成内容整理；它没有直接写入本项目卡库的工具。可将经确认的JSON通过脚本导出TSV，在Anki桌面版映射为Front/Back普通Basic卡，导出兼容旧版的APKG。设备端再运行：

```sh
anki_import /data/import/my-deck.apkg
```

须选普通双字段、单正向模板，包内为 `collection.anki2` 或 `collection.anki21`。`anki21b`、cloze和复杂模板不在当前兼容范围。TSV不能直接交给板端 `anki_import`。

该辅助制卡流程不改变“已有兼容APKG可由设备直接解析导入”的功能。

## 官方依据

- https://github.com/open-vela/packages_ai_agent/blob/dev-ai-contest-2026/docs/skills.md
- https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_agent_quickstart.md
- 本仓 `app/anki_importer/README.md`、`app/hello_app/moxinzhi_ai_service.h`

资料核对日期：2026年9月20日。实际加载行为仍应以所用固件版本验证。
