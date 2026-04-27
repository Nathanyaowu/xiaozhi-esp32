# Agent 约束

## 项目简介

本项目 fork 自 [ZhouhaoJiang/xiaozhi-esp32](https://github.com/ZhouhaoJiang/xiaozhi-esp32)（小智 AI 聊天机器人），针对 **Waveshare ESP32-S3-RLCD-4.2** 开发板进行定制开发。

主要增强功能：
- 股票行情页（新浪财经 API，支持 A 股/港股/美股）
- 状态栏音量图标（Font Awesome 矢量字符，四级动态切换）
- BOOT 长按切换音量档位（0%→34%→67%→100% 循环 + 0.5s 蜂鸣反馈）
- 系统信息滚动显示修复
- 详细中文代码注释 + 调用链流程图

## 与上游仓库的关系

```
上游仓库: ZhouhaoJiang/xiaozhi-esp32 (origin)
     ↓ fork
我们的仓库: Nathanyaowu/xiaozhi-esp32
     ↓ 开发分支
feat/rlcd-enhancements （所有定制功能在此分支）
```

- `main` 分支：保持与上游同步，不做自定义改动
- `feat/rlcd-enhancements` 分支：所有定制开发在此进行
- 同步上游：`git fetch origin && git merge origin/main`（在 main 分支操作后再 rebase 开发分支）

## Git 操作规则

- **所有 push 由用户执行。** Agent 禁止运行 `git push`。
- **每次 `git commit` 前，必须先通知用户** 进行代码 review 和刷机验证。Agent 不得自行提交。

## 工作流程

1. Agent 修改代码，确认编译通过。
2. Agent 通知用户："改动已就绪，请 review 并刷机验证。"
3. 用户 review 代码，烧录固件到设备上验证功能。
4. 用户确认通过 → Agent 提交（或用户手动提交）。
5. 用户执行 push 到远程仓库。
