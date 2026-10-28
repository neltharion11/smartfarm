# Altium Designer 工程目录

硬件设计工具确定为 **Altium Designer 24**。当前工程位于 [`test/test.PrjPcb`](test/test.PrjPcb)，原理图为 [`test/Sheet1.SchDoc`](test/Sheet1.SchDoc)，PCB 为 [`test/PCB1.PcbDoc`](test/PCB1.PcbDoc)，配套器件库保存在 `test/library/`。克隆仓库后可用 AD 24 打开工程；当前保存进度不代表已经完成设计或生产验证。

2026-10-08 用户明确分工：**正式原理图和 PCB 由用户在 Altium Designer 24 中绘制。助手负责设计说明、器件核算、引脚/连接表、布局规则、检查清单和用户提交图纸的审查，不操作绘图软件、不代画或自动生成正式 EDA 工程。到达绘图阶段时明确通知用户。**

**现在可由用户建立 MVP 原理图工程。** 按 [MVP 执行方案](../MVP_BUILD_REV_A.md)和 [必要检查清单](../COMPONENT_SELECTION_GATE.md)推进；完整工程验证不再作为绘图前置条件。实际封装在布局及下单前核对。本板仅装配电源/保护、继电器/驱动及连接器外围；ESP32、OLED、传感器和负载均通过线缆外接，见 [装配范围与接口](../BOARD_SCOPE_AND_INTERFACES_REV_A.md)。项目当前进度见 [硬件总览](../README.md)和 [PCB 设计计划](../PCB_DESIGN_PLAN.md)。
