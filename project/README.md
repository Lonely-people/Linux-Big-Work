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

再开一个或多个终端启动客户端：

```bash
./project/bin/client 127.0.0.1 9090
```

输入 `/quit` 关闭当前客户端。模型服务和聊天服务分开运行；每个 TCP 客户端由独立子进程处理，但所有子进程调用同一个 llama-server 和已加载的模型。

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

支持终端的括号粘贴模式：粘贴多行文本时保留换行，直到再按 `Enter` 才发送。消息最多 4095 字节（包含换行，中文通常每字 3 字节）；输入或粘贴超限会提示，发送前还会检查转义后的协议帧是否超限。多行输入超过窗口高度时，只显示光标附近的内容；窗口缩放时重新定位输入区，原显示保留在终端滚动历史中。

管道和文件输入仍然逐行发送；重定向输出或 `TERM=dumb` 时也使用普通逐行输入。复杂 emoji、组合字符按 Unicode 码点处理，不提供整字素簇编辑；方向键用于当前消息编辑，不浏览历史消息。

运行无需模型的输入回归测试（需要 Python 3）：

```bash
make -C project test
```

## 会话、上下文与日志

- 每个客户端连接对应一个服务端子进程和一个独立 `session_t`，不同连接之间不共享聊天历史。
- 每次请求把当前 session 的 `messages` 历史发送给 llama-server。历史最多保留 32 条消息（最多 16 轮）。
- 客户端断开后，该进程内存中的会话释放；重新连接会开始新会话。
- 日志写入 `project/logs/session_<id>.log`，记录时间、用户输入和完整 assistant 回复。
- 日志用于审阅，不会在新连接时自动恢复成聊天上下文。

## TCP 协议

客户端请求帧为 `Q <escaped-question>` 或 `QUIT`，每帧以换行结束。每次回复由 `BEGIN` 开始，服务器收到模型文本时逐块发送 `DATA <escaped-chunk>`，完成后发送 `END`。模型请求失败时返回 `ERROR <escaped-message>`。反斜杠、换行和回车在帧 payload 中分别转义为 `\\`、`\n` 和 `\r`。

## 验证步骤

1. `make -C project clean all` 应成功完成。
2. 按上文启动 llama-server、聊天服务器和至少两个客户端。
3. 向模型提出需要较长回答的问题，确认客户端在生成过程中逐段显示内容。
4. 在客户端 A 先输入“我叫张三”，再问“我叫什么”；客户端 B 单独问“我叫什么”。预期 A 能利用自己的历史，B 不知道 A 的信息。
5. 同时使用两个客户端提问，确认都能收到回复；退出其中一个不会关闭另一个。
6. 查看独立日志：

   ```bash
   find project/logs -maxdepth 1 -name 'session_*.log' -print
   cat project/logs/session_1.log
   ```

   日志应包含时间、`USER` 输入和完整 `ASSISTANT` 回复。

7. 关闭 llama-server 后再从客户端提问，应收到错误提示；聊天 TCP 连接仍可继续使用，模型服务恢复后可以再次提问。

## 常见问题

- `client: connect: Connection refused`：检查聊天服务器是否在运行，以及客户端端口是否和服务器一致（默认 `9090`）。
- `cannot connect to llama-server`：检查 llama-server 是否在运行，并监听 `127.0.0.1:8080`。
- 模型加载失败：确认模型路径存在；根据显存情况调整 `--n-gpu-layers`。
