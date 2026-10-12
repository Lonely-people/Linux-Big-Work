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
