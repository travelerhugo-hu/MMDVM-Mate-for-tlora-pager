# MMDVM Mate — GitHub 社区发布提案

> 本文档用于正式上传前与用户确认。它不包含芯片、开发板、屏幕、音频方案、接口、框架等技术规格，仅面向社区玩家描述「这是什么」「怎么玩」「哪里有趣」。

---

## 一、建议仓库名

`MMDVM-Mate`

---

## 二、英文标题与简介

**Title:** MMDVM Mate — A Pocket-Sized Live Talkgroup Listener

**Tagline:** Tune into global amateur radio talkgroups wherever you have Wi-Fi.

**What it is**

MMDVM Mate is a small, battery-powered companion that brings live digital-voice talkgroup traffic to your pocket. Connect to Wi-Fi, pick a talkgroup, and listen to operators around the world — while the screen shows you who is talking, a rolling QSO history of recent contacts, and a live activity meter.

It is a **network listener**, not a transceiver: it only receives live audio from the talkgroup feed and never transmits.

**How to play**

1. On first boot, open **Settings** and enter your Wi-Fi name and password.
2. Choose the talkgroup you want to monitor. The orange key switches the keyboard to number mode, so you can type the talkgroup number directly.
3. The device reconnects automatically, subscribes to that talkgroup, and starts playing live audio.
4. Use the side keys for volume and backlight, and scroll the recent-contacts list with the rotary dial.

All settings are saved, so the next time you power on it just reconnects and keeps listening.

**What's fun**

- **A window into a global community.** You might hear a repeater across town, or an operator on another continent.
- **Callsigns come alive.** Cryptic DMR IDs are resolved into real names and callsigns (via Talker Alias and radioid.net lookup).
- **Your personal "heard log."** The QSO history keeps the last contacts you monitored, so you can look back at who was on the air.
- **Truly standalone.** No phone, no PC, no companion app required — just Wi-Fi and curiosity.
- **Worry-free listening.** Because it only listens, you can monitor without the risk of accidentally keying up a transmitter.

---

## 三、中文标题与简介

**标题：** MMDVM Mate —— 揣进口袋的实时通话组收听器

**副标题：** 有 Wi-Fi 的地方，就能听见全球业余电台。

**这是什么**

MMDVM Mate 是一台小型、电池供电的随身装置，把数字语音通话组的实时通联装进你的口袋。连接 Wi-Fi、输入一个通话组号码，就能收听世界各地的业余电台通联，同时屏幕会显示当前发话的呼号、最近联络清单，以及实时活跃度表。

它是一台**网络收听器**，不是收发信机：只接收通话组语音流，从不发射。

**怎么玩**

1. 首次开机进入**设定**，输入 Wi-Fi 名称与密码。
2. 选择想收听的通话组。按橙色键把键盘切换到数字模式，直接输入通话组号码。
3. 装置自动重连、订阅该通话组，并开始播放实时语音。
4. 用侧边键调节音量与背光，旋转滚轮翻看最近联络记录。

所有设定都会保存，下次开机即可自动重连、继续收听。

**哪里有趣**

- **通往全球业余电台社区的一扇窗。** 你可能听到隔壁中继台，也可能听到大洋彼岸的通联。
- **呼号变「活人」。** 冷冰冰的 DMR ID 会被解析成真实姓名与呼号。
- **你的私人「收听日志」。** 最近联络清单记录你监听过的电台，方便回头查看。
- **完全独立。** 不需要手机、不需要电脑、不需要配套 App，只要有 Wi-Fi 和好奇心。
- **安心监听。** 因为只接收不发话，不用担心误触发射键。

---

## 四、建议公开 README 顶部结构

如果直接面向 GitHub 社区，建议把 README 顶部改成以下顺序：

1. **Cover image**（`cover-mmdvm-mate.png`）
2. **Title + Tagline**（如上英文版）
3. **What it is**（3–4 句话）
4. **Quick demo / UI mockup**（`ui-mockup-mmdvm-mate.png`）
5. **How to play**（5 步以内）
6. **Why it's fun**（4–5 个 bullet）
7. **One-line disclaimer**（非官方、仅监听、遵守当地法规）

现有的 Hardware / Architecture / Building / Flashing / Configuration 等技术章节可以保留在下方「For builders」或「Developer notes」折叠区，或另存为 `docs/BUILD.md`，避免首页过载。

---

## 五、素材文件

| 文件 | 用途 | 备注 |
| --- | --- | --- |
| `assets/cover-mmdvm-mate.png` | GitHub 项目封面 / social preview | 由固件 UI 代码精确重绘的实机级渲染（深色 GitHub 主题、真实标签与配色），非照片 |
| `assets/ui-mockup-mmdvm-mate.png` | README 里的主界面示意图 | 按 `ui.cpp` 实际坐标/配色 1:1 重绘的主监控屏（960×444，即实机 480×222 的 2×） |

> 说明：以上为**根据固件自身 UI 代码精确重绘**的渲染图（颜色、文字、布局与设备在屏幕上绘制的一致），用于在没有实机照片时的发布素材。
> 若你手上有**真实设备截图 / 照片**，请放入 `assets/`，我可一键替换为 `cover-mmdvm-mate.png` 与 `ui-mockup-mmdvm-mate.png`。
> 源文件：`assets/cover-mmdvm-mate.svg`、`assets/ui-mockup-mmdvm-mate.svg`、`assets/build_assets.mjs`（可随时调整内容后重新生成）。

---

## 六、公开文案禁区（已规避）

以下内容**未**出现在本提案中：

- 芯片 / SoC / MCU（ESP32-S3 等）
- 开发板型号（LilyGo T-LoRa Pager 等）
- 屏幕规格、分辨率、驱动
- 音频编解码器 / 采样率 / I2S / 音频方案
- 硬件接口、GPIO、键盘矩阵、外设名称
- 开发框架、RTOS、UI 框架、构建工具

---

## 七、上传前确认事项

请确认：

1. 中英文标题与简介是否符合社区调性？
2. 封面图与 UI 概念图是否可用，还是需要重新生成/去水印/改用实机截图？
3. 是否希望我基于本提案直接重写一份「社区版 README.md」替换当前首页？
4. 是否保留「Developer / Builder」技术章节在 README 后方，还是拆分到 `docs/`？
5. GitHub 仓库的 License、.gitignore、Topics / Tags 是否已就绪？

---

*Prepared for confirmation before GitHub upload. Do not upload until reviewed.*
