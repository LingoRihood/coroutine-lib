#include <ucontext.h>
#include <stdio.h>

// 此函数用来给makecontext使用
void func1(void *arg) {
    puts("1");
    puts("11");
    puts("111");
    puts("1111");
}

void context_test() {
    //设置栈的空间
    // 这里为子上下文分配了一个栈空间（128KB）
    char stack[1024 * 128];
    //设置两个上下文
    ucontext_t child, main;
    //将此时的上下文信息保存到child中
    // getcontext(&child) 会保存当前的程序计数器（程序正在执行的位置）和堆栈状态。程序执行时，context_test 函数还没有结束，因此 getcontext 会保存当前位于 context_test 函数中的执行点。它会把这个状态保存到 child 变量中，准备将来恢复执行。
    getcontext(&child);
    //指定栈空间
    child.uc_stack.ss_sp = stack;
    //指定栈空间大小
    child.uc_stack.ss_size = sizeof(stack);
    child.uc_stack.ss_flags = 0;
    //设置后继上下文
    // child.uc_link = &main; 这一行代码的作用是设置 child 上下文的 后继上下文，即指定当 child 上下文执行完毕后，程序应该恢复执行的上下文。简单来说，它定义了上下文切换的 回退目标。
    child.uc_link = &main;
    //修改上下文让其指向func1的函数
    // makecontext() 用来设置 child 上下文，使其在恢复时执行 func1。
    makecontext(&child, (void(*)(void))func1, 0);
    //切换到child上下文，保存当前上下文到main
    swapcontext(&main, &child);
    // 程序会从 swapcontext 返回的地方继续执行
    //如果设置了后继上下文也就是uc_link指向了其他ucontext_t的结构体对象则makecontext中的函数function
    //执行完成后会返回此处打印main，如果指向的为nullptr就直接结束
    puts("main");
}

int main() {
    context_test();
    return 0;
}