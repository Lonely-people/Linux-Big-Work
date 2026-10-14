# Linux 本地 AI 聊天系统

本项目使用 Linux C socket 编写 TCP 聊天客户端和 `fork()` 并发服务器。服务端通过本机 HTTP socket 调用 llama.cpp 的 `llama-server`，使用本地 Qwen GGUF 模型生成流式回复；不调用外部大模型 API。

## 运行环境

- Linux / WSL2 Ubuntu，GCC 和 GNU Make
- 已编译的 `llama.cpp/build/bin/llama-server`
- 模型文件 `models/Qwen3-4B-Q4_K_M.gguf`
- llama-server 默认监听 `127.0.0.1:8080`
- 聊天服务器默认监听 `127.0.0.1:9090`

## 编译

在项目根目录运行：

```bash
make -C project clean all
```

可执行文件生成在 `project/bin/`。

## 启动

先在项目根目录启动本地模型服务：

```bash
./llama.cpp/build/bin/llama-server \
  -m ./models/Qwen3-4B-Q4_K_M.gguf \
  --host 127.0.0.1 \
  --port 8080 \
  --n-gpu-layers 99
```

另开终端启动聊天 TCP 服务：

```bash
./project/bin/server 9090
```

服务端启动时会在控制台打印监听端口和主进程 PID，同时写入活动日志 `project/logs/server.log`（持续追加）。每当有客户端 TCP 连接建立，日志会记录客户端 IP 和端口（`CONNECT`）、fork 出的子进程 PID（`FORK`），以及子进程处理结束的记录（`EXIT`），方便观察连接与进程活动：

```
[2026-10-13 12:00:00] START server listening on 127.0.0.1:9090 (pid=1234, log=.../logs/server.log)
[2026-10-13 12:00:05] CONNECT peer=127.0.0.1:53124
[2026-10-13 12:00:05] FORK child=1235 for peer=127.0.0.1:53124
[2026-10-13 12:00:30] EXIT child=1235 peer=127.0.0.1:53124
```

再开一个或多个终端启动客户端：

```bash
./project/bin/client 127.0.0.1 9090
```

输入 `/quit` 关闭当前客户端。模型服务和聊天服务分开运行；每个 TCP 客户端由独立子进程处理，但所有子进程调用同一个 llama-server 和已加载的模型。

## 会话选择与独立界面

交互终端启动客户端后会显示会话列表，按更新时间倒序排列。列表显示编号、标题、更新时间，以及是否正在被其他客户端使用：

- `↑` / `↓` 选择，`Enter` 进入。
- `N` 新建并进入空会话；首次成功提问的首行前 24 个字符成为标题。
- `R` 刷新列表，`Q` 退出。
- 会话内输入 `/back` 返回列表，输入 `/quit` 退出客户端；只有整条消息完全匹配时才是命令。
- `PageUp` / `PageDown` 翻阅当前会话历史，上下方向键仍用于编辑输入。

界面使用终端备用屏幕，进入会话时只显示它自己的历史和输入框；退出后恢复启动前的终端画面。历史区默认显示最新内容，输入区最多占窗口高度的三分之一。回复生成期间可翻阅历史、调整窗口或按 `Ctrl+C` 退出，待回复完成后再输入或切换会话。

同一个会话一次只允许一个客户端打开；返回列表、退出或连接断开后释放占用。不同客户端可同时使用不同会话。连接的客户端共享服务端会话列表，没有账户隔离。

需要原来的普通聊天方式时：

```bash
./project/bin/client --plain 127.0.0.1 9090
```

普通模式每次连接在首次提问时创建新会话，不显示列表；管道、重定向或 `TERM=dumb` 自动采用普通模式。旧版本服务端不支持会话协议，连接旧服务端时也应使用 `--plain`。

## 客户端输入编辑

在交互终端中，客户端支持多行消息编辑：

- `Enter`：发送整条消息。
- `Ctrl+J`：在光标处换行，继续编辑同一条消息。
- `Shift+Enter`：终端发送独立按键编码时换行，支持 `CSI 13;2u`、`CSI 27;2;13~` 和 `CSI 13;2~`。
- `←` / `→`：逐字符移动；中文按完整 UTF-8 字符编辑。
- `↑` / `↓`：在显示行之间移动，尽量保留光标列。
- `Home` / `End`：移动到当前显示行的首尾。
- `Backspace` / `Delete`：删除光标前后一个字符，包括换行。
- `Ctrl+C`：退出；空输入时 `Ctrl+D` 退出，非空时删除光标后的字符。

Windows Terminal / WSL 的实际按键编码取决于终端版本和设置。如果 `Shift+Enter` 与普通 `Enter` 都发送回车字节，客户端无法区分，按两者都会发送消息；请使用 `Ctrl+J` 换行。本实现不要求修改 Windows Terminal 的设置，也不保证所有终端原生支持 `Shift+Enter`。

支持终端的括号粘贴模式：粘贴多行文本时保留换行，直到再按 `Enter` 才发送。消息最多 4095 字节（包含换行，中文通常每字 3 字节）；输入或粘贴超限会提示，发送前还会检查转义后的协议帧是否超限。多行输入超过输入区高度时，只显示光标附近的内容。全屏模式缩放窗口时重排历史和输入区；普通模式重新定位输入区，原显示保留在终端滚动历史中。

管道和文件输入仍然逐行发送；重定向输出或 `TERM=dumb` 时也使用普通逐行输入。复杂 emoji、组合字符按 Unicode 码点处理，不提供整字素簇编辑；方向键用于当前消息编辑，不浏览历史消息。

