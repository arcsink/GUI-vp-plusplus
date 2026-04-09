# VP src 变更记录

## 2026-04-08 入库 VNC 启动保护逻辑

### 变更内容

- `VNCServer::start()` 在 `vncPort == 0` 时直接返回 `false`，表示 VNC 端口被禁用时不创建 rfb screen。
- `VNCSimpleFB::updateProcess()` 在 `vncServer.start()` 返回失败时直接退出，避免继续访问不存在的 rfb screen。
- `VNCServer::isActive()` 增加 `rfbScreen != nullptr` 判断，避免未启动或已停止状态下调用 `rfbIsActive(nullptr)`。

### 变更原因

`vp/src` 下存在未入库的 VNC 防护改动。该改动让 VNC 端口为 0 或 VNC screen 尚未创建时的行为更明确，避免后续 framebuffer 更新流程访问空指针。

### 验证结果

- `git -C riscv-vp-plusplus diff --check`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
