# coroutine_crash
# 查看core文件中协程协程信息的工具
## 使用环境
支持linux x86_64 gcc环境，支持其他平台扩展，但是本人没有其他环境的测试环境。

## 使用方法
1. 编译程序
```
g++ -m64 -g -O2 coroutine.cpp -o co.out
```
2. 将协程寄存器放到文本文件中，比如名字regs.txt,格式：
```
rsp:0x7f1c31a677a0 rip:0x7f1c7aa52eca rbp:0x7f1cf319ad40 rbx:0xffffffff r15:0x7f1cf319ad50 r14:0x7f1ce28e2008 r13:0x7f1cf333cf68 r12:0x7f1cf3116570
```
一行代表一个协程，寄存器名字不区分大小写，寄存器与值使用':'分隔，最少有rsp和rip(栈寄存器和指令指针寄存器)才能回溯栈。

3. 使用命令修改core文件(NOTE: 注意备份core文件)
```
./co.out corefile regs.txt
```
## 大概原理
core文件中使用PT_NOTE program 段保存线程信息，我将原有的线程信息复制出来，将寄存器设置到线程信息中。最后将原有的和复制出来的新线程信息追加到core文件尾部，然后修正PT_NOTE program header的文件偏移量和大小
