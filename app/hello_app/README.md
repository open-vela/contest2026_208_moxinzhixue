# 墨芯智学 P0

该应用映射到 openvela
`packages/demos/contest2026_208_hello_app`，保留原命令名
`lvgl_screen_test`。默认启动的是“墨芯智学”产品界面，原 LCD/触摸测试作为
设置页中的硬件诊断模式保留。

## P0 范围

- 首页：学习入口、知识卡与掌握数量概览；
- AI 问答：三组纯本地模拟问题及答案，不访问网络；
- 知识卡：保存最近问答，支持“掌握/未掌握”和切换卡片；
- 学习统计：提问、保存、掌握、复习次数及当前卡片掌握率；
- 设置：每日目标、KVDB/触摸/语言状态、数据清空、硬件诊断；
- 完整本地闭环：模拟问答 → 保存知识卡 → 掌握/未掌握 → 统计更新；
- KVDB 持久化：使用 `persist.moxinzhi.*` 键，目标配置实际落到
  `/data/persist.db`，不依赖 SQLite。

最多保留 6 张知识卡，写满后循环覆盖最早槽位。每张卡和元数据分别保存，
记录包含版本、长度和校验值，并保持在 KVDB 单值 255 字节限制以内。

## 线程与服务边界

应用只有一个 LVGL UI/事件循环线程。页面创建、事件回调、模型刷新和诊断
界面切换均在该线程中执行。

`moxinzhi_ai_service.h` 定义了可替换的 AI 查询函数：P0 注入本地模拟
provider；后续接入网络或小智协议时替换 provider 即可，UI 与学习模型无需
直接依赖协议实现。本版本刻意不包含网络、小智协议或 SQLite 代码。

主要模块：

| 文件 | 职责 |
| --- | --- |
| `hello_app_main.c` | 板级、LVGL 后端与单线程主循环生命周期 |
| `moxinzhi_ui.c` | 五页产品 UI、导航和交互 |
| `moxinzhi_model.c` | 问答、卡片、掌握状态和统计模型 |
| `moxinzhi_ai_service.c` | AI 服务接口与纯本地模拟 provider |
| `moxinzhi_storage.c` | KVDB 持久化适配 |
| `moxinzhi_diag.c` | 原 LCD、灰阶、刷新和触摸坐标诊断能力 |

## Gemini-S1 适配

应用继续通过 LVGL NuttX LCD 后端打开 `/dev/lcd0`，适配 Gemini-S1 的
2.8 英寸 ILI9341。布局读取实际分辨率，在 240×320 竖屏和 320×240 横屏
下自动调整内容区与底部导航高度；触摸默认使用 `/dev/input0`。

若系统包含 `/resource/fonts/MiSans-Normal.ttf` 且启用 LVGL FreeType，
界面显示中文；字体资源不可用时自动显示英文界面，避免中文占位方框。
字体路径可通过 Kconfig 修改。

## 配置

目标配置需启用：

```text
CONFIG_GRAPHICS_LVGL=y
CONFIG_LV_USE_NUTTX=y
CONFIG_LV_USE_NUTTX_LCD=y
CONFIG_KVDB=y
CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST=y
```

触摸还需启用 `CONFIG_LV_USE_NUTTX_TOUCHSCREEN=y`。相关配置项：

```text
CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_INPUT_DEVPATH="/dev/input0"
CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_FONT_PATH="/resource/fonts/MiSans-Normal.ttf"
```

Gemini-S1 工作区构建命令：

```bash
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh \
  --cmake -j6
```

## 运行

默认产品界面：

```text
lvgl_screen_test
```

直接进入全屏硬件诊断：

```text
lvgl_screen_test --diagnostic
```

运行 30 秒后退出并释放 LCD、触摸及 LVGL 资源：

```text
lvgl_screen_test -t 30
```

同一时刻只能运行一个 LVGL 应用；启动前需退出 `lvgldemo` 等其他 LVGL
程序。触摸设备打开失败时产品界面仍可显示，并在设置页及终端标记触摸不可用。