运行无需真实模型的输入、会话存储及全屏界面回归测试（需要 Python 3、GCC，并允许本地 TCP socket）：

```bash
make -C project test
```

## 会话、上下文与日志

- 每个客户端连接对应一个服务端子进程，选择会话后获取该会话的独占锁。
- 完整消息历史持久化到 `project/sessions/session_<id>.dat`。文件包含版本、编号、更新时间、标题、导入来源及按字节长度编码的消息；新建、保存使用临时文件、文件同步和原子替换。
- 显示历史不受 32 条上限限制；发送模型请求时只恢复最近完整的成功对话，继续使用 `SESSION_MAX_MESSAGES=32` 的上下文上限。
- 客户端退出或服务端重启后，重新选择会话可恢复历史并继续聊天。编号持久保留，并发创建不会分配重复编号。
- 回复成功保存后才发送完成标记。生成失败或保存失败时，当前提问和部分回复显示为 `not saved`，不进入持久历史或后续模型上下文。
- 新的可读日志写入 `project/sessions/logs/session_<id>.log`；它们用于审阅，结构化 `.dat` 文件是恢复会话的数据来源。

首次运行新服务端时，自动将 `project/logs/session_*.log` 每个文件导入为一个 `[Imported]` 会话，原日志不会被修改。导入来源记录在会话文件中，重启不会重复导入。今后新增的旧日志文件也会在启动时被导入，但已经导入的文件不会重新读取新增部分。

旧日志没有明确的会话边界或正文转义：以完整的“时间戳、空行、USER/ASSISTANT 标记”识别记录，每个文件整体导入，不尝试拆分过去重启后混写的连接。孤立提问和无法解析的文字保留用于查看，仅相邻的用户/助手完整轮次恢复进模型上下文。原有乱码不会被猜测修复；显示时将无效 UTF-8 和控制字符替换为 `?`。如果结构化存储文件损坏，列表标记为不可读，打开时返回错误；存在损坏文件时暂停自动导入，防止重复创建历史会话。

`make clean` 不删除会话或日志。备份时应同时保存 `project/sessions/` 和原来的 `project/logs/`。

## TCP 协议

客户端请求帧为 `Q <escaped-question>` 或 `QUIT`，每帧以换行结束。每次回复由 `BEGIN` 开始，服务器收到模型文本时逐块发送 `DATA <escaped-chunk>`，完成后发送 `END`。模型请求失败时返回 `ERROR <escaped-message>`。反斜杠、换行和回车在帧 payload 中分别转义为 `\\`、`\n` 和 `\r`。

会话管理扩展仍使用相同的行边界和转义规则：

| 请求 | 成功响应 |
| --- | --- |
| `SESSIONS` | `SESSIONS_BEGIN`，多条 `SESSION <id> <unix-time> <busy:0/1> <escaped-title>`，`SESSIONS_END` |
| `NEW` 或 `OPEN <id>` | `OPENED <id> <escaped-title>`，历史消息，`HISTORY_END` |
| `LEAVE` | `LEFT`，释放当前会话 |

每条历史消息使用 `MESSAGE <U/A/X> <context:0/1>` 开始，零到多个 `TEXT <escaped-chunk>` 传正文，`MESSAGE_END` 结束。`X` 表示无法解析的旧日志内容。长回复和历史正文会分块，每帧不超过现有协议长度限制。失败返回 `ERROR`，客户端可继续操作。服务端不主动发送欢迎帧；旧客户端直接发送 `Q` 时会自动创建会话。

## 验证步骤

1. `make -C project clean all` 应成功完成。
2. 按上文启动 llama-server、聊天服务器和至少两个客户端。
3. 向模型提出需要较长回答的问题，确认客户端在生成过程中逐段显示内容。
4. 在客户端 A 和 B 分别新建不同会话。在客户端 A 先输入“我叫张三”，再问“我叫什么”；客户端 B 单独问“我叫什么”。预期 A 能利用自己的历史，B 不知道 A 的信息。
5. 同时使用两个客户端提问，确认都能收到回复；退出其中一个不会关闭另一个。
6. 查看独立日志：

   ```bash
   find project/sessions/logs -maxdepth 1 -name 'session_*.log' -print
   cat project/sessions/logs/session_1.log
   ```

   日志应包含时间、`USER` 输入和完整 `ASSISTANT` 回复。

7. 关闭 llama-server 后再从客户端提问，应收到错误提示；聊天 TCP 连接仍可继续使用，模型服务恢复后可以再次提问。

8. 在会话中输入 `/back`，再进入同一会话，应只显示它自己的历史；退出客户端并重启服务端后仍可恢复。
9. 客户端 A 打开某会话时，B 选择相同会话应收到“使用中”提示；A 返回列表后 B 可以打开。
10. 在 Windows Terminal / WSL 中检查 PageUp/PageDown、中文多行输入、窗口缩放和流式回复；退出应恢复原终端画面。

## 常见问题

- `client: connect: Connection refused`：检查聊天服务器是否在运行，以及客户端端口是否和服务器一致（默认 `9090`）。
- `cannot connect to llama-server`：检查 llama-server 是否在运行，并监听 `127.0.0.1:8080`。
- 模型加载失败：确认模型路径存在；根据显存情况调整 `--n-gpu-layers`。

User messages and the interactive input editor use cyan; AI replies use the terminal default color. Colors are display-only and are not stored in sessions or logs. Piped, redirected, and TERM=dumb input remains uncolored.
