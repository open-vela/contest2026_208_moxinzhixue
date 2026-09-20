# 墨芯智学 Anki 本地导入器

本模块在 Gemini-S1 / R528 / OpenVela 板端直接解析 `/data/import/*.apkg`，把可支持的卡片写入统一的 `moxinzhi-card-model/1` 本地模型，并将被卡片引用的媒体流式释放到本地目录。主机脚本仅用于生成测试夹具，不参与产品导入流程，也不要求先在 PC 上预处理牌组。

## 当前兼容范围

- Anki 2.0：ZIP 中的 `collection.anki2` SQLite。
- Anki 2.1 旧包：ZIP 中的 `collection.anki21` SQLite。
- 普通 note type（`type=0`）、恰好两个字段、`cards.ord=0`。
- 前两个 note 字段直接映射为正面/背面；不执行 Anki 模板。
- 基础 HTML 清理、常用实体解码、段落换行、`script/style` 内容移除。
- `<img src="...">` 与 `[sound:...]` 本地媒体引用，按 legacy `media` JSON 的数字成员映射导入。

明确不支持：`collection.anki21b` 新包、protobuf media manifest、zstd 内层数据、cloze、复杂模板/条件模板、JavaScript、CSS 渲染、LaTeX 渲染、反向或额外模板卡（`ord>0`）、Anki 调度和复习历史。LaTeX 标记会作为普通文本保留并产生 warning；JavaScript 不会执行。

格式调研、依赖、资源评估和集成步骤见 [APKG_FORMAT_AND_INTEGRATION.md](docs/APKG_FORMAT_AND_INTEGRATION.md)。

## 板端启用与运行

在 `menuconfig` 启用：

```text
Application Configuration
  -> Moxinzhixue Anki package importer
```

Kconfig 会选择 zlib/minizip、cJSON、UnQLite 和 SQLite。默认路径为：

```text
输入       /data/import/*.apkg
卡片库     /data/moxinzhi/cards.unqlite
媒体       /data/moxinzhi/media/<sha256>/
工作目录   /data/moxinzhi/.import-work/<sha256>/
导入报告   /data/moxinzhi/import-reports/<sha256>.json
```

NSH 命令：

```sh
anki_import /data/import/my-deck.apkg
```

需要覆盖路径时：

```sh
anki_import \
  --input-root /data/import \
  --store /data/moxinzhi/cards.unqlite \
  --media /data/moxinzhi/media \
  --work /data/moxinzhi/.import-work \
  --reports /data/moxinzhi/import-reports \
  /data/import/my-deck.apkg
```

## UI / 统计模块接口

只需包含 `card_model.h`，无需读取 UnQLite 私有 key：

- `card_model_foreach_card()`：遍历所有已提交卡片。
- `card_model_get_card()`：按包指纹与 Anki card id 读取。
- `card_model_update_review()`：更新本地复习次数、正确次数和下次到期时间。
- `card_model_get_media_by_name()`：将字段中的媒体名解析为本地文件。
- `card_model_get_import()`：读取导入状态和统计。

调用 `card_model_get_card()` 后，字符串由 `allocation` 指向的单块内存承载，调用方使用完后执行 `free(allocation)`。

## 主机测试

依赖 Linux/WSL、CMake、Python 3、SQLite3 和 zlib 开发包，以及完整 OpenVela workspace：

```sh
cd app/anki_importer/tests
OPENVELA_WORKSPACE=/path/to/openvela-workspace ./run_host_tests.sh
```

测试在 `/tmp` 构建，动态生成 APKG；不向仓库写入二进制夹具。覆盖 2.0/2.1、HTML、媒体、重复导入、损坏 ZIP/SQLite、路径穿越、加密、未知压缩、压缩炸弹、超限条目、新版包拒绝、源文件符号链接和断电后 STAGING 恢复。
