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
更多请参考[core文件中查看切换出去协程的栈信息](https://blog.csdn.net/hnwyllmm/article/details/101057074)

## 使用brpc的程序，打印bthread协程信息
使用gdb 打开core文件，执行 `source brpc_coroutine_view.gdb`，加载gdb脚本中的函数，然后执行`brpc_print_bthreads`函数，就会打印出所有可能切换出去的协程寄存器信息
示例：
```
(gdb) source /data02/wangyl11/coroutine_crash/brpc_coroutine_view.gdb 
(gdb) brpc_print_bthreads
rsp:0x7f5b2caf3a40 rip:0x7f5b283635d8 rbp:0x7f5b2caf3aa0 rbx:0x7f5b300a7800 r15:0x0 r14:0x7f5b2caf3b28 r13:0x7f5b4b6d6000 r12:0x7f5b4b770000
rsp:0x7f5b0b3e2ba0 rip:0x7f5b28363634 rbp:0x7f5b0b3e2c00 rbx:0x7f5b300a7e00 r15:0x0 r14:0x7f5b0b3e2cc8 r13:0x7f5b9fbd2e00 r12:0x7f5b4b770080
rsp:0x7f5b0b2e1ad0 rip:0x7f5b283635d8 rbp:0x7f5b0b2e1b30 rbx:0x7f5b4b722000 r15:0x0 r14:0x7f5b0b2e1bb8 r13:0x7f5b4b742000 r12:0x7f5b4b770100
warning: Error executing canned sequence of commands.
```
脚本执行的时候可能会打印最后的那个告警信息，目前不知道解决方法，不过经过测试确实会遍历所有可能出现的协程。
