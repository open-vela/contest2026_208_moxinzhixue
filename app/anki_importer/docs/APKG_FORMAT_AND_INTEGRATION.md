# APKG 格式调研与 OpenVela 集成说明

## 1. Anki 包格式结论

APKG/Colpkg 的外层是 ZIP，但内部格式随 Anki 代际变化：

| 代际 | collection 成员 | media 成员 | 压缩方式 | 本模块 |
| --- | --- | --- | --- | --- |
| Anki 2.0 | `collection.anki2`，SQLite | JSON：`{"0":"pic.png"}` | ZIP Stored/Deflate | 支持 |
| Anki 2.1 legacy | `collection.anki21`，SQLite | 同上 | ZIP Stored/Deflate | 支持 |
| 新版包 | `collection.anki21b` | protobuf manifest，另有 protobuf `meta` | 外层成员通常 Stored，collection/media payload 各自 zstd | 明确拒绝 |

新版包可能同时放置一个 dummy `collection.anki2`。检测顺序因此必须先检查 `collection.anki21b`，不能看到 `.anki2` 就按旧包导入。本实现遵守该顺序并返回 `-EPROTONOSUPPORT`。

上游依据：

- [meta.rs](https://github.com/ankitects/anki/blob/main/rslib/src/import_export/package/meta.rs)
- [media.rs](https://github.com/ankitects/anki/blob/main/rslib/src/import_export/package/media.rs)
- [colpkg/export.rs](https://github.com/ankitects/anki/blob/main/rslib/src/import_export/package/colpkg/export.rs)

Legacy SQLite 只读取：

```sql
SELECT models, decks FROM col LIMIT 1;
SELECT cards.id, notes.id, notes.mid, cards.did, notes.tags, notes.flds
FROM cards JOIN notes ON notes.id=cards.nid
WHERE cards.ord=0 ORDER BY cards.id;
```

数据库使用 `SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX` 打开，设置 `query_only`、关闭 mmap、将 page cache 请求限制为 256 KiB，并执行 `PRAGMA quick_check(1)`。SQLite 是板端源格式解析器；不能用 KVDB/UnQLite 代替读取 Anki SQLite。

## 2. OpenVela 现有能力

Gemini-S1 `r528s3-gemini-s1/configs/nsh` 已启用：

- `CONFIG_LIB_ZLIB=y`：其 target 已编入 minizip `unzip.c`，支持 `unz*64`，无需自写 ZIP parser。
- `CONFIG_NETUTILS_CJSON=y`。
- `CONFIG_UNQLITE=y`。
- `CONFIG_KVDB=y`。

SQLite 位于 `external/sqlite`，默认未启用。导入器选择 `CONFIG_LIB_SQLITE`，并用下列条件限制菜单可见性：

```text
CONFIG_SYSTEM_POPEN=y
CONFIG_FS_LARGEFILE=y
CONFIG_FS_LINKS=y
CONFIG_FS_LOCK_BUCKET_SIZE > 0
```

最小新增配置是：

```text
CONFIG_MOXINZHI_ANKI_IMPORTER=y
CONFIG_LIB_SQLITE=y                 # 由 importer select
```

zlib、cJSON、UnQLite 也由 importer select；Gemini-S1 当前基线已经具备它们。KVDB 保留给项目其他配置/服务场景，本模块通过 `card_model` 抽象直接使用已有 UnQLite 库，避免让 UI 依赖源 Anki schema 或 importer 私有实现。

Legacy Make 构建中，SQLite 的 `sqlite_cfg.h` 在主机生成，而 ARM EABI 的 `uint32_t` 底层 C 类型可能与主机不同。本模块的 `Make.defs` 在启用时补充 `UINT32_TYPE=unsigned`、`INT32_TYPE=int`，使 SQLite 私有 `u32/i32` 与公开 `sqlite3.h` ABI 一致。CMake 的 SQLite target 不使用该 host config header，不需要这一补丁。

## 3. 镜像与 RAM 评估

测量环境：仓库 SQLite 3.43.0 amalgamation、OpenVela 自带 `arm-none-eabi-gcc 13.4.0`、Cortex-A7 Thumb、`-Os -ffunction-sections -fdata-sections`、`SQLITE_OMIT_LOAD_EXTENSION`。

| 测量项 | text | data | bss | 合计 |
| --- | ---: | ---: | ---: | ---: |
| SQLite 完整对象 | 386,820 B | 6,488 B | 665 B | 393,973 B（384.7 KiB） |
| 以 importer 使用的 SQLite API 为 GC roots | 374,677 B | 6,464 B | 521 B | 381,662 B（372.7 KiB） |

因此对 Gemini-S1 镜像应按约 **+0.37 MiB flash** 预留。该数字是使用实际 ARM 工具链得到的链接可达估算；最终固件 delta 会因全局 LTO、公共 libc 符号和其他 SQLite 使用者略有变化，应在产品 defconfig 上做最终前后镜像差分。

运行期主要预算：

- SQLite page cache 请求上限：256 KiB（`PRAGMA cache_size=-256`）。
- importer task stack 默认：48 KiB。
- ZIP/文件流 buffer：16 KiB。
- 正面和背面清理 buffer：各 `CONFIG_MOXINZHI_ANKI_MAX_FIELD_BYTES`，默认各 32 KiB。
- models/decks 与 media JSON 按配置上限临时载入；默认上限分别为 1 MiB/2 MiB，但不会载入 collection 或媒体文件整体。

如果镜像预算无法接受，应在 OpenVela 公共 `external/sqlite` 中设计全局可验证的裁剪配置，而不是在 importer 内复制 SQLite；例如关闭其他消费者确实不需要的扩展。不能用“PC 先转 JSON/SQLite”替代板端解析来规避该成本。

## 4. 本地卡片模型

存储 schema 标识为 `moxinzhi-card-model/1`。包 SHA-256 是导入 namespace 和重复检测键。公开模型包含：

- import：状态、源格式、时间、卡片/牌组/媒体/跳过计数、源文件名。
- card：源 card/note/deck/model id、front/back、deck name、tags、媒体引用、本地复习统计。
- media：源数字 ZIP index、原文件名、本地路径、大小。

导入只保留内容和项目自己的复习计数；Anki `revlog`、queue、ease、interval、lapses、原 due 等调度历史不迁移。

## 5. 安全、资源限制和恢复

导入前后均不信任包内容：

- 源必须是配置 input root 的直接 `.apkg` 子文件；符号链接拒绝。
- ZIP entry 名拒绝绝对路径、反斜杠、盘符、空/`.`/`..` segment。
- 拒绝加密和 Stored/Deflate 之外的 ZIP compression method。
- 限制包大小、entry 数、collection 大小、单媒体大小、总解压大小和精确压缩比。
- 每个释放文件使用 `O_EXCL`，验证实际字节数和 ZIP CRC。
- 只释放被已导入卡片引用、并经 legacy media map 映射的数字成员。
- media map 指向不存在成员时产生 `MEDIA_MISSING` warning，不向 UI 暴露无效本地路径。

事务顺序：

1. 计算 SHA-256；若 COMPLETE 已存在，直接报告 duplicate。
2. 独立提交 STAGING marker。
3. 在一个 UnQLite transaction 内写牌组、卡片和媒体记录。
4. 媒体写入 `.staging-<sha256>`，文件和目录 fsync 后原子 rename。
5. 写 COMPLETE record，最后提交 UnQLite transaction。
6. JSON 报告用 `.tmp` + fsync + rename 写入。

若在任一步骤断电，下次遇到同一 SHA-256 的 STAGING 会删除该 namespace、工作目录、媒体 staging/final 目录并重新导入。已 COMPLETE 的媒体目录不会在重复检测前被删除。

## 6. 导入报告

每次调用都会尝试生成 `moxinzhi-anki-import-report/1` JSON。`result` 数值为：0 imported、1 duplicate、2 rejected、3 failed。示例见 [import-report.example.json](import-report.example.json)。报告写失败不回滚已经完成的卡片导入，而是设置 `ANKI_IMPORT_WARN_REPORT_IO`。

## 7. 集成边界

- manifest 将 `app/anki_importer` link 到 `packages/demos/contest2026_208_anki_importer`。
- 不修改 UI 主文件。UI/统计只调用 `card_model.h`。
- host fixture generator 仅构造测试输入；板端命令读取原始 APKG 并完成 ZIP、SQLite、HTML、media 和事务处理。
- 新版 zstd/protobuf 支持应作为后续独立能力加入，不能把 dummy `collection.anki2` 当作可导入 collection。
