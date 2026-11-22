# wing-gcc

[English](README.md) | [简体中文](README.zh-CN.md)

从零开始用 C 语言编写的 C 编译器，作为个人项目，用于学习编译原理并打磨二进制安全方向的技术能力。

## 项目简介

`wing-gcc` 是一个自包含的 C 编译器，把 C 语言的一个实用子集翻译为机器码。前端按阶段逐步
搭建——源码管理、词法分析、语法分析与 AST 构建——后续阶段会把 AST 降低到后端。
目标是让每一层都显式、可理解：不使用 parser 生成器，不依赖外部编译器框架，只用 C和操作系统本身。

## 目标

- 用纯 C 完整实现一条真实的编译器流水线。
- 保持代码精炼、可读、**零第三方库依赖**（只链接 libc；后端落地后调用系统 `as`/`ld`，
  与 gcc 一致），目标规模约 **10000 行代码**。
- 作为面向 **二进制安全 / 系统方向实习** 的作品集项目，体现底层 C 编程、内存管理、
  数据结构与语言内部机制的能力。

## 当前进度

| 组件         | 状态                                                          |
| ------------ | ------------------------------------------------------------- |
| 源码管理     | ✅ `source.c` / `source.h`                                    |
| 词法分析器   | ✅ 数字、字符串、字符、标识符、关键字、标点、注释              |
| 抽象语法树   | 🚧 节点类型与接口已定义                                        |
| 语法分析器   | 🚧 表达式、语句、函数与递归声明                                |
| 代码生成     | ⬜ 计划中                                                      |

词法分析器已完成：它把源码缓冲区转换为 token 链表，支持数字、字符串与字符字面量
（含 `u8`/`u`/`U`/`L` 前缀）、标识符与关键字、标点符号，以及行注释和块注释。边界扫描、
位置附加、字面量解码和翻译阶段 6 的相邻字符串拼接是分离的内部阶段，公共 lexer 只返回完整 token；词法错误以 GCC 风格报告并继续分析。AST 与语法分析器已覆盖
表达式、语句、函数、递归声明符、结构/联合/枚举说明符、带作用域的 typedef-name 判定、
初始化器及局部/全局声明，包括 C11 原子/对齐/线程/函数说明符、复合字面量与泛型选择；
错误恢复位于语句与外部声明边界。暂不支持隐式返回类型（C89）
与嵌套函数定义。

## 架构

```
        +-----------+     +---------+     +----------+     +-----+
source  |  source.c | --> |  lexer/ | --> | parser/  | --> | AST | --> backend
buffer  |           |     | tokenize|     |  parse   |     |     |     (planned)
        +-----------+     +---------+     +----------+     +-----+
                              |                |
                              v                v
                       Token linked list    Node tree
```

词法分析器（`tokenize`）只返回完整 `Token` 链表，每个 token 内嵌一个 `SourceSpan`（文件、起点、
长度、行、列）及适用时的已解码字面量。边界扫描、元信息附加、字面量解码和相邻字符串拼接仍是彼此分离的内部阶段。
语法分析器消费该 token 流，构建 `ast.h` 中定义的 `Node` 树。诊断统一
为 GCC 风格，基于共享的 `DiagKind` + `SourceSpan` 核心，`fatal()` 用于内部错误并终止。一个
开放寻址的 `HashMap`（`hashmap.c`）支撑前端名字状态，显示宽度规则与源码位置一起放在
`source.h`，保证 token 列与诊断 caret 始终一致。

## 目录结构

```
wing-gcc/
├── include/              # 公共头文件，与 src/ 分层镜像
│   ├── util/             # 跨阶段基础
│   │   ├── common.h      # fatal/分配工具
│   │   ├── diagnostic.h  # 错误报告
│   │   ├── hashmap.h     # 哈希表
│   │   └── source.h      # SourceFile + SourceSpan
│   ├── frontend/         # 前端
│   │   ├── ast.h         # AST 节点定义
│   │   ├── lexer.h       # 词法分析器接口
│   │   ├── parser.h      # 语法分析器接口
│   │   └── token.h       # Token 定义
│   └── backend/          # 预留：出现第一份后端文件时创建
├── src/                  # 编译器实现，与 include/ 镜像
│   ├── main.c            # 前端驱动
│   ├── util/
│   │   ├── common.c      # fatal/分配工具
│   │   ├── diagnostic.c  # 诊断
│   │   ├── hashmap.c     # 哈希表
│   │   └── source.c      # 源码加载
│   ├── frontend/
│   │   ├── lexer/        # lexer 子系统
│   │   │   ├── lexer.c   # 完整公共流水线
│   │   │   ├── lexer_internal.h
│   │   │   ├── literal.c # 字面量校验与解码
│   │   │   ├── scan.c    # token 边界扫描
│   │   │   └── token.c   # 精确终结符清单与 token 生命周期
│   │   └── parser/       # 递归下降 parser 子系统
│   │       ├── ast.c
│   │       ├── declaration.c
│   │       ├── diagnostic.c
│   │       ├── expression.c
│   │       ├── parser.c
│   │       ├── parser_internal.h
│   │       └── statement.c
│   └── backend/          # 预留：出现第一份后端文件时创建
├── tests/            # 测试用例
│   ├── tests/        # 内部用例：valid/ 应接受，error_lex/ 应拒绝，
│   │                 # warning_lex/ 仅 gcc 告警；error_parse/ 为 parser 阶段拒绝用例；
│   │                 # warning_parse/ 预留
│   └── third_party/writing-a-c-compiler-tests/  # git submodule
├── .clang-format     # C 代码格式规则
├── .gitmodules
├── test.py           # 与 gcc 对拍的测试脚本
└── Makefile
```

## 构建

需要 C 编译器（`gcc` 或 `clang`）与 `make`。编译器本身只链接 libc；后端落地后会调用系统
汇编器（`as`）与链接器（`ld`），与 gcc 的依赖方式一致——这些工具链组件是使用系统已有的，
不会打包进本项目。

```sh
make                # 构建驱动到 bin/wing-gcc
make check          # 构建并运行所有测试驱动
make clean          # 删除 bin/
```

单个测试驱动：`make lexer_test diagnostic_test source_test hashmap_test parser_test`。

### 测试

`make check` 会构建 `wing-gcc`，运行断言式单元测试（`source_test`、`diagnostic_test`、
`hashmap_test`）、词法测试（token 分类、显示行列、整条 token 流比对）以及 parser 测试
（cursor/作用域原语、AST 构造函数、声明、表达式优先级、语句/外部声明级错误恢复）。

`./test.py` 把 wing-gcc 与真实 `gcc` 跑在同一批源码上，逐条对比**完整诊断块**
（`文件:行:列: error: 消息` 行、引用的源码行、caret 行）。每条 wing-gcc 诊断都必须在
双方找到一一对应：`match`（逐字节全同且一一对应）、`near`（位置相同或归一化后消息相同，
且 gcc 的块全部有对应）、`differ`（有块无对应或仅部分一致）、`missed`（gcc 报错、
wing-gcc 接受）、`FALSE+`（gcc 接受、wing-gcc 报错）。所有用例一律走完整前端，没有
词法专用模式。ch1–4 的 `false-positives` 必须为 0；ch5+ 与词法 `valid/` 的失败是 parser
覆盖度债务；不存在豁免名单，gcc 接受而 wing-gcc 拒绝一律计入 `FALSE+` 并使本次运行失败。
`-my` 跑 `tests/tests`，`-third-party <chapter> [dir]` 跑内置套件，
`-v` 打印双方完整输出。

## 使用

驱动运行前端：先词法分析再语法分析，并报告诊断（成功时静默）；任何错误都会返回非零状态。

```sh
./bin/wing-gcc tests/tests/basic.c     # 词法 + 语法分析；成功时静默
./bin/lexer_test tests/tests/basic.c   # 专用的 token 打印驱动
```

token 打印示例（`lexer_test`）：

```
KEYWORD   int
IDENT     main
PUNCT     (
PUNCT     )
PUNCT     {
KEYWORD   return
NUMBER    0
PUNCT     ;
PUNCT     }
EOF
```

## 路线图

- [x] 源码管理
- [x] 词法分析器
- [ ] 语法分析与 AST 构建
- [ ] 语义分析
- [ ] 中间表示与优化
- [ ] x86-64 后端代码生成
- [ ] 自举探索

## 致谢

`tests/third_party/writing-a-c-compiler-tests` 下的测试套件来自《Writing a C Compiler》
一书，仅用于本地正确性验证。
